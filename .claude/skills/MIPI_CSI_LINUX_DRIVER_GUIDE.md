# RV1106 MIPI CSI-2 数据流与 Linux 驱动指南

本文是 `CAMERA_AUDIO_DRIVER_STACK.md` 的摄像头底层补充，基于当前 Echo Mate DTS 和 SC3336/SC4336/SC530AI 驱动，重点解释：MIPI 究竟传多少数据、数据怎样流动，以及 D-PHY、CSI-2、CSI Host、CIF、ISP、V4L2 和 Linux 驱动模型分别负责什么。

Camera Sensor输出:     模拟光强 → MIPI差分电压信号 (模拟)
↓
D-PHY转换:            模拟差分信号 → 数字字节流 (数字)
↓
MIPI CSI-2 Host解析:  字节流 → MIPI数据包 (协议层)
↓
VICAP格式转换:        MIPI数据包 → RAW像素数组 (Bayer格式)
↓
ISP图像处理:          RAW Bayer → YUV/RGB彩色图像
↓
Memory存储:           YUV图像 → 内存Buffer地址
↓
应用程序:             读取内存 → 显示/编码/分析

## 1. 先回答：MIPI 传多少字节

答案取决于 sensor 当前 mode，而不是应用请求的最终图像尺寸。

当前 sensor 经 MIPI 输出的是 RAW10。RAW10 每个像素有 10 bit，CSI-2 按 4 个像素打包成 5 byte：

```text
4 pixels × 10 bit = 40 bit = 5 byte
payload bytes/line = ceil(width × 10 / 8)
active payload/frame = payload bytes/line × height
active payload/s = active payload/frame × fps
```

### 1.1 SC4336：当前最典型的例子

驱动 mode：

```text
2560 × 1440
25 fps
SBGGR10 / RAW10
2 data lanes
link_freq = 315 MHz
```

有效像素负载：

```text
每行：2560 × 10 / 8 = 3,200 byte
每帧：3,200 × 1440 = 4,608,000 byte
每秒：4,608,000 × 25 = 115,200,000 byte/s
                              = 921.6 Mbit/s
```

若按“每个有效行一个 CSI-2 long packet”估算，每个 long packet 还有 4-byte header 和 2-byte CRC：

```text
行包开销：1440 × 6 = 8,640 byte/frame
Frame Start + Frame End short packets：2 × 4 = 8 byte/frame
总计约：4,608,000 + 8,640 + 8
      = 4,616,648 byte/frame
      = 115,416,200 byte/s @ 25 fps
```

这个值仍未计入可选 embedded-data packet、PHY SoT/EoT、lane 状态切换等开销；sensor 的具体 packet 组织应以芯片手册或逻辑分析结果为准。

Linux 的 `link_freq` 是高速差分时钟频率。D-PHY data lane 使用 DDR，所以：

```text
单 lane bit rate = link_freq × 2
                 = 315 MHz × 2 = 630 Mbit/s

2-lane 总线速容量 = 630 × 2 = 1,260 Mbit/s
                  = 157.5 MB/s
```

所以有效 RAW10 负载占线速约：

```text
921.6 / 1260 ≈ 73.1%
```

余量并非浪费，它覆盖水平/垂直消隐对应的传输时间、CSI-2 包开销、PHY 切换和时钟误差。驱动计算的 pixel rate 正是：

```c
pixel_rate = link_freq * 2 * lanes / bits_per_sample
           = 315M * 2 * 2 / 10
           = 126 Mpixel/s
```

### 1.2 当前三种 sensor 的估算

| Sensor mode | RAW 有效负载/帧 | 含基本 CSI 行包开销/帧 | 有效负载速率 | 配置 lane 线速容量 |
|---|---:|---:|---:|---:|
| SC3336 2304×1296 RAW10 @25 | 3,732,480 B | 约 3,740,264 B | 93.312 MB/s | 2×506.25 Mb/s = 126.56 MB/s |
| SC3336 2304×1296 RAW10 @30 | 3,732,480 B | 约 3,740,264 B | 111.974 MB/s | 2×510 Mb/s = 127.5 MB/s |
| SC4336 2560×1440 RAW10 @25 | 4,608,000 B | 约 4,616,648 B | 115.2 MB/s | 2×630 Mb/s = 157.5 MB/s |
| SC530AI 2880×1620 RAW10 @30、2 lane | 5,832,000 B | 约 5,841,728 B | 174.96 MB/s | 2×792 Mb/s = 198 MB/s |

“含基本 CSI 行包开销”假定每行一个 long packet，只加 6 byte/line 和 FS/FE 两个 short packet，不是示波器上的精确总量。

### 1.3 为什么应用的帧只有 460,800 byte

当前应用从 ISP 节点请求 640×480 NV12：

```text
NV12 bytes/frame = width × height × 1.5
                 = 640 × 480 × 1.5
                 = 460,800 byte
```

它与 MIPI 上的约 4.6 MB RAW10/帧不是同一位置的数据：

