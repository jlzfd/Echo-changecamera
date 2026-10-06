# Linux 多媒体子系统学习文档：结合 RV1106 摄像头链路

## 1. 文档目标

本文以当前项目的 RV1106、SC3336/SC4336、MIPI CSI-2、RKCIF、RKISP 为背景，说明 Linux 摄像头多媒体子系统的核心概念、驱动注册关系、设备节点、数据流、控制流和常用排障方法。

重点回答以下问题：

- Sensor 为什么既是 I2C 设备，又是 V4L2 sub-device；
- Media Controller、V4L2 core、VB2 分别负责什么；
- entity、pad、link 如何组成摄像头拓扑；
- `/dev/mediaX`、`/dev/v4l-subdevX`、`/dev/videoX` 有什么区别；
- DTS、驱动 probe、异步匹配、视频节点注册之间是什么关系；
- 应用执行 `REQBUFS/QBUF/STREAMON/DQBUF` 后，数据怎样到达内存；
- RKAIQ、V4L2 Controls 和 Sensor I2C 寄存器之间如何衔接。

---

## 2. Linux Media 子系统全景

Linux Media 子系统覆盖摄像头、视频编解码器、电视接收、遥控器等设备。摄像头项目最常接触下面几个组成部分。

| 组件 | 核心职责 |
| --- | --- |
| Linux Device Model | 描述 device、driver、bus，完成 probe/remove |
| I2C Core | Sensor 等 I2C 外设的实例化、匹配和寄存器通信 |
| V4L2 Core | 统一视频设备注册、ioctl 框架、事件和文件句柄管理 |
| V4L2 Sub-device | 表示 Sensor、CSI、ISP 等不能单独向应用交付普通视频帧的子模块 |
| Media Controller | 使用 entity、pad、link 描述复杂媒体拓扑 |
| V4L2 Async | 解决 Sensor、CSI、CIF、ISP probe 顺序不确定的问题 |
| V4L2 Controls | 统一曝光、增益、VBLANK、翻转等控制项 |
| Videobuf2（VB2） | 管理视频 buffer 的申请、排队、完成、mmap、USERPTR、DMABUF |
| 具体硬件驱动 | 配置寄存器、DMA、中断、时钟、电源及错误处理 |
| RKAIQ | 用户空间 ISP/3A 算法框架，根据统计结果控制 Sensor 和 ISP |

一条典型的 RV1106 摄像头链路为：

```text
SC3336/SC4336 Sensor
        │ RAW Bayer，MIPI CSI-2 packets
        ▼
      D-PHY
        │ 串并转换、时钟恢复、Lane接收
        ▼
 CSI-2 Receiver
        │ 解析VC、Data Type、ECC/CRC、帧边界
        ▼
      RKCIF
        │ 接收、路由、裁剪，也可直接DMA输出RAW
        ▼
      RKISP
        │ 去黑电平、去马赛克、AWB、降噪、Gamma等
        ▼
 MainPath/SelfPath video node
        │ DMA写入VB2 buffer
        ▼
   /dev/videoX → 应用
```

这里存在两类“流”：

```text
控制流：应用/RKAIQ → ioctl → V4L2 → 驱动 → I2C/寄存器
数据流：Sensor → MIPI → CSI/CIF/ISP → DMA → 内存buffer → 应用
```

不要把控制流和像素数据流混为一谈。Sensor 的曝光通过 I2C 控制，但图像不会通过 I2C 传输。

---

## 3. 五个最重要的内核对象

### 3.1 `struct media_device`

`media_device` 表示一张由 Media Controller 管理的媒体拓扑。注册后通常对应：

```text
/dev/media0
/dev/media1
```

RKCIF 和 RKISP 可以分别拥有自己的 `media_device`。当前代码中可看到：

```c
v4l2_device_register(dev, &xxx->v4l2_dev);
media_device_init(&xxx->media_dev);
media_device_register(&xxx->media_dev);
```

项目位置：

- `sysdrv/source/kernel/drivers/media/platform/rockchip/cif/dev.c`
- `sysdrv/source/kernel/drivers/media/platform/rockchip/isp/dev.c`

`/dev/mediaX` 用于查询和配置拓扑，不用于 `DQBUF` 获取普通图像帧。

