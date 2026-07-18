# 音频链路 (ALSA + PortAudio + Opus) — 知识卡片

## 1. 一句话概括

RV1106 内置 Codec 通过 I2S0 与 SoC 连接，ALSA 内核驱动管理 DMA 搬运，PortAudio 用户态库封装 ALSA 提供回调式录音/播放，Opus 实现 16kHz 语音低延迟编解码。

---

## 2. 为什么需要它

```
语音交互闭环:
  MIC → ADC → I2S → DMA → PortAudio 回调 → Opus encode → WebSocket → 云端 ASR
  云端 TTS → WebSocket → Opus decode → PortAudio 回调 → I2S → DAC → SPK

没有音频链路 = 聋哑机器人
```

---

## 3. Linux 底层原理

### 3.1 全链路架构

```
Userspace          │  Kernel Space
════════           │  ════════════

PortAudio          │
  ↓                │
libasound          │
  ↓ ioctl          │  ALSA Core (sound/core/pcm_native.c)
                   │    ↓
                   │  负责 format 协商 + buffer 管理
                   │    ↓
                   │  DAI Link (simple-audio-card)
                   │    ├── CPU DAI: rockchip_i2s_tdm
                   │    │    ├ hw_params → 设位宽/采样率/分频
                   │    │    ├ trigger   → START/STOP DMA
                   │    │    └ 写入 TX FIFO → I2S 总线 (BCLK/LRCK/SDO)
                   │    │
                   │    └── Codec DAI: rv1106_codec
                   │         ├ 控制 ADC: MICBIAS → PGA gain → ADC
                   │         ├ 控制 DAC: digital vol → lineout → PA
                   │         └ GPIO PA enable
                   │
                   │  DMA 引擎 (drivers/dma/pl330.c)
                   │    DDR ←→ I2S FIFO
                   │
                   │  I2S 总线 → RV1106 内置 Codec → MIC / SPK
```

### 3.2 ALSA PCM 数据流

```
                  ALSA Ring Buffer (DMA 直接写的 CMA 物理内存)
                  ┌──────────────────────────────────────┐
Record:            │ [████████░░░░░░░░░░░░░░░░░░░░░░░░] │
  DMA 从 I2S RX    │  ↑ hw_ptr      ↑ appl_ptr          │
  写到 buffer      │  (硬件位置)    (用户读位置)         │
                   │                                      │
Playback:          │ [░░░░░░░░░░░░░░░░░░████████░░░░░░] │
  用户写到 buffer   │                   ↑ appl_ptr       │
  DMA 读到 I2S TX   │              (用户写位置)           │
                   └──────────────────────────────────────┘
  
  snd_pcm_readi:  copy buffer[appl_ptr..hw_ptr] → 用户 buffer → 更新 appl_ptr
  snd_pcm_writei: copy 用户 buffer → buffer[appl_ptr..] → 更新 appl_ptr
```

### 3.3 XRUN (Underrun / Overrun)

```
录音 Overrun:
  DMA 写满了 ring buffer → 还有新数据来 → hw_ptr 超过 appl_ptr+size
  → ALSA 报告 EPIPE (xrun) → PortAudio 调 snd_pcm_recover → 重置指针

播放 Underrun:
  appl_ptr 追上了 hw_ptr → 没有数据给 DMA 搬 → 输出上一次的值 (pop 声)
  → ALSA 报告 EPIPE → PortAudio 自动恢复
```

### 3.4 Opus 编解码

```
PCM 16kHz 16bit mono:
  40ms 帧 = 640 samples × 2 bytes = 1280 bytes/帧

Opus encode:
  1280 bytes → ~80 bytes (压缩比 ~16:1)
  → 打包二进制协议头 → WebSocket SendBinary → 云端

Opus decode:
  收到 ~80 bytes → decode → 1280 bytes PCM
  → addFrameToPlaybackQueue → PortAudio 播放
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 PortAudio 流

```c
// 录音流
Pa_OpenStream(&recordStream,
    NULL,                              // 无输出
    &inputParams,                      // 输入参数: 16kHz, 1ch, int16
    sampleRate,                        // 16000
    framesPerBuffer,                   // 640 (40ms)
    paNoFlag,
    recordCallback,                    // 每 40ms 调一次
    this);

