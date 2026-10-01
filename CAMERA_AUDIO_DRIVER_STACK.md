# Echo Mate 摄像头与音频完整链路

本文按当前仓库实际配置说明 RV1106 Echo Mate 从板级 DTS、内核驱动匹配，到用户态 `AIChat_demo` / `yolov5_demo` 调用的完整链路。重点不是泛泛介绍 Linux 框架，而是回答下面四个问题：

1. 哪个 DTS 和内核配置真正参与编译；
2. DTS 节点如何匹配到驱动的 `probe()`；
3. 内核中各子设备如何组装，最终生成什么设备节点；
4. 当前应用具体通过哪些 API 获得摄像头帧、录音和播放。

> 说明：`/dev/videoN`、ALSA card/device 编号取决于启动时的注册顺序。本文会标出当前应用使用的编号，但不把该编号当作稳定 ABI。

MIPI RAW10 字节数、CSI-2 packet、D-PHY/CIF/ISP 分工以及 Linux V4L2 驱动模型的深入说明，见 `MIPI_CSI_LINUX_DRIVER_GUIDE.md`。

## 1. 从板级配置到 DTB、内核和 rootfs

### 1.1 当前构建入口

SDK 根目录的 `.BoardConfig.mk` 当前选择：

```text
project/cfg/BoardConfig_IPC/
  BoardConfig-SD_CARD-Buildroot-RV1106_Echo_Mate-DeskMate.mk
```

该配置给出本链路最重要的构建变量：

```text
RK_CHIP=rv1106
RK_KERNEL_DTS=rv1106g-echo-mate.dts
RK_KERNEL_DEFCONFIG=echo_rv1106_linux_defconfig
RK_BUILDROOT_DEFCONFIG=echo_mate_defconfig
RK_CAMERA_SENSOR_IQFILES=
  sc4336_OT01_40IRC_F16.json
  sc3336_CMK-OT2119-PC1_30IRC-F16.json
```

`build.sh` 读取这些变量，将所选 DTS、kernel defconfig 和 Buildroot defconfig 链接到各子构建系统，然后调用 kernel、media 和 rootfs 构建。其结果可概括为：

```text
BoardConfig
   ├─ RK_KERNEL_DTS ───────> dtc ─────> rv1106g-echo-mate.dtb ─┐
   ├─ RK_KERNEL_DEFCONFIG ─> Kconfig ──> Image + *.ko          ├─> 固件镜像
   ├─ RK_BUILDROOT_DEFCONFIG ──────────> rootfs + 用户态库/工具 ┤
   └─ RK_CAMERA_SENSOR_IQFILES ────────> /oem/usr/share/iqfiles ┘
```

DTS 的 `#include` 在编译期展开，`dtc` 生成扁平设备树 DTB。Bootloader 启动内核时把 DTB 地址传给内核；内核展开 DTB，创建 platform device、I2C client，并用 `compatible` 查找驱动的 `of_match_table`。

### 1.2 DTS 的包含关系

当前顶层文件是：

```text
arch/arm/boot/dts/rv1106g-echo-mate.dts
   ├─ rv1106.dtsi                  SoC IP：寄存器、IRQ、clock、reset、DMA
   ├─ rv1106-evb.dtsi              通用板级配置
   └─ rv1106-echo-mate-ipc.dtsi    本板 camera/audio 连线和 enable 状态
```

理解 DTS 时要区分两类信息：

- SoC `.dtsi` 描述“芯片里有什么”：I2C、MIPI DPHY、CSI2 host、CIF、ISP、I2S、codec、DMA。
- 板级 `.dtsi` 描述“板上怎么接”：传感器位于哪个 I2C 地址、MIPI lane、MCLK/PWDN GPIO，以及 CPU DAI 和 codec 如何组成声卡。

`status = "okay"` 使节点参与设备创建；驱动是否真正绑定，还要继续经过 `compatible` 匹配、资源获取和硬件 ID 检查。