### 3.2 `struct v4l2_device`

`v4l2_device` 是一组 V4L2 对象在内核中的管理容器，可挂接多个 subdev 和 video device。它不等同于 `/dev/videoX`，通常也不会单独产生一个供采集使用的设备节点。

可以这样理解：

```text
media_device：管理“图”
v4l2_device：管理这套设备中的V4L2对象
```

### 3.3 `struct v4l2_subdev`

`v4l2_subdev` 表示媒体流水线中的子模块，例如：

- Camera Sensor；
- CSI-2 Receiver；
- ISP 核心；
- 镜头 VCM；
- Flash；
- Serializer/Deserializer。

subdev 通过 `v4l2_subdev_ops` 暴露操作：

```c
struct v4l2_subdev_ops {
    const struct v4l2_subdev_core_ops  *core;
    const struct v4l2_subdev_video_ops *video;
    const struct v4l2_subdev_pad_ops   *pad;
};
```

常见职责：

- `video->s_stream()`：开始或停止硬件数据流；
- `pad->get_fmt/set_fmt()`：查询或配置 pad 上的媒体总线格式；
- `pad->enum_mbus_code()`：枚举 RAW10、RAW12 等格式；
- `core` 或 control handler：电源、私有 ioctl、曝光和增益控制。

subdev 在条件满足时可暴露为 `/dev/v4l-subdevX`，但它通常不是应用取得图像帧的节点。

### 3.4 `struct video_device`

`video_device` 是面向用户空间的 V4L2 节点，注册后通常形成 `/dev/videoX`：

```c
video_register_device(vdev, VFL_TYPE_VIDEO, -1);
```

具体的 CIF/ISP 驱动负责创建并初始化 `video_device`，V4L2 core 负责：

- 分配主次设备号；
- 注册字符设备入口；
- 创建设备模型对象；
- 将 ioctl 分发到驱动；
- 最终通过 devtmpfs/udev 呈现 `/dev/videoX`。

因此准确表述是：

> CIF/ISP 驱动发起 `video_device` 注册，V4L2 core 完成通用字符设备注册并形成 `/dev/videoX`。

当前项目示例：

- `drivers/media/platform/rockchip/cif/capture.c`
- `drivers/media/platform/rockchip/isp/capture.c`

### 3.5 `struct vb2_queue`

`vb2_queue` 表示一个视频节点的 buffer 队列。VB2 负责管理 buffer 状态，而硬件驱动负责把 DMA 地址交给硬件，并在中断中报告 buffer 完成。

典型状态循环：

```text
用户空闲
   │ QBUF
   ▼
已排队给驱动
   │ 硬件取用
   ▼
DMA进行中
   │ 帧完成中断
   ▼
DONE
   │ DQBUF
   ▼
交给应用处理
   │ 再次QBUF
   └──────────────→ 循环
```

---

## 4. Media Controller：entity、pad、link

Media Controller 把摄像头流水线描述为有向图。

### 4.1 Entity

Entity 是图中的功能模块，对应 `struct media_entity`。

它可能是物理硬件：

- SC3336 Sensor；
- CSI Receiver；
- ISP。

也可能是逻辑功能：

- ISP MainPath；
- ISP SelfPath；
- Statistics 节点；
- Parameters 节点。

一个物理 ISP 可以对应多个 entity，所以 entity 不等于“一颗芯片”。

### 4.2 Pad

Pad 是 entity 的数据接口，对应 `struct media_pad`：

```text
Sink Pad   ：数据进入entity
Source Pad ：数据离开entity
```

Sensor 通常只有一个 Source Pad；CSI 和 ISP 通常同时拥有 Sink 和 Source Pad。

### 4.3 Link

Link 是 Source Pad 到 Sink Pad 的有向连接，对应 `struct media_link`：

```text
Entity A:Source Pad ─────→ Entity B:Sink Pad
```

驱动通常通过下面的接口创建：

```c
media_create_pad_link(source_entity, source_pad,
                      sink_entity, sink_pad, flags);
```

重要 Link 标志：

| 标志 | 含义 |
| --- | --- |
| `MEDIA_LNK_FL_ENABLED` | 当前启用该路径 |
| `MEDIA_LNK_FL_IMMUTABLE` | 固定硬件连接，用户不可修改 |