Pa_StartStream(recordStream);          // 开始 → ALSA PREPARE → TRIGGER START
Pa_StopStream(recordStream);           // 停止 → TRIGGER STOP

// 播放流 — 对称，只有 outputParams 没有 inputParams
```

### 4.2 回调函数

```c
// 录音回调 — 每 40ms PortAudio 调用一次
int recordCallback(const void *inputBuffer,    // ALSA 填好的 PCM
                   void *outputBuffer,          // 未使用
                   unsigned long framesPerBuffer, // 640
                   const PaStreamCallbackTimeInfo* timeInfo,
                   PaStreamCallbackFlags statusFlags,  // 含 xrun 标志
                   void *userData) {
    
    const int16_t *input = (int16_t*)inputBuffer;
    std::vector<int16_t> frame(input, input + framesPerBuffer * channels);
    
    // 推入队列 — 消费者线程取
    lock(); recordedAudioQueue.push(frame); unlock();
    cv.notify_one();
    
    return paContinue;  // 必须快速返回！回调在 ALSA 中断上下文
}

// 播放回调 — 对称
int playCallback(...) {
    int16_t *output = (int16_t*)outputBuffer;
    
    if (playbackQueue.empty()) {
        // 队列空 — 填静音，不能卡住
        std::fill(output, output + framesPerBuffer, 0);
    } else {
        // 从队列取 frame → copy 到 output
        std::copy(frame.begin(), frame.end(), output);
    }
    return paContinue;
}
```

### 4.3 Opus 编码器

```c
// 初始化
OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));  // 32kbps
opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));

// 编码
opus_encode(enc, pcm, frame_size, opus_data, max_bytes);
// pcm: 640 samples int16 → opus_data: ~80 bytes

// 解码
OpusDecoder *dec = opus_decoder_create(16000, 1, &err);
opus_decode(dec, opus_data, len, pcm_out, frame_size, 0);
```

---

## 5. 完整调用流程

### 5.1 启动录音

```
main.cc → Application::Application() → AudioProcess constructor
  → Pa_Initialize()                      // 初始化 PortAudio
  → Pa_OpenStream(&recordStream, ...)    // 打开录音流
      → ALSA: snd_pcm_open("default")
      → snd_pcm_hw_params(S16_LE, 1ch, 16000)
      → snd_pcm_sw_params(start_threshold=1)
  → Pa_StartStream(recordStream)         // 不立即 start — 等 IdleState 进入
      → snd_pcm_prepare()
      → snd_pcm_start()
```

### 5.2 录音数据流

```
ALSA 中断 → DMA 填满 → snd_pcm_readi 内部
  → PortAudio 回调: recordCallback()
      → copy 640 samples → vector<int16_t>
      → lock → recordedAudioQueue.push(frame)
      → if queue.size() >= 750 → pop() 丢弃最旧帧
      → unlock → notify_one()
      → return paContinue

IdleState::Run():
  while(运行) {
    recordedQueueIsEmpty()? → 等 notify
    getRecordedAudio(data)  → lock → 取出所有帧 → swap 清空 → unlock
    kws_feed_frame(data)    → NPU 推理 → 检测唤醒词
  }
```

### 5.3 播放数据流

```
ws_msg_thread 收到二进制音频:
  UnpackBinFrame → Opus payload
  opus_decode → 640 samples int16_t
  addFrameToPlaybackQueue(pcm)
    → lock → push → if frame短于预期 → resize padding 静音 → unlock

PortAudio 回调: playCallback()
  → lock → if queue.empty() → fill(output, 0) 静音
    else → copy 到 output → pop → unlock
  → return paContinue