```text
MIPI 输入：2560×1440 RAW10，约 4.6 MB/frame
  ↓ ISP 去马赛克、颜色处理、缩放、YUV 转换
V4L2 输出：640×480 NV12，约 0.46 MB/frame
```

不能用 `/dev/video11` 的 NV12 大小反推 MIPI 带宽。

## 2. MIPI、D-PHY、CSI、CIF、ISP 分别是什么

### 2.1 MIPI 是标准组织，不是某一个硬件块

摄像头语境下说“MIPI”通常是把两层合在一起：

- MIPI D-PHY：物理层，规定差分线、电气电平、时钟、低功耗/高速状态；
- MIPI CSI-2：协议层，规定帧、行、虚拟通道、数据类型、packet header、ECC 和 CRC。

类似“以太网 PHY + Ethernet frame”，D-PHY 负责 bit 如何在线上传，CSI-2 负责这些 bit 表示什么。

### 2.2 D-PHY：把差分电信号恢复成 bit

典型连接包含：

```text
1 对 clock lane
1/2/4 对 data lane
```

D-PHY 有两种主要状态：

- LP：低功耗单端状态，用于空闲、进入/退出高速等；
- HS：高速差分 DDR 传输，用于图像数据。

接收端 D-PHY 负责终端电阻、lane 状态检测、HS settle、时钟恢复、串并转换和错误状态。它一般不理解“这是第几行 RAW10”，那属于 CSI-2 controller。

当前仓库把 DPHY 分成两个驱动：

- `phy-rockchip-csi2-dphy-hw.c`：共享实体硬件、寄存器、clock/reset；
- `phy-rockchip-csi2-dphy.c`：V4L2 subdev、endpoint、sensor 绑定和 stream 控制。

DTS 对应：

```text
csi2_dphy_hw: compatible = "rockchip,rv1106-csi2-dphy-hw"
csi2_dphy0:   compatible = "rockchip,rv1106-csi2-dphy"
```

### 2.3 CSI-2：给像素加上 packet 语义

CSI-2 packet 分两类。

Short packet 固定 4 byte：

```text
Data ID(VC + Data Type) | Short Packet Data(2 B) | ECC(1 B)
```

常见用途是 Frame Start、Frame End、Line Start、Line End。

Long packet：

```text
4-byte header | payload，由 Word Count 指定 | 2-byte CRC
```

图像行通常放在 long packet 中。Data Type 表示 RAW8/10/12、YUV422、RGB 等，Virtual Channel 允许同一物理链路复用多路数据或 HDR 曝光。

CSI-2 header 的 ECC保护 header，CRC16 保护 long-packet payload。CSI host 解包后会报告 ECC、CRC、lane 和 FIFO 等错误。

### 2.4 CSI Host：协议接收控制器

CSI Host 位于 DPHY 后面，职责包括：

- 配置 lane 数、VC 和 Data Type；
- 接收 DPHY 提供的字节流；
- 解析 short/long packet；
- 检查 header ECC、payload CRC；
- 按 virtual channel/data type 将像素送给后级；
- 上报 packet、FIFO、同步等中断。

当前节点 `mipi0_csi2` 匹配 `drivers/media/platform/rockchip/cif/mipi-csi2.c`。其 `.s_stream()` 会配置 host 并继续调用上游 DPHY/sensor 的 `.s_stream()`。

### 2.5 CIF：Camera Interface，接收、路由和落存

Rockchip CIF（代码名 RKCIF）是 SoC 摄像头接入前端。它不是 MIPI 标准的一部分。其职责随 SoC 变化，通常包括：

- 接收 CSI2/DVP/LVDS 数据；
- VC/channel 路由；
- crop、对齐和格式处理；
- DMA 写 DDR，或经 SDITF 在线送 ISP；
- 管理帧开始/结束、中断和 buffer 切换。

当前链路使用：

```text
rkcif_mipi_lvds → rkcif_mipi_lvds_sditf → rkisp_vir0
```

SDITF 可以理解为 CIF 到 ISP 的内部串接接口。CIF 驱动还实现 V4L2 capture/vb2，用 `rkcif_buf_queue()`、`rkcif_start_streaming()` 和 IRQ handler 管理 DMA buffer 生命周期。

### 2.6 ISP：把 RAW 变成可用图像

ISP 处理 Bayer RAW，常见模块包括：

- black level correction；
- defective pixel correction；
- lens shading correction；
- demosaic；
- denoise、sharpen；
- white balance、color correction、gamma；
- crop/scale；
- RAW/RGB 到 YUV/NV12；
- 生成 AE/AWB/AF statistics。

ISP 驱动负责硬件寄存器、DMA、stats/params video node 和中断；rkaiq 等用户态算法读取统计量、结合 IQ JSON 计算参数，再交回 ISP/sensor。ISP 驱动本身不等于完整 3A 算法。

## 3. 一帧数据究竟怎样流动

### 3.1 控制面：先让所有模块达成一致

