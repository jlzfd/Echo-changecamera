# 嵌入式多模态机器人项目面试架构手册

## 1. 项目定位

这是一个基于 RV1106 的端云协同多模态机器人系统。

端侧主要负责：

- 麦克风采集
- 音频编码与上传
- TTS 音频播放
- 摄像头采集
- OpenCV / YOLO 本地检测
- 最新帧缓存
- 状态机调度
- WebSocket 实时通信

云端主要负责：

- VAD
- ASR
- LLM 对话
- VLM 视觉理解
- TTS 生成
- function_call / 意图识别

一句话面试描述：

> 这是一个面向嵌入式场景的端云协同多模态机器人项目，端侧负责实时感知和交互调度，云端负责大模型语义理解。

---

## 2. 技术栈

### 客户端

- C / C++
- Linux
- OpenCV
- WebSocket++
- PortAudio
- Opus
- JsonCpp
- pthread / std::thread
- mutex / condition_variable / atomic
- RKNN / YOLOv5
- LVGL（集成在 DeskBot_demo）

### 服务端

- Python
- asyncio
- websockets
- ThreadPoolExecutor
- requests
- DashScope 多模态模型接口

---

## 3. 实际入口与运行方式

项目有两个入口，要面试时讲清楚。

### 3.1 独立调试入口

文件：

- `AIChat_demo/Client/main.cc`

用途：

- 独立调试 AIChat 客户端
- 通过命令行传入：
  - server address
  - port
  - token
  - yolov5 model path

调用链：

```text
main.cc
  -> 创建 Application
  -> Application::Run()
```

### 3.2 实际产品入口

在 DeskBot 集成模式下，真正入口是：

- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/ui_ChatBotPage.c`
- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/app_ChatBotPage.c`

参数来自：

- `DeskBot_demo/utils/system_para.conf`

读取链路：

```text
system_para.conf
  -> sys_load_system_parameters()
  -> ui_system_para.aichat_app_info
  -> start_ai_chat(...)
  -> create_aichat_app(...)
  -> new Application(...)
```

实际运行是：

```text
LVGL UI 页面进入 ChatBot
  -> start_ai_chat()
  -> pthread_create(ai_chat_thread)
  -> run_aichat_app(app_instance)
  -> Application::Run()
```

---

## 4. 整体架构分层

可以分成五层。

### 4.1 UI 层

DeskBot 的 LVGL 页面负责：

- 页面显示
- 页面切换
- 进入 / 离开 ChatBot 页面
- 调用 AIChat C 接口

### 4.2 C 接口桥接层

文件：

- `AIChat_demo/Client/c_interface/AIchat_c_interface.h`
- `AIChat_demo/Client/c_interface/AIchat_c_interface.cc`

职责：

- 把 C++ 的 `Application` 封装成 C 可用接口
- 用 `void*` 句柄保存 C++ 对象
- 提供 create / run / stop / destroy / get_state / get_intent

这是 C/LVGL 工程和 C++ AIChat 核心之间的桥接层。

### 4.3 客户端编排层

核心类：

- `Application`

职责：

- 统一管理状态机
- WebSocket 客户端
- 音频处理器
- 摄像头生命周期
- 最新帧缓存
- 视觉请求 request_id
- 主动/被动视觉链路
- TTS 接收门控

### 4.4 感知与通信层

包括：

- AudioProcess
- WebSocketClient
- WS_Handler
- VisionFrameBuffer
- AIcamera_c_interface

### 4.5 云端服务层

包括：

- WebSocketServer
- AudioHandler
- TextHandler
- ServiceManager
- VADService / ASRService / ChatService / VisionService / TTSService
- TaskManager 线程池

---

## 5. 核心模块职责

## 5.1 Application

`Application` 是项目的客户端总调度类，不是 TCB，不是简单的主线程类。

它更准确的定位是：

- 应用层 Runtime Context
- 生命周期管理器
- Orchestrator

它持有并协调：

- `AudioProcess`
- `StateMachine`
- `EventQueue`
- `IntentQueue`
- `WebSocketClient`
- `VisionFrameBuffer`
- 视觉 request_id
- TTS 接收控制

---

## 5.2 StateMachine

状态包括：