## 2. 摄像头链路总览

当前 media graph 是：

```text
SC3336 / SC4336 / SC530AI
  │ I2C4：配置寄存器、曝光、增益、stream on/off
  │ MIPI CSI-2：实际图像像素
  ▼
csi2_dphy0
  ▼
mipi0_csi2
  ▼
rkcif_mipi_lvds
  ▼
rkcif_mipi_lvds_sditf
  ▼
rkisp_vir0 / RKISP
  ▼
V4L2 video node + vb2 buffer queue
  ▼ ioctl + MMAP + DMA-BUF fd
yolov5_demo/AIcamera_c_interface.cc
  ▼ RGA：NV12 → RGB
Arena/NPU：YOLOv5、face 等推理
```

I2C 只负责控制 sensor，原始图像不通过 I2C；像素通过 MIPI CSI-2 进入 DPHY、CSI host、CIF 和 ISP。

### 2.1 Sensor DTS：设备从哪里来

`rv1106-echo-mate-ipc.dtsi` 打开 `i2c4`，并声明三种可能的 sensor：

| DTS compatible | I2C 地址 | 驱动 |
|---|---:|---|
| `smartsens,sc3336` | `0x30` | `drivers/media/i2c/sc3336.c` |
| `smartsens,sc4336` | `0x30` | `drivers/media/i2c/sc4336.c` |
| `smartsens,sc530ai` | `0x30` | `drivers/media/i2c/sc530ai.c` |

节点还给出：

- MCLK：`MCLK_REF_MIPI0`；
- PWDN：GPIO3 PC5；
- pinctrl：`mipi_refclk_out0`；
- `rockchip,camera-module-*` 模组信息；
- endpoint 的 `data-lanes = <1 2>` 和对端 phandle。

三个候选节点使用相同总线和地址，并不表示三颗 sensor 能同时工作。系统尝试匹配相应驱动后，驱动会读芯片 ID；只有与实物相符的驱动能完成 probe，其他候选会失败。量产配置更理想的做法是只启用实际 BOM 对应的 sensor。

### 2.2 Sensor 驱动怎样 match 和 probe

以 SC3336 为例：

```text
DTS compatible = "smartsens,sc3336"
   ↓ OF/I2C modalias
sc3336_of_match[]
   ↓ i2c_driver.probe
sc3336_probe()
   ├─ 读取 module/lens/facing 等 DTS 属性
   ├─ 获取 xvclk、reset/pwdn GPIO、regulator
   ├─ v4l2_i2c_subdev_init()
   ├─ 初始化曝光、增益、vblank 等 V4L2 controls
   ├─ 上电并执行 sc3336_check_sensor_id()
   ├─ media_entity_pads_init()
   └─ v4l2_async_register_subdev_sensor_common()
```

SC4336、SC530AI 的结构相同：`of_device_id` 匹配 `compatible`，各自的 `probe()` 获取资源、检查 sensor ID，然后注册为 V4L2 sub-device。

sensor 的关键运行函数不是 `read()`：

- `.s_stream = scxxxx_s_stream`：启动时批量写 mode 寄存器并进入 streaming，停止时写 standby；
- `.set_fmt` / `.get_fmt`：协商分辨率、media-bus code；
- V4L2 control handler：把 exposure、analogue gain、vblank 等控制转换成 I2C 寄存器写入；
- runtime PM：按使用状态管理 clock、GPIO 和 regulator。

### 2.3 MIPI、CIF、ISP 的 compatible 匹配