打开 video node 后，应用通过 V4L2 ioctl 设置 640×480 NV12。V4L2/media pipeline 会在各 pad 上传播格式；sensor 仍选择其支持的 RAW mode，例如 SC4336 2560×1440 RAW10。

开始 streaming 前大致发生：

```text
VIDIOC_REQBUFS/QUERYBUF/QBUF
  → vb2 建立可供 DMA 使用的 buffer queue

VIDIOC_STREAMON
  → vb2 start_streaming
  → CIF/ISP 配置输出尺寸、stride、DMA 地址和中断
  → CSI host 配置 lanes、VC、data type
  → DPHY 配置 lane rate / settle
  → sensor 写 mode 寄存器、曝光/增益
  → sensor register 0x0100 进入 streaming
```

驱动通常从下游向上游准备，最后才启动 sensor，避免 sensor 已经吐数据而后级尚未准备。

### 3.2 数据面：曝光到 DQBUF

```text
1. Sensor 像素阵列积分曝光
2. Sensor ADC 将模拟电荷变成 Bayer RAW10
3. Sensor CSI-2 TX 将 RAW10 打包成行 long packets
4. D-PHY TX 经 clock lane + 2 data lanes 串行发送
5. RV1106 D-PHY RX 恢复时钟和 bit/byte
6. CSI Host 解析 VC、Data Type、Word Count、ECC、CRC
7. RKCIF 接收、路由并经 SDITF 送 ISP
8. ISP 处理 RAW，生成 NV12/统计数据
9. DMA 将结果写到当前 vb2 buffer
10. frame-end IRQ 到来，驱动更新 buffer 状态
11. vb2_buffer_done(..., VB2_BUF_STATE_DONE)
12. VIDIOC_DQBUF 返回 index/timestamp/bytesused
13. 应用通过 mmap 地址或 DMA-BUF fd 使用图像
14. VIDIOC_QBUF 将 buffer 交回驱动复用
```

这里存在两种常见硬件路径：

- online：CIF 通过内部接口直接送 ISP，中间 RAW 不一定完整落 DDR；
- offline/readback：CIF 先把 RAW 写 DDR，ISP 再读回，灵活但增加 DDR 带宽和 buffer。

当前 DTS 的 `rkcif_mipi_lvds_sditf → rkisp_vir0` 表示存在 CIF→ISP 串接关系，具体 mode 还受驱动和用户态配置影响。

### 3.3 buffer 是在哪里复用的

MIPI lane 上没有“应用 buffer”；它持续传 packet。buffer 从 CIF/ISP DMA 写内存这一层开始出现。

应用先申请 N 个 vb2 buffer：

```text
QUEUED → 交给 DMA → ACTIVE → 一帧完成 → DONE
   ↑                                      ↓
   └────────── 应用处理后再次 QBUF ────────┘
```

`VIDIOC_EXPBUF` 把同一物理 buffer 导出为 DMA-BUF fd。RGA/NPU 可 attach/map 这个 fd，无需先 memcpy 到另一个普通 CPU buffer。这里优化的是 DDR 分配、拷贝和 cache 维护，不会减少 sensor 在 MIPI 上发出的 RAW 数据量。

## 4. Linux 驱动知识：从 DTS 到函数调用

### 4.1 Linux 驱动的四个核心对象

```text
bus       规定 device 和 driver 如何匹配
device    某个硬件实例及其资源
driver    能操作一类设备的代码和 callbacks
class     面向用户态的功能分类，如 video4linux、sound
```

本链路涉及：

- platform bus：DPHY、CSI host、CIF、ISP 等片上 IP；
- I2C bus：SC3336/SC4336/SC530AI sensor；
- media/V4L2 framework：subdev、entity、pad、link、video node；
- DMA/PHY/clock/reset/pinctrl/regulator 等公共子系统。

### 4.2 DTS 节点如何变成 device

内核启动时解析 DTB：

- SoC 节点通常创建 `platform_device`；
- I2C controller probe 后枚举其子节点，创建 `i2c_client`；
- `status = "disabled"` 的节点不会作为正常设备使用；
- `reg`、`interrupts`、`clocks`、`resets`、`dmas`、GPIO 等成为资源描述。

驱动注册时带有 `of_device_id`：

```c
static const struct of_device_id match[] = {
    { .compatible = "rockchip,rv1106-csi2-dphy" },
    { }
};
```

device 的 `compatible` 与表项相等后，driver core 调用 `probe()`。内核模块还生成 OF `MODULE_ALIAS`，用户态 uevent/modprobe 能据此自动加载 `.ko`。

### 4.3 `probe()` 应该做什么

典型 platform probe：

```text
devm_kzalloc 私有结构
platform_get_resource / devm_ioremap_resource 映射寄存器
platform_get_irq / devm_request_irq 注册中断
devm_clk_get、reset_control_get、pinctrl、runtime PM
解析 graph endpoint
注册 PHY/V4L2 subdev/media entity/video device
platform_set_drvdata 保存私有指针
```

典型 sensor I2C probe：