- `startup`
- `idle`
- `listening`
- `thinking`
- `speaking`
- `fault`
- `stopping`

作用：

- 管理语音交互主流程
- 防止状态冲突
- 抑制主动视觉在 busy 状态下打断当前语音链路

---

## 5.3 AudioProcess

职责：

- PortAudio 录音
- PortAudio 播放
- Opus 编码
- Opus 解码
- 二进制协议打包
- 二进制协议解包
- 录音队列 / 播放队列管理

---

## 5.4 WebSocketClient

职责：

- 维护与服务端的长连接
- 发送文本 JSON
- 发送二进制音频帧
- 接收文本 / 二进制消息
- 注册消息回调

---

## 5.5 WS_Handler

职责：

- 分发服务端返回消息
- 处理 `asr` / `chat` / `tts` / `vision_result` / `error`
- 对视觉请求结果做 `request_id` 过滤
- 处理 stale request
- 控制 TTS 音频接收门控

---

## 5.6 VisionFrameBuffer

这是 latest-only frame buffer，用来保证视觉问答永远基于最新画面。

核心设计：

- 只保存最新一帧
- 每帧记录：
  - `seq`
  - `captured_ms`
  - `valid`
- 使用 `mutex` 保护数据
- 使用 `condition_variable` 等待首帧
- `cv::Mat` 通过 `clone()` 做深拷贝

解决的问题：

- 首帧为空
- 旧帧误读
- `cv::Mat` 跨线程共享风险

---

## 5.7 AIcamera_c_interface

职责：

- 启动摄像头线程
- 用 OpenCV 持续抓帧
- 把最新帧推给 Application
- 做运动检测 / YOLO 视觉检测
- 主动触发视觉事件
- 更新 LCD / RGB565 显示缓冲

---

## 5.8 ServiceManager

服务端统一管理：

- `VADService`
- `ASRService`
- `IntentService`
- `ChatService`
- `TTSService`
- `VisionService`
- `TaskManager`
- `audio_queue`
- `ws_send_queue`
- TTS owner
- 最近视觉上下文

---

## 6. WebSocket 协议设计

WebSocket 不是简单“连上就发数据”，而是：

```text
JSON 信令 + Binary 音频流
```

## 6.1 JSON 信令

主要消息类型：

- `hello`
- `functions_register`
- `state`
- `describe_scene`
- `asr`
- `tts`
- `chat`
- `vision_result`
- `error`

### `hello`

客户端启动后发送：

- API key
- 音频参数

示意：

```json
{
  "type": "hello",
  "api_key": "...",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 40
  }
}
```

### `state`

客户端状态同步给服务端：

```json
{
  "type": "state",
  "state": "listening"
}
```

作用：

- 服务端知道当前阶段
- 可以清空 VAD / ASR
- 可以打断 TTS

### `describe_scene`

视觉问答请求核心消息：

```json
{
  "type": "describe_scene",
  "request_id": "vision-17",
  "source": "passive_voice",
  "frame_seq": 883,
  "captured_ms": 12345678,
  "prompt": "请用中文简洁描述当前画面",
  "image": "base64..."
}
```

---

## 6.2 二进制音频协议

音频不是直接发 PCM，而是：

```text
PCM -> Opus -> Binary frame
```

协议头大致结构：

```cpp
struct BinProtocol {
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint8_t payload[];
}
```

字段含义：

- `version`：协议版本
- `type`：类型，音频目前为 0
- `payload_size`：负载大小
- `payload`：Opus 编码后的音频数据

客户端发送链路：

```text
PortAudio 录音
  -> PCM frame
  -> Opus encode
  -> PackBinFrame
  -> WebSocket Binary send
```

客户端接收链路：

```text
Binary frame
  -> UnpackBinFrame
  -> Opus decode
  -> playbackQueue
  -> PortAudio 播放
```

---

## 7. 音频流编解码流程

## 7.1 录音上行

链路：

```text
麦克风
  -> PortAudio record callback
  -> recordedAudioQueue
  -> ListeningState 取一帧
  -> Opus encode
  -> PackBinFrame
  -> WebSocket Binary
  -> Server AudioHandler
```

## 7.2 服务端音频处理

链路：

