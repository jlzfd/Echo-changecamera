# Echo Mate 项目 MIPI 摄像头链路学习文档

## 1. 学习目标

学完本文应能回答以下问题：

1. MIPI、D-PHY、CSI-2、CSI Host、CIF、ISP 分别是什么；
2. 摄像头为什么通过 I2C 配置，却通过 MIPI 传图像；
3. DTS 中的 `compatible`、`data-lanes`、`endpoint` 分别有什么作用；
4. DTB 加载后，各硬件如何匹配到 Linux 驱动；
5. 应用调用 `STREAMON` 后，数据如何从 sensor 到达 `/dev/videoN`；
6. RAW10 在 MIPI 上传多少字节，为什么应用最终只得到较小的 NV12；
7. V4L2 的四个 buffer 和 DMA-BUF fd 在整条链路中扮演什么角色；
8. MIPI 无图、花屏、丢帧和超时时，应从哪里排查。

本文基于当前项目：

- SoC：Rockchip RV1106；
- Sensor：SC3336、SC4336、SC530AI 配置；
- MIPI：2 data lanes；
- 接收链路：D-PHY → CSI-2 Host → RKCIF → RKISP；
- 应用输出：通常为 640×480 NV12；
- 后处理：V4L2 DMA-BUF → RGA → NPU/CPU → LCD。

更深入的寄存器、驱动函数和匹配细节见 [MIPI_CSI_LINUX_DRIVER_GUIDE.md](./MIPI_CSI_LINUX_DRIVER_GUIDE.md)。

## 2. 先建立整条链路

```text
                         控制面：I2C
CPU / Sensor Driver ───────────────────────> Camera Sensor
      设置模式、曝光、增益、翻转、开流            │
                                                 │ RAW10 CSI-2 packets
                                                 ▼
光线 → Sensor ADC → MIPI D-PHY TX → 板级差分线 → D-PHY RX
                                                   │ 恢复 bit/byte
                                                   ▼
                                              CSI-2 Host
                                                   │ 解包、ECC/CRC、VC/DT
                                                   ▼
                                                RKCIF
                                                   │ 接收、路由、帧同步
                                                   ▼ SDITF
                                                 RKISP
                                                   │ RAW Bayer → NV12
                                                   ▼
                                                DMA/vb2
                                                   │
                                                   ▼
                                             /dev/videoN
                                                   │ DQBUF + dma-buf fd
                                                   ▼
                                            RGA → NPU/CPU → LCD
```

这里存在两条不同路径：

- 控制路径：CPU 通过 I2C 操作 sensor 寄存器；
- 数据路径：sensor 通过 MIPI CSI-2 高速发送图像。

I2C 带宽很低，只负责配置，不传整帧图像；MIPI 带宽很高，负责连续视频数据。

## 3. 各模块到底负责什么

### 3.1 Sensor

Sensor 完成：

```text
光子 → 像素电荷 → ADC → Bayer RAW → CSI-2 packet
```

Sensor driver 通过 I2C 设置：

- 分辨率、帧率和 RAW 位宽；
- 曝光、模拟/数字增益；
- MIPI lane 数和 lane rate；
- 镜像、翻转、测试图；
- streaming on/off，通常是寄存器 `0x0100`。

Sensor 输出的 RAW Bayer 还不是普通 RGB 图片。以 SBGGR10 为例，每个像素只有一种颜色采样和 10 bit 强度，后续需要 ISP 去马赛克。

### 3.2 MIPI D-PHY

D-PHY 是物理层，负责电气和串并转换：

- clock lane：提供高速时钟；
- data lane：传输图像 bit；
- LP 状态：低功耗、空闲和状态切换；
- HS 状态：高速差分 DDR 数据传输；
- 接收端完成终端匹配、时钟恢复、lane 状态检测和串并转换。

D-PHY 不理解“这一包是 RAW10 第 20 行”。它只负责可靠地恢复字节流。

当前项目使用：

```dts
data-lanes = <1 2>;
```

表示使用两条 data lane。它不是说每次只传两个字节，而是两条串行通道并行分担高速 bit 流。

### 3.3 CSI-2

CSI-2 是协议层，负责给字节流增加图像语义。