| DTS 节点 | compatible | 驱动及入口 |
|---|---|---|
| `csi2_dphy_hw` | `rockchip,rv1106-csi2-dphy-hw` | `phy-rockchip-csi2-dphy-hw.c` platform probe |
| `csi2_dphy0` | `rockchip,rv1106-csi2-dphy` | `phy-rockchip-csi2-dphy.c` platform probe |
| `mipi0_csi2` | `rockchip,rk3588-mipi-csi2` | `media/.../cif/mipi-csi2.c` probe |
| CIF hardware | `rockchip,rv1106-cif` | `media/.../cif/hw.c:rkcif_plat_hw_probe()` |
| MIPI/LVDS CIF | `rockchip,rkcif-mipi-lvds` | CIF platform driver probe |
| CIF→ISP bridge | `rockchip,rkcif-sditf` | CIF sditf driver |
| ISP hardware | `rockchip,rv1106-rkisp` | `media/.../isp/hw.c` probe |
| ISP virtual node | `rockchip,rkisp-vir` | `media/.../isp/dev.c` probe |

`mipi0_csi2` 使用名为 `rk3588` 的 compatible 是当前 vendor 内核的复用设计，不代表板子被识别成 RK3588；匹配的是可兼容的 CSI2 控制器实现。

### 2.4 OF graph 如何把多个驱动拼成一条 pipeline

DTS 中每一级都有 `ports/port/endpoint`，通过 `remote-endpoint` 双向引用：

```text
sensor_out
  ↔ csi_dphy_input / csi_dphy_output
  ↔ mipi_csi2_input / mipi_csi2_output
  ↔ cif_mipi_in
  ↔ rkcif_mipi_lvds_sditf
  ↔ rkisp_vir0
```

各驱动 probe 的先后顺序不可靠，所以 media framework 使用 async notifier：先注册的 bridge 可以等待远端 sensor subdev；当 graph 上需要的 subdev 都出现后执行 bind/complete，创建 media links。最终能用 `media-ctl -p` 看到 entity、pad 和 link，而不是靠应用手工把每个 platform driver 串起来。

CIF/ISP probe 进一步完成：

- `v4l2_device_register()`：注册 V4L2 容器；
- `media_device_register()`：注册 media controller；
- 初始化 video pipeline、controls、interrupt；
- 初始化 videobuf2 队列；
- `video_register_device()`：生成 `/dev/videoN`；
- 注册 subdev nodes，生成需要的 `/dev/v4l-subdevN`。

### 2.5 `STREAMON` 后一帧怎样到达应用

应用先提交一组空 buffer。之后的数据面如下：

```text
sensor 曝光完成
  → MIPI lanes 输出 RAW Bayer packet
  → DPHY 做物理层接收
  → CSI2 host 解包 virtual-channel/data-type
  → CIF 接收并路由
  → ISP 完成 Bayer、AE/AWB 相关处理、颜色与格式输出
  → DMA 写入 vb2 buffer
  → frame-end IRQ
  → 驱动把 buffer 标记 DONE，唤醒 DQBUF
  → 用户态 VIDIOC_DQBUF 获得 buffer index
```

应用消费完成后执行 `VIDIOC_QBUF`，同一组 buffer 在驱动和应用之间循环，避免逐帧申请/释放。当前封装还用 `VIDIOC_EXPBUF` 导出 DMA-BUF fd，使 RGA 可以直接引用摄像头 buffer，减少 CPU 拷贝。

### 2.6 当前应用实际调用

`yolov5_demo/cpp/v4l2_capture.cc` 的初始化顺序是：

```text
v4l2_capture_init()
  ├─ open("/dev/videoN", O_RDWR)
  ├─ VIDIOC_QUERYCAP
  ├─ VIDIOC_S_FMT：640×480、NV12
  ├─ VIDIOC_REQBUFS：V4L2_MEMORY_MMAP
  ├─ 每个 buffer：VIDIOC_QUERYBUF
  ├─ mmap()
  ├─ VIDIOC_EXPBUF：取得 DMA-BUF fd
  ├─ VIDIOC_QBUF
  └─ VIDIOC_STREAMON
```

逐帧路径：