```text
AudioHandler
  -> unpack_bin_frame
  -> decode_audio
  -> 轻量 RMS 门限过滤
  -> VAD
  -> ASR
  -> task_manager.submit(chat_start_task)
```

## 7.3 下行播放

链路：

```text
LLM/VLM 文本
  -> TTSService
  -> audio_queue
  -> AudioSendThread
  -> encode_audio
  -> pack_bin_frame
  -> WebSocket Binary
  -> Client WS_Handler
  -> Opus decode
  -> playbackQueue
  -> PortAudio 播放
```

---

## 8. 图像传输流程

客户端获取最新帧后：

```text
cv::Mat
  -> resize(448x448)
  -> imencode(".jpg")
  -> Base64
  -> JSON["image"]
```

服务端调用视觉模型时：

```json
{
  "model": "qwen-vl-plus",
  "input": {
    "messages": [
      {
        "role": "user",
        "content": [
          {"image": "data:image/jpeg;base64,..."},
          {"text": "请描述当前画面"}
        ]
      }
    ]
  }
}
```

所以大模型之所以知道输入的是图片，是因为：

- 请求协议里用了 `image` 字段
- 且前缀明确写了 `data:image/jpeg;base64,...`

它不是猜出来的，而是接口协议明确定义的。

---

## 9. request_id 与结果回传机制

视觉请求是异步的，必须解决“旧请求晚返回覆盖新请求”的问题。

### 客户端发请求时

- 生成 `request_id`
- 设置 `active_vision_request_id_`

### 服务端回结果时

尽量带同一个 `request_id`：

- `vision_result`
- `tts(start/end/interrupted)`
- `chat(dialogue=end)`

### 客户端收结果时

如果：

```text
request_id != active_vision_request_id_
```

则认定为 stale request，丢弃。

这解决了：

- 旧视觉结果覆盖当前问题
- 旧 TTS 状态乱入
- 多模态请求串台

---

## 10. 线程关系

这是面试非常容易追问的点。

### 10.1 客户端线程

#### 1. LVGL UI 主线程

职责：

- 页面刷新
- 触摸事件
- 页面切换

不能长时间阻塞。

#### 2. ai_chat_thread

创建者：

- `DeskBot_demo` 的 `start_ai_chat()`

作用：

- 后台运行 `Application::Run()`

如果直接在 UI 线程调用 `Application::Run()`，UI 会卡死，所以要单独建线程。

#### 3. state_trans_thread_

创建者：

- `Application::Run()`

作用：

- 消费 `eventQueue_`
- 驱动状态机迁移

#### 4. ai_camera_thread

创建者：

- `start_ai_camera()`

作用：

- OpenCV 抓帧
- 运动检测 / YOLO
- 推送最新帧

#### 5. WebSocket 网络线程

创建者：

- `WebSocketClient::Run()` 内部

作用：

- 接收服务端消息
- 发送文本和二进制包

#### 6. PortAudio 回调线程

创建者：

- PortAudio 内部

作用：

- 录音回调
- 播放回调

---

### 10.2 服务端线程

#### 1. asyncio 主线程

作用：

- WebSocket 事件循环
- 网络收发

#### 2. AudioSendThread

作用：

- 从 `audio_queue` 取 PCM
- 编码 Opus
- 发回客户端

#### 3. ThreadPoolExecutor worker

作用：

- 跑耗时任务
- chat_start_task
- vision_start_task

---

## 11. 线程之间共享什么数据

主要共享：

- `VisionFrameBuffer.latest_`
- `eventQueue_`
- `IntentQueue_`
- playbackQueue
- recordedAudioQueue
- active_vision_request_id_
- active_tts_request
- 最近视觉上下文

---

## 12. 线程怎么同步

主要机制：

- `mutex`
- `condition_variable`
- `atomic`
- 线程安全队列

### 例子 1：最新帧缓存

- 摄像头线程写
- 视觉问答线程读
- 用 mutex 保护

### 例子 2：首帧同步

- `WaitFirstFrame()`
- `frame_ready_cv_.wait_for(...)`
- `Push()` 中 `notify_all()`

### 例子 3：停止线程

- `threads_stop_flag_`
- `ai_camera_stop`
- `stop_event`

---