```text
解析 module 属性和 endpoint
获取 xvclk、GPIO、regulator
v4l2_i2c_subdev_init
初始化 controls、media pad
上电并用 I2C 读取 chip ID
v4l2_async_register_subdev_sensor_common
```

`devm_*` 资源与 device 生命周期绑定，probe 失败或 remove 时自动回收，显著简化错误路径。

### 4.4 为什么需要 V4L2 subdev 和 Media Controller

一个 camera pipeline 有多个硬件块，不能都表现成独立 `/dev/videoN` 给普通应用读写。

- sensor、DPHY、CSI receiver、bridge、ISP：通常注册为 V4L2 subdev；
- 可被用户态排队 buffer 的 capture/output/statistics 端点：注册 video device；
- media entity 表示功能块；
- pad 表示输入/输出端口；
- link 表示实体间连接。

DTS `endpoint/remote-endpoint` 描述物理拓扑，驱动用 fwnode graph API 和 V4L2 async notifier 等待远端 subdev：

```text
notifier_init
  → parse_fwnode_endpoints
  → notifier_register
  → remote subdev 出现
  → .bound 建立关联/link
  → 全部到齐
  → .complete 注册完整 pipeline/subdev nodes
```

因此 probe 顺序可以变化。上游没到齐时的 deferred probe 或 notifier waiting 不一定是错误。

### 4.5 V4L2 ioctl 怎样进入驱动

```text
应用 ioctl(fd, VIDIOC_*, arg)
  → video_ioctl2
  → V4L2 core 校验和分派
  → v4l2_ioctl_ops 或 vb2 ioctl helper
  → 具体 CIF/ISP driver callback
```

buffer 相关 ioctl 通常由 vb2 帮助函数处理：

| 用户 ioctl | vb2/驱动含义 |
|---|---|
| `REQBUFS` | 选择 memory model 并建立 buffer pool |
| `QUERYBUF` | 返回 plane offset/length |
| `QBUF` | buffer 进入 queued list，调用 `.buf_queue` |
| `STREAMON` | 检查最少 buffer，调用 `.start_streaming` |
| `DQBUF` | 等待 done queue，返回已完成 buffer |
| `STREAMOFF` | 调 `.stop_streaming`，归还所有 buffer |

当前 RKCIF 的 vb2 ops 包含：

```text
.buf_queue       = rkcif_buf_queue
.start_streaming = rkcif_start_streaming
.stop_streaming  = rkcif_stop_streaming
```

### 4.6 中断、DMA 和并发

硬件不会在进程上下文中“主动调用应用”。帧完成路径是：

```text
DMA 写完 / frame end
  → 硬件置 IRQ status
  → CPU 进入 ISR
  → 驱动读清中断、切换下一 buffer
  → 完成当前 vb2 buffer
  → 唤醒阻塞在 poll/DQBUF 的进程
```

中断上下文不能睡眠，不能做慢速 I2C 或大规模内存处理。共享队列通常用 spinlock；可睡眠的配置路径使用 mutex。耗时工作应放 threaded IRQ、workqueue 或用户态。

DMA 地址也不等于 CPU 虚拟地址：

- CPU 使用 kernel/user virtual address；
- DMA engine 使用 DMA/IOMMU address；
- vb2 memory allocator 建立对应关系；
- DMA-BUF 在设备之间共享 backing storage，并管理 attach/map/fence/cache sync。

### 4.7 runtime PM 与 stream 生命周期

probe 成功不表示硬件一直上电。runtime PM 允许空闲时关闭 clock/power domain：

```text
open/configure
  → pm_runtime_resume_and_get
  → enable clocks / deassert reset / power PHY

stream off/close
  → pm_runtime_put
  → autosuspend
  → disable clocks / assert reset / power down PHY
```

sensor 还会控制 MCLK、PWDN 和 regulator。若 runtime PM 引用计数不平衡，常见表现是第二次开流失败、待机功耗高或 suspend/resume 后无图。

### 4.8 错误码与日志应该怎样看

- `-EPROBE_DEFER`：依赖的 clock/PHY/subdev 尚未出现，稍后重试；
- `-ENODEV`：设备或 chip ID 不存在；
- `-EINVAL`：lane、format、分辨率或参数不支持；
- CSI ECC/CRC：信号完整性、lane rate、settle、时钟或 sensor 配置；
- FIFO overflow：后级/DDR 来不及消费，或 pipeline/clock 配置不足；
- frame timeout：sensor 未出流、link 未启用或中断未到。

## 5. 当前仓库的 stream 调用关系

代码中的关键入口是：

```text
yolov5_demo v4l2_capture_init
  → VIDIOC_STREAMON
  → vb2 rkcif_start_streaming
  → RKCIF/ISP pipeline stream enable
  → v4l2_subdev_call(..., s_stream, 1)
  → mipi-csi2.c:csi2_s_stream()
  → phy-rockchip-csi2-dphy.c:csi2_dphy_s_stream()
  → scxxxx_s_stream()
  → sensor register 0x0100 = streaming
```

