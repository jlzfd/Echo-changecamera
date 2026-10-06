# RV1106 Echo Mate 音频与 ASoC 驱动指南

## 1. 当前项目音频目标

当前应用使用16 kHz、单声道、16-bit PCM：

```text
录音：Mic → ADC → I2S RX → DMA → ALSA → PortAudio → AudioProcess → Opus
播放：Opus → AudioProcess → PortAudio → ALSA → DMA → I2S TX → DAC → PA/Speaker
```

PortAudio没有绕过内核音频驱动。它只是应用层抽象，Linux后端仍然通过alsa-lib访问ALSA PCM。

## 2. DTS如何描述一张声卡

项目使用`simple-audio-card`把CPU DAI和片内Codec连接起来，核心结构可概括为：

```dts
acodec_sound: acodec-sound {
    compatible = "simple-audio-card";
    simple-audio-card,name = "rv-acodec";
    simple-audio-card,format = "i2s";
    simple-audio-card,mclk-fs = <256>;

    simple-audio-card,cpu {
        sound-dai = <&i2s0_8ch>;
    };

    simple-audio-card,codec {
        sound-dai = <&acodec>;
    };
};
```

三个关键节点：

- `i2s0_8ch`：CPU DAI，负责数字音频串行接口和DMA请求；
- `acodec`：片内Codec，负责ADC、DAC、模拟增益、MicBias和PA控制；
- `acodec_sound`：Machine/Card描述，声明两端怎样连接。

## 3. 为什么ASoC分成三部分

| 组件 | 当前项目 | 责任 |
|---|---|---|
| CPU DAI | `rockchip_i2s_tdm.c` | I2S格式、BCLK/LRCK、FIFO、DMA请求 |
| Codec | `rv1106_codec.c` | ADC/DAC、模拟通路、增益、DAPM、PA GPIO |
| Machine/Card | `simple-card.c` | 连接CPU DAI与Codec DAI，形成声卡 |

这种拆分允许同一个I2S控制器连接不同Codec，也允许同一个Codec用在不同SoC上。

## 4. 从DTB到三类probe

```text
Bootloader传入DTB
  ↓
内核解析i2s、codec和sound节点
  ↓
i2s platform_device  ─→ rockchip_i2s_tdm_probe()
codec platform_device─→ rv1106_codec_probe()
sound platform_device─→ simple_card_probe()
```

### CPU DAI probe

典型工作：

```text
映射I2S寄存器
  → 获取mclk/hclk、reset、IRQ和DMA资源
  → 注册snd_soc_component和snd_soc_dai
  → 注册dmaengine PCM
```

### Codec probe

典型工作：

```text
获取regmap、clock、reset、GPIO
  → 初始化模拟寄存器默认值
  → 注册Codec component和DAI
  → 注册mixer control与DAPM widget/route
```

### simple-card probe

```text
解析cpu/codec的sound-dai phandle
  → 解析format、mclk-fs和主从关系
  → 构造snd_soc_dai_link
  → devm_snd_soc_register_card()
```

如果simple-card先probe，而CPU DAI或Codec尚未注册，会返回`-EPROBE_DEFER`。内核稍后重试，这通常是正常的组件依赖处理。

## 5. ALSA设备节点如何出现

组件全部绑定成功后，ASoC创建`snd_card`和PCM runtime：

```text
snd_soc_card
  └─ snd_soc_pcm_runtime
       ├─ CPU DAI
       ├─ Codec DAI
       └─ platform/dmaengine PCM
              ↓
       /dev/snd/pcmC0D0c  capture
       /dev/snd/pcmC0D0p  playback
```

`C0D0`不是永久保证。应通过`arecord -l`、`aplay -l`或声卡名`rv-acodec`定位设备。

## 6. 应用open到hw_params

应用调用：

```text
Pa_OpenStream()
  → PortAudio ALSA backend
  → snd_pcm_open()
  → ALSA PCM open
  → ASoC startup()
```

设置16 kHz、mono、S16_LE时：

```text
snd_pcm_hw_params()
  → ASoC hw_params
  → CPU DAI hw_params
  → Codec DAI hw_params
  → DMAengine PCM配置
```

关键参数关系：

```text
采样率 Fs = 16000 Hz
有效位宽 = 16 bit
声道数 = 1
MCLK通常 = Fs × mclk-fs = 16000 × 256 = 4.096 MHz
```

实际BCLK还取决于I2S slot宽度和slot数量，不能简单等于`Fs × 有效位宽 × 声道数`。硬件可能使用双slot和32-bit slot。

## 7. trigger后数据如何流动

### 录音

```text
snd_pcm_start()
  → ASoC trigger(START)
  → 配置Codec ADC和DAPM上电路径
  → 启动I2S RX
  → 启动DMA循环描述符
  → DMA把FIFO数据搬到PCM ring buffer
  → period完成中断
  → snd_pcm_period_elapsed()
  → 唤醒/回调用户态
```