Short packet 通常描述：

- Frame Start；
- Frame End；
- Line Start；
- Line End。

Long packet 通常承载一行像素：

```text
4-byte header | payload | 2-byte CRC
```

header 中包含：

- VC：Virtual Channel；
- DT：Data Type，例如 RAW10；
- Word Count：payload 字节数；
- ECC：保护 header。

payload 后的 CRC 用于检测像素数据传输错误。

### 3.4 CSI-2 Host

RV1106 的 CSI Host 位于 D-PHY 后面，负责：

- 配置 lane 数和接收速率；
- 解析 CSI-2 packet；
- 识别 VC 和 Data Type；
- 检查 header ECC、payload CRC；
- 将有效像素送给 CIF；
- 上报 lane、packet、FIFO 等错误。

项目 DTS 节点为 `mipi0_csi2`。

### 3.5 RKCIF

CIF 是 Rockchip Camera Interface，不属于 MIPI 标准。它是 SoC 内部摄像头接入前端，主要负责：

- 接收 CSI/DVP/LVDS 数据；
- 帧、行和虚拟通道路由；
- crop、格式对齐和 DMA 调度；
- 将 RAW 写入 DDR，或通过内部 SDITF 在线送到 ISP。

项目链路使用：

```text
rkcif_mipi_lvds → rkcif_mipi_lvds_sditf → rkisp_vir0
```

SDITF 可以理解为 CIF 到 ISP 的 SoC 内部数据接口。

### 3.6 RKISP

ISP 将 Bayer RAW 转换为应用可使用的图像，典型处理包括：

- 黑电平校正、坏点修复；
- 镜头阴影校正；
- 去马赛克；
- 白平衡、色彩校正、Gamma；
- 降噪、锐化；
- crop 和 scale；
- RAW/RGB 转 NV12/YUV；
- 生成 AE/AWB/AF 统计信息。

ISP 硬件驱动负责寄存器、DMA、中断和参数队列；rkaiq 等用户态算法结合 IQ 文件计算 3A 参数。因此“ISP 驱动”和“图像算法”不是同一个东西。

## 4. 当前项目的 DTS 图拓扑

项目配置文件：

```text
rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/
├── rv1106.dtsi
├── rv1106-echo-mate-ipc.dtsi
└── rv1106g-echo-mate.dts
```

摄像头 graph 可概括为：

```text
sc4336_out
   ↕ remote-endpoint
csi_dphy_input1
   ↓
csi_dphy_output
   ↕ remote-endpoint
mipi_csi2_input
   ↓
mipi_csi2_output
   ↕ remote-endpoint
cif_mipi_in
   ↓
mipi_lvds_sditf
   ↕ remote-endpoint
isp_in
```

Sensor 节点示意：

```dts
sc4336: sc4336@30 {
    compatible = "smartsens,sc4336";
    reg = <0x30>;
    clocks = <&cru MCLK_REF_MIPI0>;
    clock-names = "xvclk";
    pwdn-gpios = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>;

    port {
        sc4336_out: endpoint {
            remote-endpoint = <&csi_dphy_input1>;
            data-lanes = <1 2>;
        };
    };
};
```

关键属性：

| 属性 | 作用 |
|---|---|
| `compatible` | 用于设备与驱动匹配 |
| `reg = <0x30>` | Sensor I2C 地址 |
| `clocks` | Sensor 外部参考时钟 MCLK |
| `pwdn-gpios` | 上电/掉电控制 |
| `data-lanes` | MIPI lane 数和映射 |
| `remote-endpoint` | 描述媒体数据连接关系 |
| `status = "okay"` | 启用节点 |

要特别区分：

```text
compatible       → 决定“由哪个驱动管理这个设备”
remote-endpoint  → 决定“这个媒体模块的数据接到哪里”
```

`remote-endpoint` 不负责调用 `probe()`；它用于各 V4L2 subdev 注册后拼接 media graph。

项目 DTSI 中列出了多个同地址 sensor 候选配置。真实硬件运行时只能有与实际模组、固件配置相符的 sensor 正常探测和出流，应以最终 DTB、I2C chip ID 日志及 `media-ctl -p` 为准，不能仅凭源码中列出的节点断言三颗 sensor 同时工作。