### 4.4 谁把图连接起来

Media Controller core 不会仅凭 entity 名称自动猜连接。完整分工是：

```text
DTS endpoint/remote-endpoint
    → 描述板级物理连接

V4L2 Async
    → 等待两端驱动都probe成功并完成匹配

CIF/ISP等主驱动
    → 根据匹配结果调用media_create_pad_link()

Media Controller
    → 保存、校验、枚举和管理最终拓扑
```

D-PHY 是否显示为独立 entity 取决于具体 Rockchip 驱动实现。它也可能只注册到 Linux PHY Framework，或者被整合在 CSI/CIF 驱动中。拓扑里没显示 D-PHY，不代表物理数据没有经过 D-PHY。

---

## 5. 三类设备节点必须分清

| 节点 | 内核对象 | 主要用途 | 能否获取普通视频帧 |
| --- | --- | --- | --- |
| `/dev/mediaX` | `media_device` | 查询/配置 entity、pad、link | 否 |
| `/dev/v4l-subdevX` | `v4l2_subdev` | 配置 Sensor、CSI、ISP 子模块 | 通常否 |
| `/dev/videoX` | `video_device` | 图像、统计、参数或 M2M 队列 | 视节点类型而定 |

另外：

```text
/dev/i2c-X
```

由通用 `i2c-dev` 驱动创建，用于直接访问 I2C 总线。它不是 Media Controller 节点。正常摄像头运行时不要从应用绕过 Sensor 驱动随意写 `/dev/i2c-X`，否则可能与 RKAIQ/V4L2 控制产生竞争。

Rockchip 系统中，并不是所有 `/dev/videoX` 都输出图像。它也可能是：

- ISP statistics metadata；
- ISP parameters；
- RAW read/write；
- CIF scale stream；
- ISP main/self path。

必须使用 `media-ctl -p` 和 `v4l2-ctl -D` 确认身份，不能依赖固定编号。

---

## 6. Sensor 为什么同时属于 I2C 和 V4L2

SC3336 驱动不是“两份独立驱动”，而是同一个驱动接入两个框架：

```text
同一个SC3336驱动
    ├─ 注册struct i2c_driver
    │    └─ 负责DTS匹配、probe和寄存器通信
    │
    └─ 在probe中初始化struct v4l2_subdev
         └─ 负责格式、stream、controls和Media拓扑
```

### 6.1 I2C 侧注册

```c
module_i2c_driver(sc3336_i2c_driver);
```

主要流程：

```text
DTS camera@30
    → I2C core创建i2c_client
    → compatible匹配i2c_driver
    → sc3336_probe(client)
    → 驱动获得总线和从地址
    → 通过i2c_transfer等接口访问Sensor寄存器
```

`i2c_driver` 是总线驱动，不等于驱动自行注册了传统 `/dev/sc3336` 字符设备。

### 6.2 V4L2 侧注册

在同一个 probe 内部，Sensor 驱动会：

```text
初始化v4l2_subdev
    → 初始化media_entity和source pad
    → 初始化v4l2_ctrl_handler
    → 异步注册sensor subdev
```

当前 SC3336 驱动中存在：

```c
media_entity_pads_init(&sd->entity, 1, &sc3336->pad);
v4l2_async_register_subdev_sensor_common(sd);
```

这样同一个对象既知道“怎样用 I2C 控制 Sensor”，又知道“在摄像头 pipeline 中怎样表现为图像源”。

---

## 7. DTS 到完整 Media Graph 的注册流程

### 步骤 1：DTS 描述设备及连接

Sensor 节点描述控制接口与输出端点：

```dts
camera@30 {
    compatible = "smartsens,sc3336";
    reg = <0x30>;

    port {
        sensor_out: endpoint {
            remote-endpoint = <&csi_in>;
            data-lanes = <1 2>;
        };
    };
};
```

其中：

- `compatible` 用于驱动匹配；
- `reg` 是 I2C 从地址；
- `data-lanes` 描述 MIPI Lane；
- `endpoint/remote-endpoint` 描述图像数据连接。

### 步骤 2：各总线独立完成驱动匹配