```text
v4l2_capture_get_frame()
  → VIDIOC_DQBUF
  → 返回 index、虚拟地址和 DMA-BUF fd
  → AIcamera 中 RGA 将 NV12 fd 转换到 Arena RGB fd
  → YOLOv5/NPU 推理和显示处理
  → v4l2_capture_put_frame()
  → VIDIOC_QBUF
```

关闭路径是 `VIDIOC_STREAMOFF → munmap → close`。

`AIcamera_c_interface.cc:start_ai_camera_v2()` 当前把设备号写为 `11`，即打开 `/dev/video11`。这在当前镜像注册顺序下可工作，但新增/删除 video entity 后编号可能变化。更稳健的方式是按 `/sys/class/video4linux/video*/name` 或 `media-ctl` 拓扑查找目标 ISP mainpath/selfpath，而不是固化数字。

### 2.7 IQ 文件处于哪一层

SC3336/SC4336 的 JSON IQ 文件不是 DTS，也不参与 kernel driver match。它属于用户态 ISP tuning 数据，用于 AE、AWB、降噪、颜色、锐化等算法参数：

```text
BoardConfig 的 RK_CAMERA_SENSOR_IQFILES
  → media 构建复制 JSON 到 /oem/usr/share/iqfiles
  → rkaiq/rkipc 以 IQ 目录初始化算法
  → 算法根据统计信息计算 ISP/sensor 参数
  → 通过 V4L2 controls/subdev ioctl 下发到 ISP 和 sensor
```

仓库的 `RkLunch.sh` 在 IQ 目录存在时使用 `rkipc -a /oem/usr/share/iqfiles`。但当前 `AIcamera_c_interface.cc` 本身只操作 V4L2 video node，没有直接初始化 rkaiq。因此部署时必须明确由哪个常驻进程负责 ISP 3A；不能只看到 `/dev/video11` 可打开就认为 IQ/3A 已运行。

## 3. 音频链路总览

录音链路：

```text
Mic 模拟信号
  → RV1106 ADC/codec
  → I2S0 RX
  → PL330 DMA channel 21
  → ALSA dmaengine PCM capture ring buffer
  → /dev/snd/pcmC*D*c
  → libasound
  → PortAudio callback
  → AudioProcess::recordCallback()
  → PCM queue → Opus encoder/KWS → WebSocket
```

播放链路：

```text
WebSocket Opus
  → Opus decoder → PCM queue
  → AudioProcess::playCallback()
  → PortAudio → libasound
  → /dev/snd/pcmC*D*p
  → ALSA dmaengine PCM playback ring buffer
  → PL330 DMA channel 22
  → I2S0 TX → RV1106 DAC/codec
  → PA GPIO/功放 → Speaker
```

### 3.1 DTS 如何组成一张声卡

板级 DTS 的根节点定义：

```dts
acodec_sound: acodec-sound {
    compatible = "simple-audio-card";
    simple-audio-card,name = "rv-acodec";
    simple-audio-card,format = "i2s";
    simple-audio-card,mclk-fs = <256>;
    simple-audio-card,cpu { sound-dai = <&i2s0_8ch>; };
    simple-audio-card,codec { sound-dai = <&acodec>; };
};
```

并启用两端：

- `i2s0_8ch`：CPU DAI；
- `acodec`：片内 codec DAI，含 `pa-ctl-gpios`；
- codec 的 `init-mic-gain = <0x22>`、micbias 等板级模拟参数。

SoC `.dtsi` 中 I2S 节点还声明：

```text
compatible = "rockchip,rv1106-i2s-tdm"
dmas = <&dmac 22>, <&dmac 21>
dma-names = "tx", "rx"
```

即播放用 DMA 22，录音用 DMA 21。

### 3.2 三类驱动怎样 match

音频不是只有 codec 驱动，而是三块组件完成后由 ASoC 绑定：