ALSA: snd_pcm_writei (PortAudio 内部) → DMA: DDR → I2S TX FIFO → DAC → SPK
```

---

## 6. 我的项目中使用

### 6.1 录音链路（KWS + 语音识别）

```
IdleState (KWS):            ListeningState (ASR):
  startRecording()               startRecording()
    → Pa_StartStream                (已经开着)
  getRecordedAudio(data)         getRecordedAudio(data)
  kws_feed_frame(data)           Opus encode(data)
  → wake_detected                → PackBinFrame → SendBinary
```

### 6.2 播放链路（TTS + 唤醒音效）

```
IdleState Exit:               Speaking:
  loadAudioFromFile(            ws_msg_thread:
    "waked.pcm")                 收到 Opus 二进制帧
    → addFrameToPlaybackQueue       → Opus decode
                                   → addFrameToPlaybackQueue
                                 wait tts_complete
```

### 6.3 关键文件

| 文件 | 作用 |
|------|------|
| [AudioProcess.cc](D:\echo\AIChat_demo\Client\Audio\AudioProcess.cc) | PortAudio 流管理 + 回调 + Opus codec |
| [AudioProcess.h](D:\echo\AIChat_demo\Client\Audio\AudioProcess.h) | 队列定义 + API |
| [Idle.cc](D:\echo\AIChat_demo\Client\Application\UserStates\Idle.cc) | KWS 录音消费 |
| [Listening.cc](D:\echo\AIChat_demo\Client\Application\UserStates\Listening.cc) | ASR 录音消费 |
| [WS_Handler.cc](D:\echo\AIChat_demo\Client\Application\WS_Handler.cc) | 被动视觉 + Opus 音频处理 |
| [rv1106-echo-mate-ipc.dtsi:12-95](D:\echo\rv1106-sdk\sysdrv\source\kernel\arch\arm\boot\dts\rv1106-echo-mate-ipc.dtsi) | DTS 声卡绑定 |

---

## 7. 涉及源码

| 层 | 路径 | 关键内容 |
|----|------|---------|
| Codec 驱动 | `sound/soc/codecs/rv1106_codec.c` | 内置 ADC/DAC 控制 |
| I2S 驱动 | `sound/soc/rockchip/rockchip_i2s_tdm.c` | I2S DMA + 时钟 |
| ALSA 核心 | `sound/core/pcm_native.c` | Ring buffer + ioctl |
| DMA 引擎 | `drivers/dma/pl330.c` | ARM PL330 DMA 搬运 |
| 用户态 ALSA | `buildroot/output/build/alsa-lib-*/src/pcm/pcm.c` | `snd_pcm_readi`/`writei` |
| PortAudio | buildroot 自动编译 | `PaAlsaStreamComponent_BeginPolling` |
| Opus | `opus-1.4/` | encode/decode |

---

## 8. 常见 Bug 及调试方法

### 8.1 没声音 (播放无输出)

```
排查顺序:
  ① cat /proc/asound/cards          → 有声卡吗？
  ② cat /proc/asound/card0/pcm0p/sub0/hw_params  → 格式对吗？
  ③ amixer controls                 → 有音量控制吗？
  ④ amixer cset name='DAC Playback Volume' 0xff → 音量拉最大
  ⑤ 查 GPIO: cat /sys/kernel/debug/gpio | grep pa → 功放开了吗？
  ⑥ 示波器测 I2S BCLK/LRCK 有没有信号
```

### 8.2 录音无声

```
  ① amixer cset name='ADC MICBIAS' 0x90  → 开偏置电压
  ② amixer cset name='ADC PGA Gain' 0x0c → 调到 0dB
  ③ arecord -d 5 -f S16_LE -c 1 -r 16000 test.wav → 裸测录音
  ④ 如果 arecord 也没声 → 硬件问题 (MIC 没焊好/极性反)
```

### 8.3 播放有 pop 声/哒哒声

```
原因: 队列空了 → PortAudio 输出上次 buffer 残留 → 电平跳变 → pop
代码已处理:
  if (playbackQueue.empty())
      fill(output, 0);  // 输出静音而非残留数据

如果还有 pop:
  → DAC 上电时序问题: 改 codec 驱动里的延时
  → PA 使能时序: 先开 PA 后放音 → pop；先放音后开 PA → 无 pop