停止大致反向执行。实际 vendor pipeline 中 CIF 和 ISP 谁作为 video-node owner、调用次序以及 online/readback mode 由所打开节点和 media links 决定，应结合板端 `media-ctl -p` 验证。

关键源码：

- `drivers/media/i2c/sc3336.c`
- `drivers/media/i2c/sc4336.c`
- `drivers/media/i2c/sc530ai.c`
- `drivers/phy/rockchip/phy-rockchip-csi2-dphy.c`
- `drivers/phy/rockchip/phy-rockchip-csi2-dphy-hw.c`
- `drivers/media/platform/rockchip/cif/mipi-csi2.c`
- `drivers/media/platform/rockchip/cif/dev.c`
- `drivers/media/platform/rockchip/cif/capture.c`
- `drivers/media/platform/rockchip/isp/`
- `yolov5_demo/cpp/v4l2_capture.cc`
- `yolov5_demo/cpp/AIcamera_c_interface.cc`

## 6. 带宽排查公式与命令

### 6.1 常用公式

```text
active_payload_bit_rate = width × height × fps × bits_per_pixel
lane_bit_rate           = link_freq × 2
total_wire_capacity     = lane_bit_rate × lane_count
utilization             = active_payload_bit_rate / total_wire_capacity

RAW10 payload bytes/line = ceil(width × 10 / 8)
RAW12 payload bytes/line = ceil(width × 12 / 8)
NV12 memory bytes/frame  = width × height × 3 / 2
```

工程上不能让 `utilization` 接近 100%，还要考虑 packet/PHY 开销、消隐时序、embedded data 和频偏。最终应以 sensor timing 表、接收端计数器和稳定性测试为准。

### 6.2 板端验证

```sh
media-ctl -p
v4l2-ctl --list-devices
v4l2-ctl -d /dev/v4l-subdevN --get-subdev-fmt pad=0
v4l2-ctl -d /dev/video11 --get-fmt-video
v4l2-ctl -d /dev/video11 --get-parm
dmesg | grep -Ei 'dphy|csi|cif|rkisp|ecc|crc|overflow|timeout'
```

建议同时核对四个层次：

1. sensor mode：分辨率、fps、RAW bit depth、link frequency；
2. DTS endpoint：lane 数和 lane 映射；
3. media pads：mbus code、width/height、VC；
4. video node：最终 memory format、stride、sizeimage、fps。

只有四层一致，计算出的 MIPI 带宽和应用实际拿到的数据才能对应起来。

## 7. 完整匹配流程：从 DTS 到 `probe()` 的 6 个步骤

下面不使用无关的 DSI 示例，而是直接使用本项目的 MIPI 摄像头接收链路。需要特别注意：一条摄像头链路不是一个 DTS 节点匹配一个“大驱动”，而是 sensor、DPHY、CSI Host、CIF、ISP 分别匹配各自的驱动，之后再由 V4L2 Media Controller 拼成 pipeline。

### 步骤 1：DTS 编写节点，使用 `compatible` 声明设备类型

以当前 DPHY 逻辑节点为例，SoC DTSI 中的结构可简化为：

```dts
csi2_dphy0: csi2-dphy0 {
    compatible = "rockchip,rv1106-csi2-dphy";
    rockchip,hw = <&csi2_dphy_hw>;
    status = "okay";

    ports {
        port@0 {
            reg = <0>;
            csi2_dphy0_input: endpoint@1 {
                reg = <1>;
                data-lanes = <1 2>;
                remote-endpoint = <&sc4336_out>;
            };
        };

        port@1 {
            reg = <1>;
            csi2_dphy0_output: endpoint@0 {
                reg = <0>;
                remote-endpoint = <&mipi0_csi2_input>;
            };
        };
    };
};
```

其中几类属性的含义不同：

| 属性 | 作用 |
|---|---|
| `compatible` | 选择能管理该设备的候选驱动 |
| `reg` | 寄存器或总线地址；并非每个逻辑节点都有独立寄存器 |
| `interrupts` | 硬件中断资源 |
| `clocks/resets/power-domains` | 时钟、复位和电源依赖 |
| `data-lanes` | MIPI data lane 数量及映射 |
| `remote-endpoint` | 描述数据端口与另一个 media entity 的连接 |
| `status = "okay"` | 启用设备节点 |

可以把 `compatible` 理解为“设备类型身份证”，但它不是唯一实例 ID。同一种 compatible 可以对应多个硬件实例；实例由设备树节点路径、寄存器地址等区分。

本项目完整链路中的身份证包括：

```text
smartsens,sc4336            Sensor
rockchip,rv1106-csi2-dphy   D-PHY 逻辑接收端
rockchip,rv1106-csi2-dphy-hw D-PHY 实体硬件
rockchip,rk3588-mipi-csi2   CSI-2 Host
rockchip,rv1106-cif         CIF hardware
rockchip,rkcif-mipi-lvds    CIF MIPI/LVDS interface
rockchip,rkcif-sditf        CIF → ISP bridge
rockchip,rv1106-rkisp       ISP hardware
rockchip,rkisp-vir          ISP virtual instance
```