| 角色 | compatible | 驱动 | 注册结果 |
|---|---|---|---|
| Machine/card | `simple-audio-card` | `sound/soc/generic/simple-card.c` | `snd_soc_card` 和 DAI link |
| CPU DAI | `rockchip,rv1106-i2s-tdm` | `sound/soc/rockchip/rockchip_i2s_tdm.c` | I2S DAI + dmaengine PCM |
| Codec DAI | `rockchip,rv1106-codec` | `sound/soc/codecs/rv1106_codec.c` | ADC/DAC codec component + DAI |
| DMA controller | `arm,pl330` | PL330 dmaengine driver | TX/RX DMA channel |

启动时的关系为：

```text
i2s platform probe
  ├─ devm_snd_soc_register_component()：注册 CPU DAI
  └─ devm_snd_dmaengine_pcm_register()：注册 PCM DMA platform

codec platform probe
  ├─ 读取 PA GPIO、micbias、初始增益
  └─ devm_snd_soc_register_component()：注册 codec DAI

simple-card probe
  ├─ 解析 cpu/codec sound-dai phandle
  ├─ 构造 dai_link：format=i2s，mclk_fs=256
  └─ devm_snd_soc_register_card()
       └─ 等组件齐全后实例化 ALSA card/PCM
```

如果 simple-card 先 probe，而 I2S/codec 尚未注册，ASoC 会返回 probe defer，之后重试。这就是为何日志中偶尔看到 deferred probe 并不一定是故障。

### 3.3 `hw_params` 和 `trigger` 如何落到硬件

应用设置采样率、位宽和通道数后，ALSA/ASoC 调用链大致为：

```text
SNDRV_PCM_IOCTL_HW_PARAMS
  → ASoC soc_pcm_hw_params()
  → simple-card 约束/时钟设置
  → rockchip_i2s_tdm hw_params()
       设置 BCLK/LRCK、sample width、channels、DMA width
  → rv1106 codec hw_params()
       设置 ADC/DAC、采样率和模拟/数字路径
  → dmaengine PCM 配置 cyclic DMA
```

开始录放：

```text
SNDRV_PCM_IOCTL_PREPARE / START
  → ASoC trigger
  → rockchip_i2s_tdm_trigger()
  → dmaengine 启动 cyclic descriptor
  → I2S RX/TX enable
  → codec DAPM 为实际使用路径上电
```

DMA 在 ring buffer 的 period 边界产生完成通知，ALSA 更新硬件指针并唤醒/回调用户态。停止时执行相反顺序，DAPM 会关闭不用的 ADC/DAC、micbias 或输出路径，以降低功耗。

`pa-ctl-gpios` 是扬声器功放控制，且 DTS 标记 `GPIO_ACTIVE_LOW`。gpiod API 使用逻辑有效值，驱动请求“on”时 GPIO 子系统会自动换算为物理低电平。录音链不需要打开 speaker PA，播放链才需要。

### 3.4 内核如何生成 ALSA 设备节点

card 注册成功后通常得到：

```text
/dev/snd/controlC0       mixer/control
/dev/snd/pcmC0D0c       capture，末尾 c
/dev/snd/pcmC0D0p       playback，末尾 p
```

卡名来自 DTS 的 `simple-audio-card,name = "rv-acodec"`。这里的 `C0D0` 只是常见结果；如果系统存在 USB 声卡或其他 card，编号可能改变。诊断和脚本应优先使用 `aplay -l`、`arecord -l` 或 ALSA 名称，而不是假设永远是 card 0。

### 3.5 PortAudio 到 ALSA 的实际路径

Buildroot 的 `echo_mate_defconfig` 启用了 ALSA utilities、PortAudio 和 Opus。当前 PortAudio 使用 ALSA host API，其 `pa_linux_alsa.c` 最终调用：

- `snd_pcm_open()` 打开 capture/playback PCM；
- `snd_pcm_hw_params*()` 设置格式、通道、采样率、period/buffer；
- `snd_pcm_prepare()` / `snd_pcm_start()`；
- mmap 模式用 `snd_pcm_mmap_begin/commit()`；
- 非 mmap fallback 用 `snd_pcm_readi()` / `snd_pcm_writei()`。