## 5. 从 DTS 到驱动 probe

整条链路不是由一个“大摄像头驱动”完成，而是多个 device 分别匹配多个 driver。

```text
设备树节点              Linux device          驱动
──────────────────      ──────────────        ─────────────────────────
sc4336@30          →     i2c_client       →    drivers/media/i2c/sc4336.c
csi2_dphy_hw       →     platform_device  →    csi2-dphy-hw driver
csi2_dphy0         →     platform_device  →    csi2-dphy driver
mipi0_csi2         →     platform_device  →    cif/mipi-csi2.c
rkcif              →     platform_device  →    Rockchip CIF driver
rkisp              →     platform_device  →    Rockchip ISP driver
```

通用匹配流程：

```text
1. DTS 经过 dtc 编译为 DTB
2. U-Boot 将 DTB 地址交给内核
3. 内核解析节点并创建 platform_device / i2c_client
4. 驱动注册自己的 of_device_id 表
5. 总线比较 compatible 字符串
6. 匹配成功后调用 probe()
```

Sensor 属于 I2C bus：I2C controller 先 probe，然后枚举 `sc4336@30` 并创建 `i2c_client`。

D-PHY、CSI、CIF 和 ISP 是 SoC 内部 IP，一般属于 platform bus，由 DT 中的寄存器、IRQ、clock、reset、power-domain 等资源创建 `platform_device`。

## 6. V4L2 Media Controller 如何组装链路

各驱动 `probe()` 后通常注册：

- V4L2 subdev：sensor、D-PHY、CSI receiver、ISP 子模块；
- media entity：一个媒体功能块；
- media pad：模块输入或输出端口；
- media link：pad 之间的数据连接；
- video device：可向用户态排队 buffer 的 `/dev/videoN`。

由于设备 probe 顺序不固定，V4L2 async notifier 会等待远端 subdev：

```text
各驱动独立 probe
  → 注册 subdev/entity/pad
  → 根据 endpoint 找远端 fwnode
  → notifier 等待远端注册
  → .bound 建立关联
  → 全部到齐后 .complete
  → media graph 完整可用
```

因此：

- `probe()` 成功不代表整个摄像头链路已经完整；
- `/dev/videoN` 存在也不代表一定能成功出图；
- 最终拓扑应使用 `media-ctl -p` 检查。

## 7. 应用从 open 到 STREAMON

当前项目 V4L2 采集逻辑的核心步骤是：

```text
open("/dev/videoN")
  ↓
VIDIOC_QUERYCAP
  ↓
VIDIOC_S_FMT：请求 640×480 NV12
  ↓
VIDIOC_REQBUFS：申请 4 个 vb2 buffer
  ↓
VIDIOC_QUERYBUF：查询每块 buffer
  ↓
mmap：建立用户虚拟地址映射
  ↓
VIDIOC_EXPBUF：每块 buffer 导出 DMA-BUF fd
  ↓
VIDIOC_QBUF × 4：全部交给驱动
  ↓
VIDIOC_STREAMON
```

`STREAMON` 触发的内核过程可简化为：

```text
video_ioctl2
  → vb2_streamon
  → ISP/CIF .start_streaming
  → 设置 DMA 地址和帧中断
  → 调用上游 subdev .s_stream(1)
      → CSI Host enable
      → D-PHY enable
      → Sensor 写 mode 寄存器
      → Sensor 0x0100 = streaming
```

通常先准备下游 buffer、DMA、ISP、CIF、CSI 和 D-PHY，最后才打开 sensor，避免 sensor 已经发数据而接收端尚未就绪。

## 8. 一帧数据的完整运行过程