```

### 8.4 录音/播放不同步

```
原因: PortAudio 两个流有独立的 ALSA ring buffer，没有硬件同步
调试: statusFlags 里有 inputUnderflow/outputUnderflow 标志
修复: 如果严重，用 PortAudio full-duplex 流 (同一个流双向)
```

### 8.5 板端无 ALSA 设备

```
症状: arecord -l → "no soundcards found"
原因:
  ① DTS 里 simple-audio-card 没配或 compatible 不匹配
  ② I2S/acodec 节点 status 不是 "okay"
  ③ 内核没编 sound driver (make menuconfig 检查)
检查:
  dmesg | grep -E "asoc|simple-card|rv1106.*codec|i2s"
```

---

## 9. 性能优化点

| 优化点 | 说明 | 状态 |
|--------|------|------|
| 帧长 40ms | Opus 最佳帧长，延迟和质量平衡 | ✅ |
| Opus bitrate 32kbps | 语音够用，每帧 ~80 bytes | ✅ |
| 队列流控 | 录音 750 帧上限 + 播放空帧静音 | ✅ |
| PortAudio buffer 4×40ms | Pa_SuggestedLatency → 4 个 period | 默认 |
| prealloc_buffer 16KB | DTS bootargs 里设置，减少内核分配 | ✅ |
| Opus complexity 5 | 10 级最强但费 CPU，5 够用 | 默认 |

---

## 10. 面试高频问题

1. **ALSA ring buffer 的工作原理？** hw_ptr (硬件 DMA 位置) 和 appl_ptr (用户读写位置) 围成的环。readi/writei 更新 appl_ptr，DMA 中断更新 hw_ptr。

2. **什么是 xrun？怎么处理？** underrun=播放断流，overrun=录音溢流。ALSA 返回 EPIPE，PortAudio 自动 snd_pcm_recover + 重置指针。

3. **PortAudio 和 ALSA 的关系？** PortAudio 是跨平台音频抽象层，在 Linux 上底层调 libasound → ALSA kernel。

4. **为什么用 Opus 而不是 PCM 直传？** 16kHz PCM 1280 bytes/帧 × 25fps = 32KB/s。Opus 压缩后 ~80 bytes/帧 = 2KB/s，省 16 倍带宽。

5. **录音回调为什么必须快速返回？** 回调在 ALSA 中断上下文执行。如果阻塞 → DMA ring buffer 溢出 → xrun → 丢音频。

6. **队列为空的播放回调怎么处理？** 填静音（zeros），不能阻塞等数据。阻塞 → 回调超时 → PortAudio 认为设备挂了。

7. **RV1106 内置 codec 和外挂 codec 的区别？** 内置直接 APB 寄存器读写；外挂通过 I2C 控制 + I2S 传数据，DTS 里 simple-audio-card/codec 改成外挂的 compatible。

8. **如何调 mic 增益？** `amixer cset name='ADC PGA Gain' 0x0c` (0dB)，增益表在 `rv1106_codec.h` 的 `ACODEC_ADC_L_ALC_GAIN_*` 宏。

9. **Opus encode/decode 在哪线程做的？** 不是 PortAudio 回调里。录音消费在 IdleState/ListeningState 线程，播放生产在 ws_msg_thread。

10. **DTS 里 `coherent_pool=0` 什么意思？影响音频吗？** 关默认 coherent 池，不影响音频 DMA——音频走 CMA，不经过 coherent_pool。

11. **怎么验证音频 DMA 在工作？** `cat /proc/interrupts | grep i2s` 看中断计数在涨。

---

## 11. 补充学习

- ALSA 官方文档: `Documentation/sound/alsa/`
- ASoC (ALSA SoC Layer): `Documentation/sound/soc/`
- Opus RFC 6716: 编码原理
- PortAudio 内部: `src/hostapi/alsa/pa_linux_alsa.c`
- `snd_pcm_readi` 源码: `alsa-lib/src/pcm/pcm.c`