因此 `Pa_OpenStream()` 并没有绕开 ALSA，它只是给应用提供跨平台 callback 接口；其下仍是 libasound → ALSA ioctl → ASoC → I2S/codec/DMA。

### 3.6 当前 `AudioProcess` 调用链

公共参数是 16 kHz、mono、signed 16-bit，40 ms 一帧，即：

```text
16000 samples/s × 40 ms = 640 samples
640 samples × 2 bytes = 1280 bytes PCM/frame
```

录音初始化：

```text
AudioProcess::startRecording()
  ├─ Pa_Initialize()
  ├─ Pa_GetDefaultInputDevice()
  ├─ Pa_OpenStream(... paInt16, mono, 16000 Hz, 640 frames ...)
  └─ Pa_StartStream()
       └─ recordCallback()
            └─ 复制 PCM 到 recordedAudioQueue
```

上层状态机的消费方向：

- Listening：从录音队列取 640 个 sample，Opus 编码后通过 WebSocket 发送；
- Idle：同一录音输入可送关键词唤醒模块；
- 状态切换负责开启、停止或清空相应队列。

播放初始化：

```text
WebSocket 收到 Opus packet
  → Opus decoder 生成 PCM，放入 playbackAudioQueue
  → AudioProcess::startPlaying()
       ├─ Pa_Initialize()
       ├─ Pa_GetDefaultOutputDevice()
       ├─ Pa_OpenStream(... paInt16, mono, 16000 Hz ...)
       └─ Pa_StartStream()
            └─ playCallback() 从 playbackAudioQueue 填充输出
```

停止分别调用 `Pa_StopStream()`、`Pa_CloseStream()`、`Pa_Terminate()`。

## 4. 两条链路中容易混淆的边界

### 4.1 DTS match 不等于硬件已经正常

`compatible` 只完成“候选驱动选择”。真正成功还依赖：

- sensor chip ID 可读且正确；
- clock/reset/regulator/GPIO 可获取；
- endpoint graph 完整且格式/lane 一致；
- I2S/codec 的 DAI 能力相容；
- DMA channel 可申请；
- 所需 `.ko` 已装入且依赖满足。

所以排查时应区分“驱动没 match”“probe 失败”“pipeline 未 complete”和“应用参数不兼容”。

### 4.2 控制面与数据面

| 链路 | 控制面 | 数据面 |
|---|---|---|
| Camera | I2C、V4L2 controls、subdev ioctls、rkaiq 参数 | MIPI CSI-2 → CIF/ISP → DMA/vb2 |
| Audio | ALSA mixer、PCM hw_params、ASoC DAPM/trigger | ADC/DAC ↔ I2S ↔ cyclic DMA ↔ PCM ring |

控制面决定硬件怎样工作，数据面搬运大流量内容。不能因为 sensor 是 I2C device 就认为图像经 I2C 传输，也不能因为应用使用 PortAudio 就忽略底层 ALSA/ASoC。

## 5. 建议的板端验证顺序

### 5.1 摄像头

```sh
dmesg | grep -Ei 'sc3336|sc4336|sc530ai|csi|cif|rkisp|dphy'
lsmod | grep -E 'sc3336|sc4336|sc530ai|rkcif|rkisp'
media-ctl -p
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video11 --all
v4l2-ctl -d /dev/video11 --stream-mmap=4 --stream-count=100 --stream-to=/tmp/camera.nv12
```

检查重点：只有实物 sensor 的 ID probe 成功；media link 是 enabled；目标 video node 支持 NV12 和 640×480；连续取帧没有 CSI/ISP overflow。

### 5.2 音频

```sh
cat /proc/asound/cards
aplay -l
arecord -l
amixer -c 0 contents
arecord -D hw:0,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/mic.wav
aplay -D hw:0,0 /tmp/mic.wav
```