## 13. 为什么 AIChat 线程不会阻塞摄像头线程和 WebSocket 线程

核心原则：

```text
阻塞函数只阻塞调用它的线程。
```

`Application::Run()` 是阻塞式函数，如果在 LVGL 线程里调用，会卡 UI。

现在的做法是：

```text
LVGL 线程
  -> pthread_create(ai_chat_thread)
  -> ai_chat_thread 里运行 Application::Run()
```

所以阻塞的是：

- `ai_chat_thread`

而不是：

- UI 线程
- 摄像头线程
- WebSocket 线程

这些线程都是独立线程，由 Linux 内核分别调度。

---

## 14. 最新帧缓存实现逻辑

项目里不是叫 LatestFrameBuffer，而是：

- `VisionFrameBuffer`

设计目的：

- 只缓存最新一帧
- 拒绝历史帧
- 解决首帧为空
- 解决 `cv::Mat` 跨线程共享问题

### 写入逻辑

```cpp
latest_.image = frame.clone();
latest_.seq = ++next_seq_;
latest_.captured_ms = NowMs();
latest_.valid = true;
ready_ = true;
notify_all();
```

### 读取逻辑

```cpp
if (!latest_.valid) return false;
if (NowMs() - latest_.captured_ms > max_age_ms) return false;
out = latest_;
out.image = latest_.image.clone();
```

### 为什么不用队列

因为视觉问答需要的是：

```text
当前画面
```

而不是：

```text
历史画面
```

队列会积压旧帧，导致延迟越来越大。

---

## 15. 500ms 新鲜度判断逻辑

写入帧时：

```cpp
captured_ms = NowMs();
```

读取帧时：

```cpp
NowMs() - captured_ms <= 500
```

则认为这帧是当前可用帧。

超过 500ms 则认为旧帧，不发送给大模型。

这里使用 `steady_clock`，因为：

- 单调递增
- 不受系统时间调整影响

---

## 16. 主动视觉触发流程

链路：

```text
摄像头线程抓帧
  -> OpenCV 运动检测 / YOLO 检测
  -> SubmitActiveVisionEvent()
  -> 检查当前状态是否 idle
  -> 检查 cooldown
  -> SubmitVisionQuestion()
  -> 获取当前最新帧
  -> 发给视觉模型
  -> 生成自然语言提醒
```

关键点：

- 主动视觉不直接打断系统
- 必须进入 Application 统一调度
- busy 状态 suppress，不消耗 cooldown

---

## 17. 被动视觉问答流程

链路：

```text
用户说“你看到了什么”
  -> IntentService 识别 function_call: look_at_environment
  -> 客户端收到 function_call
  -> SubmitVisionQuestion()
  -> 从 VisionFrameBuffer 取 500ms 内最新帧
  -> JPEG + Base64
  -> describe_scene
  -> VisionService 调用视觉大模型
  -> 返回 vision_result
  -> TTS 播报
```

---

## 18. 项目亮点

### 18.1 端云协同设计合理

端侧负责实时采集和事件触发，云端负责大模型理解，符合 RV1106 边缘设备算力边界。

### 18.2 不是玩具 Demo，而是完整链路

已经打通：

- 语音采集
- 音频编解码
- WebSocket
- 状态机
- 摄像头线程
- 最新帧缓存
- 图像传输
- VLM 调用
- TTS 回放

### 18.3 解决了真实系统问题

包括：

- 旧帧
- 首帧为空
- `cv::Mat` 跨线程
- 主动视觉与语音冲突
- 请求串台
- TTS 打断

### 18.4 C/C++ 混合工程桥接

通过 C 接口把 DeskBot C/LVGL 工程和 AIChat C++ 核心解耦。

---

## 19. 项目难点

### 19.1 旧帧问题

用户问的是“现在画面”，不是历史画面。

解决：

- latest-only buffer
- 500ms 新鲜度校验

### 19.2 首帧为空问题

摄像头线程初始化异步。

解决：

- condition_variable
- WaitFirstFrame(timeout)

### 19.3 `cv::Mat` 跨线程问题

解决：

- 写入时 clone
- 读取时 clone

### 19.4 多模态请求串台

解决：

- request_id
- active_vision_request_id_
- stale request drop