```text
1. Sensor 像素阵列进行曝光
2. ADC 输出 Bayer RAW10
3. Sensor 将一行 RAW10 打包成 CSI-2 long packet
4. D-PHY TX 通过 clock lane 和两条 data lane 发送
5. RV1106 D-PHY RX 恢复 clock、bit 和 byte
6. CSI Host 解析 VC、Data Type、Word Count、ECC 和 CRC
7. RKCIF 接收并路由到 SDITF
8. RKISP 完成去马赛克、颜色、降噪和缩放，生成 NV12
9. ISP/CIF DMA 将 NV12 写入一个 vb2 buffer
10. Frame End 中断到达
11. ISR 清中断、切换下一块 buffer
12. vb2_buffer_done() 将当前 buffer 放入 DONE queue
13. 阻塞的 poll()/DQBUF 被唤醒
14. VIDIOC_DQBUF 返回 index、timestamp、sequence、bytesused
15. 应用按 index 找到对应 DMA-BUF fd
16. RGA 直接读取该 fd，进行格式转换/缩放
17. 应用 QBUF，将摄像头 buffer 交回驱动循环复用
```

摄像头的 4 个 buffer 循环为：

```text
DEQUEUED → QBUF/QUEUED → ACTIVE(DMA写) → DONE → DQBUF/DEQUEUED
                 ↑                               │
                 └──────────── 再次 QBUF ────────┘
```

一个时刻摄像头可以向 B1 写入，而应用/RGA 正在处理已经 DQBUF 的 B0。所有权规则保证摄像头不会同时覆盖应用持有的 B0。

## 9. MIPI 究竟传多少数据

### 9.1 RAW10 打包

RAW10 每像素 10 bit，4 个像素打包成 5 byte：

```text
payload_bytes_per_line = ceil(width × 10 / 8)
payload_bytes_per_frame = payload_bytes_per_line × height
payload_bit_rate = width × height × fps × 10
```

以 SC4336 2560×1440 RAW10 @25 fps 为例：

```text
每行 = 2560 × 10 / 8 = 3200 byte
每帧 = 3200 × 1440 = 4,608,000 byte
每秒 = 4,608,000 × 25 = 115,200,000 byte/s
     = 921.6 Mbit/s
```

实际线上还包含：

- 每个 long packet 的 4-byte header；
- 2-byte CRC；
- Frame Start/End 等 short packet；
- embedded data；
- D-PHY SoT/EoT、状态切换和时序余量。

因此有效像素量小于实际物理链路消耗。

### 9.2 两条 lane 的容量

D-PHY 高速数据为 DDR：

```text
single_lane_bit_rate = link_freq × 2
total_capacity = link_freq × 2 × lane_count
```

如果 `link_freq = 315 MHz`、两条 lane：

```text
单 lane = 630 Mbit/s
两条 lane = 1260 Mbit/s = 157.5 MB/s
RAW10 有效负载利用率 = 921.6 / 1260 ≈ 73.1%
```

不能把链路设计到接近 100%，必须给包开销、消隐、时钟误差和硬件调度留下余量。

### 9.3 为什么应用帧只有 460800 byte

应用拿到的是 ISP 输出的 640×480 NV12：

```text
NV12 = Y plane + interleaved UV plane
size = width × height × 1.5
     = 640 × 480 × 1.5
     = 460800 byte/frame
```

两者位于不同阶段：

```text
MIPI：2560×1440 RAW10，约 4.6 MB/frame
  ↓ ISP 去马赛克、缩放、格式转换
V4L2：640×480 NV12，约 0.46 MB/frame
```

所以不能用 V4L2 输出 buffer 的大小反推 MIPI 线上带宽。

## 10. DMA-BUF 在项目中的作用

`VIDIOC_EXPBUF` 为每个 V4L2 buffer 导出 DMA-BUF fd：

```text
buffer.index 0 → fd 7
buffer.index 1 → fd 8
buffer.index 2 → fd 9
buffer.index 3 → fd 10
```

这里：

- `index` 是 V4L2 队列槽位；
- `fd` 是进程文件描述符表里的 DMA-BUF 句柄；
- fd 不是物理地址，也不是 CMA 下标；
- RGA 驱动导入 fd 后，通过 `dma_buf_attach/map_attachment` 得到 SG/DMA 地址；
- Camera 和 RGA 可以看到不同 IOVA，但访问同一份底层存储。

项目数据路径为：

```text
Camera/ISP DMA 写 NV12 V4L2 buffer
  → DQBUF
  → RGA 导入 camera dma-buf fd
  → RGA 将 NV12 转为 RGB
  → 尽早 QBUF 归还 camera buffer
  → NPU/CPU 使用独立 RGB Arena
  → RGA 转 RGB565
  → SPI LCD 双缓冲
```