检查重点：卡名为 `rv-acodec`；录音/播放支持应用要求的 16 kHz、mono、S16_LE，或 ALSA plug 层能转换；无 underrun/overrun；播放时 PA GPIO 状态正确。

### 5.3 常见故障定位

| 症状 | 优先检查 |
|---|---|
| 没有 sensor 日志 | DTS 是否为实际烧录 DTB、I2C4/pinctrl/clock、sensor 模块是否加载 |
| sensor probe `chip id mismatch` | BOM 型号、I2C 地址、电源/PWDN/MCLK |
| sensor 成功但无 video node | endpoint phandle、CIF/ISP 模块、async notifier complete 日志 |
| 能开 video 但 DQBUF 卡住 | STREAMON 上游失败、MIPI lane/频率、sensor stream 寄存器、ISP link |
| 图像有帧但颜色/曝光异常 | NV12/RAW 格式协商、IQ 文件与模组是否匹配、rkaiq/3A 是否运行 |
| 没有 ALSA card | simple-card、I2S/codec probe，deferred probe，DAI phandle |
| 能录不能播 | playback DAI/DMA 22、DAC route、PA GPIO、mixer |
| 能播不能录 | capture DAI/DMA 21、ADC/micbias、mic gain、mixer |
| PortAudio 无设备 | Buildroot ALSA backend、default PCM 配置、设备占用/权限 |

## 6. 当前实现值得改进的点

1. 摄像头节点不应长期硬编码为 `/dev/video11`，应按 entity/name 或 media graph 发现。
2. 三个同地址 sensor 都为 `okay` 会制造无意义的 probe 失败，应按硬件 SKU 生成唯一启用配置。
3. 应明确 AI 应用启动前由谁启动 rkaiq/加载 IQ；将该依赖写入启动脚本和健康检查。
4. PortAudio 当前用 default input/output device；若产品可能插入 USB 声卡，应按 ALSA card 名 `rv-acodec` 选择，避免默认设备漂移。
5. 建议启动日志打印最终选中的 video entity、像素格式、ALSA host API/device、实际采样参数，便于远程诊断。

## 7. 关键源码索引

### 构建与 DTS

- `rv1106-sdk/.BoardConfig.mk`
- `rv1106-sdk/project/cfg/BoardConfig_IPC/BoardConfig-SD_CARD-Buildroot-RV1106_Echo_Mate-DeskMate.mk`
- `rv1106-sdk/build.sh`
- `rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-echo-mate.dts`
- `rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/rv1106-echo-mate-ipc.dtsi`
- `rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/rv1106.dtsi`
- `rv1106-sdk/sysdrv/source/kernel/arch/arm/configs/echo_rv1106_linux_defconfig`
- `rv1106-sdk/sysdrv/source/buildroot/buildroot-2023.02.6/configs/echo_mate_defconfig`

### 摄像头内核与应用

- `drivers/media/i2c/sc3336.c`
- `drivers/media/i2c/sc4336.c`
- `drivers/media/i2c/sc530ai.c`
- `drivers/phy/rockchip/phy-rockchip-csi2-dphy.c`
- `drivers/phy/rockchip/phy-rockchip-csi2-dphy-hw.c`
- `drivers/media/platform/rockchip/cif/`
- `drivers/media/platform/rockchip/isp/`
- `yolov5_demo/cpp/v4l2_capture.cc`
- `yolov5_demo/cpp/AIcamera_c_interface.cc`

### 音频内核与应用

- `sound/soc/generic/simple-card.c`
- `sound/soc/rockchip/rockchip_i2s_tdm.c`
- `sound/soc/codecs/rv1106_codec.c`
- `drivers/dma/pl330.c`
- `AIChat_demo/Client/Audio/AudioProcess.cc`
- `.../portaudio-190700_20210406/src/hostapi/alsa/pa_linux_alsa.c`