```text
Sensor：I2C bus匹配i2c_driver
CSI/CIF/ISP：platform bus匹配platform_driver
D-PHY：通常由platform/PHY framework管理
```

### 步骤 3：各驱动注册自己的媒体对象

```text
Sensor → v4l2_subdev + entity + source pad
CSI    → v4l2_subdev + entity + sink/source pads
CIF    → subdev/entity + video_device + vb2_queue
ISP    → ISP subdev/entity + 多个video_device
```

### 步骤 4：V4L2 Async 完成跨驱动匹配

由于 probe 顺序不固定，主驱动通过 notifier 等待远端 subdev。Sensor 与接收端都就绪后，触发 `bound`/`complete` 等回调。

### 步骤 5：创建 Media Link

CIF/ISP 驱动根据 endpoint 及内部硬件结构调用 `media_create_pad_link()`，形成拓扑。

### 步骤 6：注册设备节点

```text
media_device_register()          → /dev/mediaX
v4l2_device_register_subdev_nodes() → /dev/v4l-subdevX（符合条件时）
video_register_device()          → /dev/videoX
```

---

## 8. 格式为什么有两套：Media Bus Format 与 Pixel Format

### 8.1 Media Bus Format

Media Bus Format 描述硬件模块之间在线上传输的数据格式，例如：

```text
MEDIA_BUS_FMT_SBGGR10_1X10
MEDIA_BUS_FMT_SRGGB10_1X10
```

它常用于 subdev pad，表达：

- Bayer 排列；
- 位宽；
- 总线组织形式；
- pad 上的宽高、crop 等。

注意：MIPI CSI-2 RAW10 的物理传输是 packet 化且按规则打包的，`1X10` 是 V4L2 的媒体总线语义，不代表每个像素在链路上简单占一个 16 位字。

### 8.2 V4L2 Pixel Format

Pixel Format 描述内存中的 buffer 布局，例如：

```text
V4L2_PIX_FMT_NV12
V4L2_PIX_FMT_NV21
V4L2_PIX_FMT_SBGGR10
```

它决定应用如何解释 DMA buffer：

- plane 数量；
- `bytesperline`；
- `sizeimage`；
- 色彩格式和存储布局。

因此：

```text
subdev pad之间：media bus format
video node内存端：pixel format
```

格式需要沿 pipeline 兼容，但不会因为应用只对末端执行一次 `S_FMT`，所有中间模块就必然完全自动正确配置。Rockchip 上层框架、驱动默认配置或 RKAIQ 可能帮助完成部分传播，排障时仍应逐个 pad 核对。

---

## 9. V4L2 Controls：曝光如何写到 Sensor

Sensor 驱动通过 `v4l2_ctrl_handler` 注册控制项：

```text
V4L2_CID_EXPOSURE
V4L2_CID_ANALOGUE_GAIN
V4L2_CID_VBLANK
V4L2_CID_HFLIP
V4L2_CID_VFLIP
V4L2_CID_TEST_PATTERN
```

以 SC3336 为例：

| Control | 驱动动作 |
| --- | --- |
| Exposure | 写 `0x3e00/0x3e01/0x3e02` |
| Analogue Gain | 组合设置模拟、数字粗增益、数字细增益 |
| VBLANK | 计算 `VTS = height + vblank`，写 `0x320e/0x320f` |

调用链为：

```text
RKAIQ AE或用户空间控制
    → V4L2 control ioctl
    → v4l2_ctrl_handler
    → sc3336_set_ctrl()
    → sc3336_write_reg()
    → I2C Core
    → Sensor寄存器
```

RKAIQ 负责决定“参数应该是多少”，Sensor 驱动负责“怎样换算并写入寄存器”。当自动 AE 正在运行时，手动写入的曝光值可能很快被算法覆盖。

---

## 10. VB2 与应用采集流程

典型应用调用顺序：

```text
open(/dev/videoX)
    ↓
VIDIOC_QUERYCAP
    ↓
VIDIOC_ENUM_FMT / VIDIOC_S_FMT
    ↓
VIDIOC_REQBUFS
    ↓
VIDIOC_QUERYBUF
    ↓
mmap，或使用DMABUF方式准备buffer
    ↓
VIDIOC_QBUF（把buffer所有权交给驱动）
    ↓
VIDIOC_STREAMON
    ↓
poll/select/epoll等待节点就绪
    ↓
VIDIOC_DQBUF（取回已完成buffer）
    ↓
应用/RGA/NPU处理
    ↓
VIDIOC_QBUF（归还buffer）
```