这样减少了 CPU `memcpy()`，但 RGA 转换本身仍然会读源内存、写目的内存。“零拷贝”通常指避免 CPU 中间拷贝，而不是数据完全不在 DDR 中流动。

## 11. 同步、异步和 buffer 不够时会怎样

摄像头采集天然是异步流水线：

```text
Camera DMA 写 B1
RGA 读 B0
NPU 处理另一块 RGB Arena
SPI 读 LCD buffer
```

但同一块 buffer 必须遵守所有权：

```text
Camera 写完 → DQBUF → 应用/RGA 可读
应用/RGA 完成 → QBUF → Camera 才可再次写
```

如果处理速度低于摄像头帧率，DONE queue 会积压，应用可能拿到旧帧。实时机器人更适合：

- drain 已完成 buffer；
- 立即 QBUF 丢弃旧帧；
- 只处理最新一帧；
- 使用 `sequence/timestamp` 监控帧龄和跳帧。

当前同步 RGA 调用返回后再使用目标 buffer，因此不必强行引入 fence。只有真正异步提交多个硬件任务、且同一 buffer 跨设备传递时，才需要明确的 dma_fence/acquire fence/release fence。

## 12. 中断与应用唤醒

硬件完成一帧时不会直接调用应用函数：

```text
DMA frame done
  → 硬件置中断状态
  → CPU 进入 ISR
  → 驱动确认并清中断
  → 当前 vb2 buffer 标记 DONE
  → 放入 done queue
  → wake_up 等待队列
  → poll/select/epoll 或 DQBUF 被唤醒
```

中断上下文不能睡眠，也不适合执行 I2C、图像转换或 NPU 推理。ISR 只做必要的寄存器和队列操作，耗时任务交给线程、workqueue 或用户态。

## 13. 常用驱动函数与职责

| 层次 | 常见函数 | 作用 |
|---|---|---|
| Sensor | `probe()` | 获取 clock/GPIO/regulator、读 chip ID、注册 subdev |
| Sensor | `.s_stream()` | 写模式寄存器并开关出流 |
| Sensor | `.set_ctrl()` | 设置曝光、增益、翻转等 |
| D-PHY | `probe()` | 映射 PHY 资源、注册 subdev/PHY |
| D-PHY | `.s_stream()` | 配置 lane/速率/settle 并启停 PHY |
| CSI Host | `.s_stream()` | 配置 VC、DT、lane，启停 packet receiver |
| CIF/ISP | `.buf_queue()` | 把 vb2 buffer 放入硬件可用队列 |
| CIF/ISP | `.start_streaming()` | 配 DMA、中断并启动 pipeline |
| CIF/ISP | IRQ handler | 完成 buffer、切换下一 buffer |
| vb2 | `vb2_buffer_done()` | 把完成帧送入用户态 done queue |

## 14. 项目源码学习路线

建议按以下顺序阅读：

1. `rv1106-echo-mate-ipc.dtsi`：先看实际硬件拓扑；
2. `rv1106.dtsi`：看各 SoC IP 的寄存器、IRQ、clock 和 compatible；
3. `drivers/media/i2c/sc4336.c`：学习 sensor probe、mode、controls、s_stream；
4. `drivers/phy/rockchip/phy-rockchip-csi2-dphy*.c`：理解物理层；
5. `drivers/media/platform/rockchip/cif/mipi-csi2.c`：理解 CSI-2 Host；
6. `drivers/media/platform/rockchip/cif/`：理解 CIF、vb2 和中断；
7. `drivers/media/platform/rockchip/isp/`：理解 ISP video node 和 pipeline；
8. `yolov5_demo/cpp/v4l2_capture.*`：理解应用 ioctl 和 buffer 生命周期；
9. `AIcamera_c_interface.cc`：理解 DMA-BUF 如何进入 RGA/NPU/LCD。

阅读每个驱动时，都用同一套问题定位：

```text
compatible 在哪里？
probe 做了什么？
注册了 platform/I2C/V4L2 中的什么对象？
stream on 回调在哪里？
中断在哪里？
buffer 在哪里入队和完成？
remove/runtime PM 怎样释放资源？
```

## 15. 板端排查步骤