### 播放

```text
应用写PCM ring buffer
  → trigger(START)
  → DMA从内存搬到I2S TX FIFO
  → Codec DAC转换
  → DAPM打开播放路径
  → PA GPIO使能
  → Speaker输出
```

## 8. PCM ring buffer、period和延迟

```text
PCM Buffer
┌────────┬────────┬────────┬────────┐
│period 0│period 1│period 2│period 3│
└────────┴────────┴────────┴────────┘
       DMA循环搬运，period完成产生通知
```

- Buffer越大，抗调度抖动能力越强，但基础延迟更高。
- Period越小，回调更频繁、延迟更低，但CPU和调度压力更大。
- 16 kHz、单声道、S16_LE每秒原始数据量为`16000 × 1 × 2 = 32000 byte/s`。
- 当前应用40 ms一帧时，每帧为`16000 × 0.04 = 640 samples`，即1280字节PCM。

## 9. XRUN是什么

播放时DMA需要数据但应用没及时写入，称为underrun；录音时应用没及时取走数据导致旧数据被覆盖，称为overrun，二者统称XRUN。

```text
Playback underrun：网络/TTS供给慢、回调阻塞、Buffer太小
Capture overrun：编码或网络发送阻塞录音消费、调度延迟过大
```

发生XRUN后通常需要`snd_pcm_prepare()`重新准备流。PortAudio可能代为恢复，但持续XRUN仍会表现为断音、爆音或录音缺块。

## 10. 当前AudioProcess调用链

代码入口：`AIChat_demo/Client/Audio/AudioProcess.cc`。

### 录音

```text
AudioProcess::startRecording()
  → Pa_Initialize()
  → Pa_OpenStream(... paInt16, mono, 16000 Hz, 640 frames ...)
  → Pa_StartStream()
  → PortAudio回调线程
  → AudioProcess::recordCallback()
  → PCM队列
  → Opus encode
  → WebSocket
```

### 播放

```text
WebSocket收到Opus
  → Opus decode
  → addFrameToPlaybackQueue()
  → AudioProcess::playCallback()
  → PortAudio
  → ALSA PCM
  → I2S/Codec/PA
```

回调线程中应避免网络访问、磁盘IO、长时间持锁和动态大内存分配，否则容易造成XRUN。

## 11. 音量、增益和“声音太小”

录音太小应按以下层次检查：

```text
Mic硬件/供电
  → MicBias
  → Codec模拟PGA增益
  → ADC数字增益
  → ALSA mixer配置
  → 应用PCM幅值
```

播放太小则检查：

```text
PCM幅值
  → DAC数字音量
  → 模拟输出增益
  → PA GPIO与功放供电
  → Speaker阻抗和硬件连接
```

不要直接在应用里无界放大PCM；这会削波失真。优先确认Codec增益、路由和模拟硬件设置。

## 12. 板端验证顺序

```sh
dmesg | grep -Ei 'asoc|snd|audio|i2s|codec|dma|defer|xrun'
cat /proc/asound/cards
cat /proc/asound/pcm
aplay -l
arecord -l
amixer -c 0 contents
```

先绕过业务应用做最小闭环：

```sh
arecord -D hw:0,0 -f S16_LE -r 16000 -c 1 -d 5 /tmp/mic.wav
aplay -D hw:0,0 /tmp/mic.wav
```

如果硬件只接受特定通道数或格式，可先用`plughw`验证alsa-lib转换层，但最终应确认真实`hw_params`是否符合产品要求。

## 13. 故障定位表

| 现象 | 优先检查 |
|---|---|
| 没有声卡 | sound节点、三个驱动probe、deferred probe、内核配置 |
| 有声卡无PCM | DAI link绑定、DMAengine PCM注册 |
| open失败 | 设备被占用、格式能力、权限、默认设备配置 |
| 录音全零 | MicBias、ADC/DAPM路由、I2S RX、DMA、增益 |
| 播放无声 | DAC/DAPM、PA GPIO极性、功放供电、I2S TX |
| 速度或音调不对 | MCLK/BCLK/LRCK、采样率、slot配置 |
| 断续或爆音 | XRUN、period/buffer、回调阻塞、网络抖动 |
| 左右声道错位 | mono到I2S slot映射、通道配置 |

## 14. 项目源码索引

```text
Kernel config:
  rv1106-sdk/sysdrv/source/kernel/arch/arm/configs/echo_rv1106_linux_defconfig

ASoC:
  rv1106-sdk/sysdrv/source/kernel/sound/soc/generic/simple-card.c
  rv1106-sdk/sysdrv/source/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c
  rv1106-sdk/sysdrv/source/kernel/sound/soc/codecs/rv1106_codec.c

Application:
  AIChat_demo/Client/Audio/AudioProcess.cc
  AIChat_demo/Client/Audio/AudioProcess.h
```