### 步骤 2：DTS 编译成 DTB，Bootloader 传给内核

当前构建选择：

```text
BoardConfig
  → RK_KERNEL_DTS=rv1106g-echo-mate.dts
  → C preprocessor 展开 #include 和宏
  → dtc 编译
  → rv1106g-echo-mate.dtb
  → 打包进 boot image
```

Bootloader 启动内核时传入 DTB 地址。早期内核通过 `unflatten_device_tree()` 将扁平 DTB 展开为内存中的 `struct device_node` 树。

这一步只有“描述数据”进入内核，还没有执行 SC4336、DPHY 或 CIF 驱动。

### 步骤 3：内核把设备树节点实例化为 Linux device

内核初始化 platform bus 并遍历可用节点。片上控制器通常被创建为：

```text
device_node
  → of_platform_populate()
  → platform_device
```

因此 DPHY、CSI Host、CIF、ISP 通常是 `platform_device`。

Sensor 稍有不同。首先创建 I2C4 controller 的 platform device；I2C controller 驱动 probe 并注册 `i2c_adapter` 后，I2C core 根据 I2C4 子节点创建：

```text
sc4336@30 device_node
  → i2c_client
     adapter = i2c4
     address = 0x30
```

关系图：

```text
DTB
 ├─ csi2-dphy0 node ───────────────> platform_device
 ├─ mipi-csi2 node ────────────────> platform_device
 ├─ rkcif/rkisp nodes ─────────────> platform_device
 └─ i2c4 node ─> I2C controller probe ─> i2c_adapter
                   └─ sc4336@30 ───────> i2c_client
```

### 步骤 4：驱动注册，并提供 `of_match_table`

DPHY platform driver 的结构可简化为：

```c
static const struct of_device_id rockchip_csi2_dphy_match_id[] = {
    {
        .compatible = "rockchip,rv1106-csi2-dphy",
        .data = &rv1106_dphy_drv_data,
    },
    { }
};
MODULE_DEVICE_TABLE(of, rockchip_csi2_dphy_match_id);

static struct platform_driver rockchip_csi2_dphy_driver = {
    .probe = rockchip_csi2_dphy_probe,
    .remove = rockchip_csi2_dphy_remove,
    .driver = {
        .name = "rockchip-csi2-dphy",
        .of_match_table = rockchip_csi2_dphy_match_id,
    },
};

module_platform_driver(rockchip_csi2_dphy_driver);
```

上面是为了说明结构而做的简化，具体类型和符号以仓库源码为准。三个关键点是：

1. `.of_match_table`：driver core 在运行时用它匹配设备；
2. `.probe`：匹配成功后的初始化入口；
3. `module_platform_driver()`：生成模块 init/exit，完成 driver 注册和注销。

`MODULE_DEVICE_TABLE(of, ...)` 不是运行时匹配动作本身。它主要把 OF alias 导出到模块元数据：

```text
modules.alias:
of:N*T*Crockchip,rv1106-csi2-dphy*
```

当设备出现时，内核发送带 modalias 的 uevent；用户态的 kmod/modprobe 可利用该 alias 自动装载正确 `.ko`。模块加载后，仍由 `.of_match_table` 完成实际匹配。

若驱动是 built-in，即使不需要 modprobe，仍然使用同一个 `of_match_table` 匹配。

Sensor 使用 `struct i2c_driver`：

```c
static const struct of_device_id sc4336_of_match[] = {
    { .compatible = "smartsens,sc4336" },
    { }
};

static struct i2c_driver sc4336_i2c_driver = {
    .driver = {
        .name = "sc4336",
        .of_match_table = sc4336_of_match,
        .pm = &sc4336_pm_ops,
    },
    .probe = sc4336_probe,
    .remove = sc4336_remove,
};
```

虽然 platform 和 I2C 的总线类型不同，但都是：device 提供 compatible，driver 提供 match table，bus 负责比较。

### 步骤 5：bus 执行 match，匹配成功后调用 `probe()`

platform 设备的主路径可概括为：

```text
platform_device_register
  或
platform_driver_register
        ↓
driver core 尝试 device ↔ driver 配对
        ↓
platform_bus_type.match
        ↓
platform_match()
        ↓
of_driver_match_device()
        ↓
比较 device_node.compatible 与 driver.of_match_table
        ↓ match
really_probe()/driver_probe_device()
        ↓
platform_driver.probe(pdev)
```

I2C sensor 的主路径类似：

```text
i2c_client + i2c_driver
  → i2c_bus_type.match
  → OF compatible / ACPI / I2C ID 匹配
  → sc4336_probe(client, id)
```

设备可以先注册，驱动后注册；也可以驱动先注册，设备后出现。driver core 都会尝试配对，所以不能依赖固定 probe 顺序。

`compatible` 还可以按“最具体 → 兼容后备”写多个字符串：

```dts
compatible = "vendor,board-special-ip", "vendor,generic-ip";
```