这里的 `poll()` 只负责等待文件描述符变为可读，不搬运图像，也不替代 `DQBUF`。

### 10.1 `STREAMON` 后发生什么

概念流程如下：

```text
应用VIDIOC_STREAMON
    → VB2检查已排队buffer数量
    → capture驱动配置DMA地址
    → 启动CIF/ISP
    → 沿pipeline启动上游subdev
    → Sensor s_stream(1)
    → Sensor开始输出MIPI帧
    → CIF/ISP DMA写入buffer
    → 帧结束中断
    → 驱动vb2_buffer_done(..., DONE)
    → poll唤醒
    → 应用DQBUF
```

### 10.2 所有权同步

不使用显式 DMA fence 时，V4L2 队列状态就是主要同步协议：

```text
QBUF之后：驱动/硬件拥有buffer，CPU不能随意修改
DQBUF之后：应用拥有buffer，硬件不应再写入
再次QBUF：应用结束使用并归还
```

这是一种隐式、按队列状态同步的所有权模型。DMA cache 一致性则由 DMA API、VB2 内存后端和设备一致性属性共同处理，不能仅靠“有一个 fd”自动保证。

---

## 11. MMAP、USERPTR、DMABUF

V4L2 常见内存模型：

| 模式 | 内存主要由谁准备 | 特点 |
| --- | --- | --- |
| MMAP | 驱动/VB2 | 最常见，应用 mmap 驱动分配的 buffer |
| USERPTR | 应用 | 驱动 pin 用户页，限制和一致性处理更复杂 |
| DMABUF | exporter 分配，fd 共享 | 适合 V4L2、RGA、NPU、显示间共享 |

DMA-BUF fd 是内核对象句柄，不是物理地址，也不是“物理地址索引表中的编号”。导入设备通过 DMA-BUF attachment 和 DMA mapping 获得适合该设备访问的 DMA 地址或 scatter-gather table。

概念流程：

```text
V4L2/VB2 exporter分配buffer
    → 导出DMA-BUF fd
    → fd传给RGA/NPU/DRM等设备
    → importer执行attach/map_attachment
    → 获得面向本设备的sg_table/DMA地址
    → 设备执行DMA
```

是否物理连续取决于 allocator、VB2 memory backend、IOMMU 和硬件要求；不能根据“有 dma-buf fd”推断底层一定是 CMA 连续内存。

---

## 12. RKAIQ 在 Media 子系统中的位置

RKAIQ 不是内核驱动，而是 Rockchip 用户空间 ISP/3A 算法框架。

```text
ISP输出亮度、颜色等统计
        ↓
RKAIQ执行AE/AWB等算法
        ↓
一部分参数配置ISP
        ↓
曝光/增益等通过V4L2 subdev controls配置Sensor
        ↓
下一帧或后续帧生效
```

例如 AE 闭环：

```text
图像偏暗
  → ISP统计亮度偏低
  → RKAIQ增加曝光时间或增益
  → V4L2_CID_EXPOSURE/ANALOGUE_GAIN
  → Sensor驱动I2C写寄存器
  → 新曝光帧到达ISP
  → 再次统计并调整
```

IQ 文件是针对具体 Sensor、镜头和模组的调参数据库，RKAIQ 是读取统计并执行策略的框架。

---

## 13. 驱动端关键注册接口速查