### 15.1 确认 DTB 和驱动绑定

```sh
dmesg | grep -Ei 'sc3336|sc4336|sc530ai|dphy|csi|cif|rkisp'
ls -l /sys/bus/i2c/devices/
ls -l /sys/bus/platform/devices/
```

检查：

- 是否烧录了正确 DTB；
- sensor chip ID 是否读取成功；
- clock、GPIO、regulator 是否获取成功；
- D-PHY/CSI/CIF/ISP 是否完成 probe。

### 15.2 检查 media graph

```sh
media-ctl -p
v4l2-ctl --list-devices
```

确认：

- sensor → D-PHY → CSI → CIF → ISP link 是否完整；
- pad format 是否为预期 RAW10；
- 最终 video node 是哪一个，不能长期硬编码 `/dev/video11` 而不核验设备枚举。

### 15.3 检查格式和开流

```sh
v4l2-ctl -d /dev/videoN --get-fmt-video
v4l2-ctl -d /dev/videoN --get-parm
v4l2-ctl -d /dev/videoN --stream-mmap=4 --stream-count=100
```

### 15.4 根据错误定位层次

| 现象 | 优先检查 |
|---|---|
| I2C chip ID 失败 | 供电、MCLK、PWDN/RESET、I2C 地址 |
| D-PHY 无锁定 | lane 映射、lane rate、HS settle、排线 |
| CSI ECC/CRC | 信号完整性、时钟、lane rate、sensor timing |
| CIF FIFO overflow | 后级吞吐、DDR、clock、pipeline 配置 |
| `STREAMON` 失败 | media link、格式协商、上游 `.s_stream()` |
| `DQBUF` 超时 | sensor 是否出流、IRQ、DMA 地址、CSI/CIF 错误 |
| 能出图但花屏 | RAW code、stride、width、lane、bit packing |
| 帧率不足 | sensor timing、ISP负载、DDR、buffer积压、应用耗时 |

## 16. 常见理解误区

### 误区一：MIPI 就是 D-PHY

不完全正确。摄像头常说的 MIPI 通常包括 D-PHY 物理层和 CSI-2 协议层。

### 误区二：CSI 和 CIF 是同一个模块

不是。CSI Host 解包 MIPI 协议；CIF 是 Rockchip 的摄像头接收、路由和 DMA 前端。

### 误区三：应用请求 640×480，sensor 就只发送 640×480

不一定。sensor 可能仍发送 2560×1440 RAW10，由 ISP 缩放为 640×480 NV12。

### 误区四：MIPI 上传的是 RGB 图

当前链路上传的是 Bayer RAW10。RGB/YUV 通常由 ISP 生成。

### 误区五：`remote-endpoint` 用于匹配驱动

驱动匹配依靠 `compatible`；`remote-endpoint` 用于建立媒体数据拓扑。

### 误区六：DMA-BUF fd 就是物理地址

fd 是 `dma_buf` 对象的用户态句柄。设备驱动导入后通过 SG/DMA API 获得自身可访问的 DMA 地址或 IOVA。

### 误区七：四个 V4L2 buffer 表示 MIPI 同时传四帧

不是。MIPI 是连续串行数据流；四个 buffer 是 DDR 中供 DMA 和应用轮转复用的存储槽位。

## 17. 面试式总结

本项目的摄像头由 I2C 完成寄存器配置，由两条 MIPI D-PHY data lane 传输 CSI-2 RAW10。RV1106 的 D-PHY 恢复高速差分数据，CSI Host 完成 packet 解包和 ECC/CRC 检查，RKCIF 完成接收、路由并通过 SDITF 将 RAW 送入 RKISP。ISP 进行去马赛克、颜色处理、降噪、缩放和 NV12 转换，最终通过 DMA 写入 videobuf2 管理的 V4L2 buffer。应用预先申请四块 buffer，将其分别导出为 DMA-BUF fd；一帧完成后通过 `DQBUF` 取得 index，再把对应 fd 交给 RGA/NPU 使用，处理结束后通过 `QBUF` 归还给摄像头循环复用。该设计利用硬件流水线和 DMA-BUF 减少 CPU 拷贝，并通过最新帧策略控制实时延迟。