驱动会选择第一个能匹配的表项，并可从表项 `.data` 取得 SoC variant 数据，例如不同寄存器布局、lane 数上限或初始化函数。

### 步骤 6：`probe()` 获取资源、初始化硬件抽象并注册上层接口

DPHY 的 `rockchip_csi2_dphy_probe()` 不会在 probe 时一直打开高速接收。它主要建立软件对象：

```text
rockchip_csi2_dphy_probe(pdev)
  ├─ 分配并初始化 private data
  ├─ of_match_device/of_device_get_match_data 取得 RV1106 variant
  ├─ 解析 endpoint、lane 和共享 DPHY hardware
  ├─ 初始化 v4l2_subdev
  ├─ 初始化 media pads/entity
  ├─ 初始化 V4L2 async notifier
  ├─ 等待并绑定上游 sensor subdev
  ├─ 注册 DPHY subdev
  └─ 配置 runtime PM
```

SC4336 的 probe 还会真正验证硬件身份：

```text
sc4336_probe(client)
  ├─ 解析 DTS module、clock、GPIO、regulator
  ├─ v4l2_i2c_subdev_init()
  ├─ 初始化 controls 和 media pad
  ├─ sensor 上电
  ├─ I2C 读取 0x3107 等 chip-ID 寄存器
  ├─ 比较期望 CHIP_ID 0xdc42
  └─ v4l2_async_register_subdev_sensor_common()
```

所以“compatible 匹配成功”和“probe 成功”不是一回事。例如 DTS 写了 SC4336，但实物是 SC3336时：

```text
compatible match 成功
  → sc4336_probe() 被调用
  → I2C 读到的 chip ID 不等于 0xdc42
  → probe 返回错误
  → driver 不绑定该 device
```

`probe()` 成功后，`struct device` 的 driver 指针才真正建立绑定。随后可在 sysfs 看到 device/driver 关系。

## 8. 单设备匹配流程图

下面以 RV1106 DPHY 为例：

```text
┌────────────────────────────────────────────┐
│ DTS                                        │
│ compatible="rockchip,rv1106-csi2-dphy"    │
└─────────────────────┬──────────────────────┘
                      │ dtc
                      ▼
┌────────────────────────────────────────────┐
│ DTB → Bootloader → kernel device_node      │
└─────────────────────┬──────────────────────┘
                      │ of_platform_populate
                      ▼
┌────────────────────────────────────────────┐
│ platform_device                            │
│ dev.of_node → csi2_dphy0                   │
└─────────────────────┬──────────────────────┘
                      │ platform bus match
                      ▼
┌────────────────────────────────────────────┐
│ platform_driver                            │
│ .of_match_table contains the compatible    │
└─────────────────────┬──────────────────────┘
                      │ match success
                      ▼
┌────────────────────────────────────────────┐
│ rockchip_csi2_dphy_probe(pdev)             │
│ parse graph / init subdev / notifier / PM  │
└─────────────────────┬──────────────────────┘
                      │ register succeeds
                      ▼
┌────────────────────────────────────────────┐
│ V4L2 subdev + media entity                 │
│ waits for sensor and downstream CSI links  │
└────────────────────────────────────────────┘
```

匹配核心可以压缩为一句话：

```text
DTS compatible
    ==
driver.of_match_table[i].compatible
    → bus match
    → probe(device)
```

## 9. 整条摄像头链路的并行匹配与异步组装

每个方框都有自己独立的第 1～6 步：

```text
DTS/DTB                 Linux device         matched driver
──────────────────      ──────────────       ──────────────────────────
sc4336@30          ───> i2c_client       ───> sc4336.c
csi2_dphy_hw       ───> platform_device  ───> csi2-dphy-hw.c
csi2_dphy0         ───> platform_device  ───> csi2-dphy.c
mipi0_csi2         ───> platform_device  ───> cif/mipi-csi2.c
rkcif hw/interface ───> platform_device  ───> cif/hw.c + dev.c
rkcif_sditf        ───> platform_device  ───> cif sditf driver
rkisp              ───> platform_device  ───> isp/hw.c
rkisp_vir0         ───> platform_device  ───> isp/dev.c
```

这些 probe 的任务是把“各零件”注册出来。`remote-endpoint` 不参与 driver compatible 匹配，而是在零件注册后用于建立数据拓扑：

```text
第一阶段：Linux driver model

device compatible ↔ driver match table
  → 各自 probe
  → 各自 V4L2 subdev/media entity 注册

第二阶段：V4L2 async + Media Controller

endpoint ↔ remote-endpoint
  → notifier .bound
  → media_create_pad_link
  → notifier .complete
  → 完整 pipeline 可用
```

整条链路图：