| 接口 | 作用 |
| --- | --- |
| `module_i2c_driver()` | 注册 Sensor I2C 驱动 |
| `module_platform_driver()` | 注册 CSI/CIF/ISP 等平台驱动 |
| `v4l2_device_register()` | 注册 V4L2 管理容器 |
| `media_device_init()` | 初始化 Media Controller 设备 |
| `media_device_register()` | 注册 `/dev/mediaX` 对应对象 |
| `v4l2_i2c_subdev_init()` | 将 I2C Sensor 初始化为 V4L2 subdev |
| `media_entity_pads_init()` | 为 entity 初始化 pads |
| `v4l2_async_register_subdev_sensor_common()` | 异步注册 Sensor subdev |
| `v4l2_device_register_subdev()` | 将 subdev 加入 v4l2_device |
| `v4l2_device_register_subdev_nodes()` | 为符合条件的 subdev 创建设备节点 |
| `media_create_pad_link()` | 建立 Source Pad 到 Sink Pad 的 Link |
| `video_register_device()` | 注册 video_device，由 V4L2 core 形成 `/dev/videoX` |
| `vb2_queue_init()` | 初始化视频 buffer 队列 |
| `v4l2_ctrl_handler_init()` | 初始化 controls |

---

## 14. 当前项目源码对应关系

### Sensor

```text
rv1106-sdk/sysdrv/source/kernel/drivers/media/i2c/sc3336.c
rv1106-sdk/sysdrv/source/kernel/drivers/media/i2c/sc4336.c
```

可以观察：

- `i2c_driver` 和 `module_i2c_driver()`；
- `probe()`；
- `v4l2_subdev_ops`；
- `v4l2_ctrl_handler`；
- `media_entity_pads_init()`；
- `v4l2_async_register_subdev_sensor_common()`；
- `s_stream()` 与寄存器表。

### RKCIF

```text
rv1106-sdk/sysdrv/source/kernel/drivers/media/platform/rockchip/cif/
```

重点文件：

- `dev.c`：CIF 主设备、Media/V4L2 注册、异步绑定和 links；
- `capture.c`：capture video node、VB2、ioctl、DMA/stream；
- `mipi-csi2.c`：CSI-2 subdev、pads、Sensor link；
- `cif-scale.c`：缩放输出节点。

### RKISP

```text
rv1106-sdk/sysdrv/source/kernel/drivers/media/platform/rockchip/isp/
rv1106-sdk/sysdrv/source/kernel/drivers/media/platform/rockchip/isp1/
```

重点文件：

- `dev.c`：Media/V4L2 主设备和 links；
- `rkisp.c`/`rkisp1.c`：ISP subdev；
- `capture.c`：MainPath/SelfPath 等视频采集节点；
- `isp_stats.c`：统计节点；
- `isp_params.c`：参数节点；
- `csi.c`：ISP侧 CSI 接口。

不同 Rockchip BSP 版本可能同时保留多代驱动目录，实际编译使用哪一套需要结合 Kconfig、Makefile、DTS compatible 和构建配置确认。

---

## 15. 上板查看和排障命令

### 15.1 枚举设备节点

```bash
ls -l /dev/media* /dev/v4l-subdev* /dev/video*
```

### 15.2 查看整张拓扑

```bash
media-ctl -p -d /dev/media0
media-ctl -p -d /dev/media1
```

重点检查：

- Sensor entity 是否出现；
- link 是否为 `ENABLED`；
- Sensor 到 CSI/CIF/ISP 的方向是否正确；
- 各 pad 的格式、宽高是否一致；
- `/dev/videoX` 对应哪个 entity。

### 15.3 查询视频节点身份

```bash
v4l2-ctl -d /dev/video0 -D
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
```

### 15.4 查询 Sensor controls

```bash
v4l2-ctl -d /dev/v4l-subdev0 --list-ctrls
v4l2-ctl -d /dev/v4l-subdev0 --all
```

不要先假设 subdev 编号，先通过 `media-ctl -p` 找到 Sensor 对应节点。

### 15.5 采集测试

```bash
v4l2-ctl -d /dev/videoX \
  --set-fmt-video=width=640,height=480,pixelformat=NV12 \
  --stream-mmap=4 \
  --stream-count=100 \
  --stream-to=/tmp/capture.nv12
```

格式和分辨率必须以目标节点实际支持的能力为准。

### 15.6 查看内核日志

```bash
dmesg | grep -Ei 'sc3336|sc4336|mipi|csi|cif|isp|v4l2|media'
```

重点观察：

- Sensor ID 是否读取成功；
- endpoint/notifier 是否绑定成功；
- CSI ECC/CRC；
- FIFO overflow；
- frame start/end；
- CIF/ISP 丢帧和 DMA 错误。