### 19.5 语音链路和视觉链路冲突

解决：

- 状态机判断
- cooldown
- busy suppress

### 19.6 TTS 被用户打断

解决：

- 服务端检测音频门限
- interrupt_active_tts()
- 客户端收到 interrupted 后清空播放

---

## 20. 面试高频问题

### Q1：你的项目里 WebSocket 协议怎么设计的？

答：

> 底层是 WebSocket 长连接，应用层是 JSON 信令加二进制 Opus 音频流混合协议。JSON 负责 hello、state、describe_scene、asr、tts、chat、vision_result 等控制语义，Binary 负责实时音频帧收发。

### Q2：图片是怎么传给大模型的？

答：

> 客户端从最新帧缓存取图后，用 OpenCV 压成 JPEG，再做 Base64 编码，通过 JSON 的 image 字段发给服务端；服务端再按视觉模型的多模态接口格式包装成 `data:image/jpeg;base64,...` 传给 qwen-vl-plus。

### Q3：为什么要用 Opus？

答：

> 因为 Opus 对语音场景压缩率高、延迟低、实时性好，比直接传 PCM 更省带宽，也更适合 WebSocket 长连接实时语音交互。

### Q4：为什么不用 HTTP 拉 TTS 音频文件？

答：

> 因为 TTS 结果是流式生成的，WebSocket 可以边合成边回传音频帧，客户端边收边播，延迟更低。

### Q5：为什么用状态机？

答：

> 因为这是多线程多模态系统，不是单函数调用流程。状态机可以清晰控制 listening、thinking、speaking 等阶段，避免录音、播报和主动视觉互相打断。

### Q6：为什么 RV1106 不能直接跑大模型？

答：

> RV1106 更适合轻量本地检测和实时采集，不适合直接运行大视觉语言模型或大语言模型，所以项目采用端云协同，把复杂语义理解放到云端。

### Q7：项目中最大的工程难点是什么？

答：

> 最大难点不是单个模型调用，而是多模态系统中的并发一致性问题，比如旧帧、空帧、状态冲突、异步结果乱序和 TTS 串台。我重点通过 VisionFrameBuffer、状态机、request_id 和线程同步机制来解决。

### Q8：如果继续优化，你会做什么？

答：

> 我会继续做三类优化：第一是协议层，把二进制音频也绑定 request_id 或 stream_id；第二是配置层，把 model_path、server 地址、token、cooldown 等全部配置化；第三是调度层，把视觉事件抽象成统一事件中心，支持优先级和更多事件类型。

---

## 21. 建议重点熟背的内容

如果面试时间有限，优先记住这些：

1. 项目定位与端云协同思路
2. 实际入口：DeskBot UI，不只是 AIChat main.cc
3. WebSocket 混合协议：JSON + Binary
4. 音频链路：PCM -> Opus -> Binary -> ASR / TTS -> Opus -> Playback
5. 图像链路：cv::Mat -> JPEG -> Base64 -> JSON -> VLM
6. VisionFrameBuffer 为什么存在
7. request_id 怎么解决串台
8. 主动视觉如何避免打断语音
9. 线程有哪些、谁创建、谁阻塞
10. 旧帧 / 首帧为空 / 多线程 / 状态机 这几个难点怎么答

---

## 22. 一分钟总结版本

> 这是一个基于 RV1106 的端云协同多模态机器人项目。端侧使用 C++、Linux、OpenCV、WebSocket 和 RKNN/YOLO，负责音频采集、Opus 编解码、摄像头采集、本地视觉检测、最新帧缓存和状态机调度；服务端使用 Python WebSocket 接入 VAD、ASR、LLM、VLM 和 TTS，实现语音对话、视觉问答和主动视觉提醒。  
>
> 项目里我主要负责嵌入式客户端链路设计，包括 Application 编排层、状态机、音频和 WebSocket 协议、最新帧缓存以及主动 / 被动视觉流程。技术上比较有代表性的点是：WebSocket 采用 JSON + Binary 混合协议，音频走 Opus 编解码，图像走 JPEG + Base64，多线程下通过 VisionFrameBuffer、request_id、mutex、condition_variable 和状态机解决旧帧、空帧、请求串台和语音视觉冲突问题。