```text
                  I2C control
┌────────────┐   compatible match   ┌──────────────────┐
│ SC4336 DTS │ ───────────────────> │ sc4336 subdev    │
└──────┬─────┘                      └────────┬─────────┘
       │ endpoint                            │ RAW10 packets
       ▼                                     ▼
┌────────────┐   compatible match   ┌──────────────────┐
│ DPHY DTS   │ ───────────────────> │ DPHY subdev      │
└──────┬─────┘                      └────────┬─────────┘
       │ endpoint                            │ byte stream
       ▼                                     ▼
┌────────────┐   compatible match   ┌──────────────────┐
│ CSI2 DTS   │ ───────────────────> │ CSI Host subdev  │
└──────┬─────┘                      └────────┬─────────┘
       │ endpoint                            │ parsed pixels
       ▼                                     ▼
┌────────────┐   compatible match   ┌──────────────────┐
│ CIF DTS    │ ───────────────────> │ RKCIF            │
└──────┬─────┘                      └────────┬─────────┘
       │ endpoint/SDITF                      │ online/readback
       ▼                                     ▼
┌────────────┐   compatible match   ┌──────────────────┐
│ ISP DTS    │ ───────────────────> │ RKISP + video    │
└────────────┘                      └────────┬─────────┘
                                           │ vb2/DMA
                                           ▼
                                  /dev/video11 → application
```

## 10. `open()` 到出图的运行调用流程

`probe()` 只完成驱动绑定和接口注册；真正的数据流由应用启动：

```text
Application
  │
  ├─ open("/dev/video11")
  │    └─ video_device file_operations.open
  │
  ├─ VIDIOC_S_FMT(640×480 NV12)
  │    └─ video_ioctl2 → ISP/CIF vidioc_s_fmt
  │         └─ pad format negotiation
  │              Sensor: 2560×1440 RAW10
  │              ISP output: 640×480 NV12
  │
  ├─ VIDIOC_REQBUFS / QUERYBUF / EXPBUF
  │    └─ vb2 creates/maps DMA-capable buffers
  │
  ├─ VIDIOC_QBUF × N
  │    └─ vb2 → rkcif_buf_queue / ISP queue callback
  │
  └─ VIDIOC_STREAMON
       └─ vb2_streamon
          └─ .start_streaming
             ├─ program ISP/CIF DMA addresses
             ├─ enable frame interrupts
             └─ v4l2_subdev_call(upstream, s_stream, 1)
                ├─ CSI Host csi2_s_stream(1)
                ├─ DPHY csi2_dphy_s_stream(1)
                └─ Sensor sc4336_s_stream(1)
                    └─ I2C write 0x0100 = streaming
```

帧完成流程：

```text
SC4336 sends CSI-2 RAW10
  → DPHY receives electrical symbols
  → CSI Host checks/unpacks packets
  → CIF routes pixels
  → ISP produces NV12
  → DMA completes one vb2 buffer
  → frame-end IRQ
  → ISR acknowledges interrupt and rotates buffer
  → vb2_buffer_done()
  → application VIDIOC_DQBUF wakes up
  → RGA/NPU consumes DMA-BUF fd
  → application VIDIOC_QBUF returns buffer
```

停止：

```text
VIDIOC_STREAMOFF
  → vb2 .stop_streaming
  → disable sensor/DPHY/CSI/CIF/ISP in safe order
  → disable DMA/interrupts
  → return queued buffers with DONE/ERROR state
  → runtime PM releases clock/power references
```

## 11. 匹配失败时在哪一步定位

| 现象 | 所处步骤 | 检查方法 |
|---|---|---|
| `/proc/device-tree` 没有节点 | DTS/DTB | 是否选择并烧录了正确 DTB |
| 节点存在但没有 Linux device | device 实例化 | `status`、父 bus/controller 是否成功 |
| device 存在但没有 driver | driver match | compatible 拼写、模块 Kconfig、`.ko`/modules.alias |
| driver 调用了但 probe 失败 | probe | `dmesg`、clock/reset/GPIO/I2C chip ID |
| 所有 probe 成功但 graph 不完整 | async/media bind | endpoint、remote-endpoint、pad/port 编号 |
| `/dev/videoN` 存在但 STREAMON 失败 | runtime stream | 格式、lane、link frequency、上游 `.s_stream()` |
| STREAMON 成功但 DQBUF 超时 | data/IRQ/DMA | sensor 出流、CSI error、CIF/ISP IRQ、DMA buffer |

推荐按顺序验证：

```sh
# 1. DTB 中是否有正确节点和 compatible
tr '\0' '\n' < /proc/device-tree/.../compatible

# 2. platform/I2C device 是否出现
ls -l /sys/bus/platform/devices/
ls -l /sys/bus/i2c/devices/

# 3. device 是否绑定 driver
readlink /sys/bus/platform/devices/<device>/driver
readlink /sys/bus/i2c/devices/4-0030/driver

# 4. module alias 是否存在
modinfo sc4336
modinfo phy_rockchip_csi2_dphy

# 5. media graph 是否 complete
media-ctl -p

# 6. video stream 是否真正运行
v4l2-ctl -d /dev/video11 --stream-mmap=4 --stream-count=100
```

不同镜像中的 sysfs 路径和模块名可能略有差异，执行前应以 `find /sys/bus/...`、`lsmod` 和 `modinfo` 的实际输出为准。