CRC/ECC/overflow 不一定默认持续打印。是否输出取决于驱动日志级别、错误中断开关、动态调试和厂商实现。高频错误往往只计数或限速打印，必要时应检查 debugfs、驱动统计接口或开启 dynamic debug。

---

## 16. 常见问题的分层定位

### 16.1 完全没有 Sensor

优先检查：

```text
DTS status/compatible/reg
→ I2C控制器是否启用
→ 电源、时钟、reset/pwdn GPIO
→ probe是否执行
→ Sensor ID是否正确
```

### 16.2 Sensor 有，但 Media Graph 断开

优先检查：

```text
port/endpoint/remote-endpoint
→ endpoint是否双向对应
→ V4L2 async notifier日志
→ pad编号和驱动期望是否一致
→ 对端驱动是否probe成功
```

### 16.3 Graph 完整，但 STREAMON 失败

优先检查：

```text
pad格式与尺寸
→ Lane数量/速率
→ Sensor mode和link frequency
→ 已QBUF数量
→ 时钟、电源域、runtime PM
→ 上游s_stream返回值
```

### 16.4 能出流但没有帧或频繁超时

优先检查：

```text
MIPI时钟/Lane mapping
→ CSI Data Type和Virtual Channel
→ RAW10打包
→ ECC/CRC/overflow
→ CIF帧中断和DMA地址
→ buffer是否及时QBUF
```

### 16.5 图像过暗或色彩异常

链路正确不代表图像质量一定正确。继续检查：

```text
RKAIQ是否启动
→ IQ文件是否匹配Sensor和镜头
→ AE曝光/增益上限
→ VTS/VBLANK与帧率限制
→ Bayer顺序
→ AWB/CCM/Gamma/黑电平
```

---

## 17. 容易混淆的结论

### 误区 1：注册一个 subdev 就会自动创建整条链路

不准确。subdev 只注册本模块，DTS 描述连接，V4L2 Async 完成匹配，主驱动调用 Media Controller 接口创建 link。

### 误区 2：V4L2 core 会自动发现硬件并创建所有 `/dev/videoX`

不准确。具体 CIF/ISP 驱动必须初始化 `video_device` 并调用 `video_register_device()`，V4L2 core 才完成通用注册。

### 误区 3：每个硬件模块都有 `/dev/videoX`

不准确。Sensor、CSI 等一般是 subdev；能够向用户交付 buffer 的采集/输出路径才通常注册 video node。

### 误区 4：每个 subdev 都一定有 `/dev/v4l-subdevX`

不准确。还受内核配置、subdev 标志以及主 V4L2 设备是否注册 subdev nodes 等条件影响。

### 误区 5：DMA-BUF fd 就是物理地址

错误。fd 是进程中的句柄。驱动通过 DMA-BUF framework 导入对象、建立 attachment，并映射为该设备可访问的 DMA 地址/SG 表。

### 误区 6：Media Graph 就是像素数据本身

错误。Media Graph 是拓扑和控制模型；像素数据实际由 MIPI 硬件链路与 DMA 搬运。

---

## 18. 最终记忆框架

```text
Linux设备模型
    负责“哪个驱动匹配哪个硬件”

I2C Core
    负责“怎样访问Sensor寄存器”

V4L2 Sub-device
    负责“Sensor/CSI/ISP模块怎样被控制”

Media Controller
    负责“这些模块怎样组成有向图”

V4L2 Core + video_device
    负责“怎样向用户暴露/dev/videoX和统一ioctl”

Videobuf2
    负责“视频buffer怎样排队、完成和共享”

CIF/ISP硬件驱动
    负责“怎样配置硬件、DMA和中断”

RKAIQ
    负责“根据统计结果算出曝光、白平衡和ISP参数”
```

对当前项目可以浓缩成一句话：

> SC3336/SC4336 通过 I2C 驱动完成寄存器控制，并注册为 V4L2 subdev/entity；CSI、CIF、ISP 各自注册媒体对象，CIF/ISP 根据 DTS 和异步匹配结果建立 links；采集路径注册 video_device，V4L2 core 形成 `/dev/videoX`，VB2 管理 buffer，硬件通过 DMA 把处理后的帧送到应用。
