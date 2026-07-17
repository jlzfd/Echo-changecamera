# 智能多模态语音对话机器人面试知识点手册

## 1. 项目定位

这是一个基于 RV1106 / Linux 的端云协同多模态机器人项目。

项目目标不是单纯做一个聊天 Demo，而是把以下能力打通：

- 语音采集、编码、上传
- 服务端 ASR / LLM / TTS
- 摄像头采集与最新帧缓存
- 视觉问答
- OpenCV / YOLO 主动视觉触发
- WebSocket 实时通信
- 状态机驱动的多线程协作

一句话面试描述：

> 这是一个端云协同的多模态机器人系统，端侧负责实时音视频采集、状态控制和本地视觉检测，云端负责 ASR、对话生成、视觉理解和 TTS。

---

## 2. 技术栈

### 2.1 客户端

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

### 2.2 服务端

- Python
- asyncio
- websockets
- ThreadPoolExecutor
- requests
- DashScope / 视觉大模型接口

---

## 3. 项目入口与运行方式

项目有两个入口，面试时要区分清楚。

### 3.1 独立调试入口

文件：

- `AIChat_demo/Client/main.cc`

作用：

- 独立调试 AIChat 客户端
- 通过命令行传入 `server_address / port / token / model_path`

调用链：

```text
main.cc
  -> 创建 Application
  -> Application::Run()
```

### 3.2 实际产品入口

在 DeskBot 集成模式下，真正入口是 LVGL UI 页面：

- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/ui_ChatBotPage.c`
- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/app_ChatBotPage.c`

配置来源：

- `DeskBot_demo/utils/system_para.conf`

链路：

```text
system_para.conf
  -> sys_load_system_parameters()
  -> ui_system_para.aichat_app_info
  -> start_ai_chat(...)
  -> create_aichat_app(...)
  -> new Application(...)
```

运行方式：

```text
LVGL 页面进入 ChatBot
  -> start_ai_chat()
  -> pthread_create(ai_chat_thread)
  -> run_aichat_app(app_instance)
  -> Application::Run()
```

---

## 4. 整体架构分层

### 4.1 UI 层

DeskBot 的 LVGL 页面负责：

- 页面显示
- 页面切换
- 进入 / 退出 ChatBot 页面
- 调用 AIChat 的 C 接口

### 4.2 C 接口桥接层

文件：

- `AIChat_demo/Client/c_interface/AIchat_c_interface.h`
- `AIChat_demo/Client/c_interface/AIchat_c_interface.cc`

职责：

- 把 C++ `Application` 封装成 C 可调用接口
- 用 `void*` / 句柄方式屏蔽 C++ 类细节
- 提供 create / run / stop / destroy / get_state / get_intent

可讲点：

- 为什么要 `extern "C"`
- 为什么 C 侧不能直接拿 `Application*`
- 为什么要用不透明句柄

### 4.3 应用调度层

核心类：

- `Client/Application/Application.h`
- `Client/Application/Application.cc`

职责：

- 管理 WebSocket 客户端
- 管理状态机
- 管理音频录制 / 播放
- 管理摄像头生命周期
- 管理最新帧缓存
- 管理主动 / 被动视觉请求
- 管理 request_id、防串台、视觉上下文

一句话：

> Application 是客户端应用级编排器，不是线程控制块。

### 4.4 通信与协议层

客户端：

- `Client/WebSocket/WebsocketClient.*`
- `Client/Application/WS_Handler.cc`

服务端：

- `Server/ws_server.py`
- `Server/handle/text_handler.py`
- `Server/handle/audio_handler.py`

职责：

- WebSocket 连接建立
- JSON 信令收发
- 二进制音频帧收发
- 按消息类型分发到业务处理模块

### 4.5 音频链路层

客户端：

- `Client/Audio/AudioProcess.*`

服务端：

- `Server/handle/audio_handler.py`
- `Server/threads/audio_send_thread.py`

职责：

- 录音
- PCM / Opus 编解码
- 二进制协议封包
- 音频播放
- TTS 音频发送与回放

### 4.6 视觉链路层

客户端：

- `Client/Application/VisionFrameBuffer.*`
- `yolov5_demo/cpp/AIcamera_c_interface.cc`

服务端：

- `Server/services/vision_service.py`

职责：

- 摄像头采集
- 最新帧缓存
- JPEG / Base64 编码
- 视觉请求发送
- OpenCV 主动视觉事件触发
- 视觉大模型调用

---

## 5. WebSocket 通信知识点

### 5.1 为什么用 WebSocket

因为项目是实时交互型系统，需要：

- 长连接
- 双向通信
- 低延迟
- 同时传输控制消息和音频流

所以选择：

```text
WebSocket 长连接
  ├─ JSON 文本消息：信令
  └─ Binary 消息：Opus 音频
```

### 5.2 JSON 信令怎么设计

统一使用：

```json
{
  "type": "..."
}
```

客户端发给服务端的典型消息：

- `hello`
- `functions_register`
- `state`
- `describe_scene`

服务端回给客户端的典型消息：

- `auth`
- `vad`
- `asr`
- `chat`
- `tts`
- `vision_result`
- `error`

### 5.3 典型 JSON 结构

#### hello

```json
{
  "type": "hello",
  "api_key": "xxx",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 40
  }
}
```

作用：

- 握手
- 同步音频参数

#### state

```json
{
  "type": "state",
  "state": "listening"
}
```

作用：

- 通知服务端当前客户端状态
- 让服务端 reset / interrupt / 对齐链路

#### describe_scene

```json
{
  "type": "describe_scene",
  "request_id": "vision-12",
  "source": "passive_voice",
  "frame_seq": 123,
  "captured_ms": 456789,
  "prompt": "Please describe the current camera frame in Chinese.",
  "image": "<base64>"
}
```

作用：

- 发起视觉理解请求

#### tts

```json
{
  "type": "tts",
  "state": "start",
  "request_id": "vision-12"
}
```

或：

```json
{
  "type": "tts",
  "state": "interrupted",
  "reason": "barge_in",
  "request_id": "vision-12"
}
```

作用：

- 控制二进制音频流的播放生命周期

### 5.4 request_id 怎么用

request_id 主要用于：

- 视觉请求结果归属
- 防止旧请求晚返回覆盖新请求
- 让 `tts / vision_result / dialogue_end / error` 绑定到同一个请求

核心思想：

```text
发请求时生成 request_id
返回结果时带同一个 request_id
客户端只处理当前 active request
旧请求返回直接丢弃
```

可讲点：

- 为什么多模态系统容易串台
- 为什么光靠时间顺序不够
- 为什么 request_id 是最稳的治理方式

---

## 6. 语音链路知识点

### 6.1 整体语音链路

```text
麦克风采集 PCM
  -> PortAudio
  -> Opus 编码
  -> 二进制封包
  -> WebSocket SendBinary
  -> 服务端解包
  -> VAD
  -> ASR
  -> LLM
  -> TTS
  -> PCM 放入发送队列
  -> Opus 编码
  -> WebSocket Binary 回客户端
  -> Opus 解码
  -> 播放队列
  -> PortAudio 播放
```

### 6.2 录音参数怎么设置

来源有两种：

- 独立调试：`main.cc` 默认参数
- DeskBot 集成：`system_para.conf`

关键参数：

- `sample_rate = 16000`
- `channels = 1`
- `frame_duration = 40ms`

这些参数会传给：

- PortAudio 输入 / 输出流
- Opus 编码器 / 解码器
- 服务端 hello 握手消息

### 6.3 为什么选 Opus

优点：

- 低码率
- 低延迟
- 适合语音
- 网络传输成本低

面试可讲：

> 项目里用 Opus 是因为语音场景不需要无损音频，更关心实时性和带宽，Opus 在低延迟语音传输场景下比较合适。

### 6.4 二进制音频帧怎么传

不是把 PCM 直接塞进 JSON，而是：

```text
PCM
  -> Opus
  -> 自定义二进制协议头
  -> WebSocket Binary
```

原因：

- JSON 不适合高频音频流
- Base64 会额外膨胀体积
- Binary 更直接

### 6.5 barge-in / TTS interrupt

当前项目已经做了部分能力：

- 服务端收到高能量语音时可中断当前 TTS
- 服务端会发 `tts: interrupted`
- 客户端收到后清播放队列

但严格意义上的“speaking 时持续录音并立即打断”还未完全闭环，因为客户端状态机目前并不是完整双工录音。

面试可诚实说：

> 我已经把打断通路和音频 owner 清理机制接好了，但真正的全双工 barge-in 还需要进一步改状态机和录音策略。

---

## 7. 摄像头链路与视觉链路知识点

### 7.1 摄像头线程做什么

摄像头线程负责：

- 打开 `VideoCapture`
- 持续采集帧
- 写入最新帧缓存
- 做运动检测 / YOLO 检测
- 触发主动视觉事件
- 更新显示缓冲

### 7.2 为什么要做 LatestFrameBuffer

核心问题：

- 主线程拿空帧
- 视觉问答拿旧帧
- `cv::Mat` 跨线程共享有风险

所以设计了 latest-only 缓存：

- 只保留最新一帧
- 每帧带 `seq / captured_ms`
- `mutex` 保护
- `condition_variable` 等首帧
- `cv::Mat clone()` 深拷贝

### 7.3 首帧为空怎么解决

做法：

- `Push()` 写入首帧时设置 `ready_` 和 `valid`
- `notify_all()`
- `Application::StartCamera()` 中 `WaitFirstFrame(timeout)`

优点：

- 不靠 `sleep` 硬等
- 有帧时立刻唤醒
- 摄像头异常时不会永久阻塞

### 7.4 旧帧问题怎么解决

做法：

- 每帧记录 `captured_ms`
- 视觉请求读取时只允许 `max_age_ms` 内的新鲜帧

例如：

```text
NowMs() - captured_ms <= 500ms
```

才允许发送给视觉模型。

### 7.5 为什么阈值是 500ms

这不是理论常数，而是工程折中：

- 太小：容易频繁误判“无可用帧”
- 太大：旧帧风险明显升高

500ms 的含义是：

> 这帧仍然可以代表“当前场景”。

### 7.6 图像怎么传给视觉大模型

链路：

```text
cv::Mat
  -> resize(448x448)
  -> cv::imencode(".jpg")
  -> Base64
  -> JSON image 字段
  -> 服务端包装成 image + text
  -> 视觉大模型
```

这里不是 MCP，而是：

```text
自定义 WebSocket 业务协议
  + 服务端 HTTP 调用视觉模型 API
```

### 7.7 为什么先缩放到 448x448

工程原因：

- 压缩更快
- 传输更轻
- 对场景描述已经够用
- 不至于像过度压缩那样损失太多语义

---

## 8. 状态机知识点

### 8.1 状态机有哪些状态

客户端主要状态：

- `startup`
- `idle`
- `listening`
- `thinking`
- `speaking`
- `fault`
- `stopping`

### 8.2 状态机怎么切换

不是谁想改就改，而是：

```text
其他线程产生日志/音频/网络事件
  -> eventQueue_.Enqueue(event)
  -> 状态机线程 Dequeue()
  -> HandleEvent()
  -> 查表
  -> old.Exit()
  -> new.Enter()
```

### 8.3 典型语音链路状态流

```text
startup
  -> idle
  -> wake_detected
  -> speaking        （播放唤醒音）
  -> speaking_end
  -> listening
  -> asr_received
  -> thinking
  -> speaking_msg_received
  -> speaking
  -> dialogue_end
  -> idle
```

### 8.4 主动视觉状态流

优化后的目标路径：

```text
idle
  -> vision_detected
  -> thinking
  -> ThinkingState 中发 describe_scene
  -> speaking_msg_received
  -> speaking
  -> dialogue_end
  -> idle
```

### 8.5 被动视觉为什么不靠 vision_detected

因为被动视觉是用户先说一句话，所以它先经过：

```text
listening -> asr_received -> thinking
```

然后服务端识别出 `function_call = look_at_environment`，客户端在 thinking 阶段发视觉请求。

所以：

- 被动视觉：先 ASR，再视觉
- 主动视觉：没有 ASR，要靠 `vision_detected`

### 8.6 状态机的价值

- 控制录音、播放、上传的生命周期
- 保证状态清晰
- 避免多个线程同时乱改状态
- 让主动视觉、被动视觉、语音链路可以对齐

---

## 9. 客户端线程模型知识点

### 9.1 客户端主要线程

1. LVGL/UI 主线程
2. AIChat 后台线程 `ai_chat_thread`
3. 状态机线程 `state_trans_thread_`
4. 摄像头线程 `ai_camera_thread`
5. WebSocket 网络线程
6. PortAudio 回调线程

### 9.2 线程之间怎么通信

主要有四种方式：

#### 1）事件队列

用于状态控制：

```text
WS_Handler / 状态模块 / 其他逻辑
  -> eventQueue_
  -> 状态机线程
```

#### 2）最新帧缓存

用于摄像头线程和业务线程共享图像：

```text
camera thread -> VisionFrameBuffer -> business thread
```

#### 3）mutex / atomic

用于：

- request_id
- 视觉上下文
- stop flag
- audio accept flag

#### 4）播放队列 / 录音队列

用于：

- PCM 播放消费
- PCM 录音生产

### 9.3 这些线程是不是“用户线程”

准确说法：

> 它们是应用层创建的 pthread / std::thread / Python threading 线程，运行在用户态，但最终由 Linux 内核调度，不是纯用户级绿色线程。

---

## 10. 主动视觉与被动视觉知识点

### 10.1 被动视觉

用户说：

- “你看到了什么”
- “现在画面里有什么”

链路：

```text
录音 -> ASR -> thinking
  -> function_call = look_at_environment
  -> SubmitVisionQuestion()
  -> describe_scene
  -> vision_result + tts
```

### 10.2 主动视觉

摄像头线程检测到：

- motion
- person
- 目标物体

链路：

```text
OpenCV / YOLO 检测事件
  -> SubmitActiveVisionEvent()
  -> cooldown / idle 检查
  -> vision_detected
  -> thinking
  -> 发视觉请求
```

### 10.3 为什么主动视觉只在 idle 触发

因为如果系统正在：

- speaking
- thinking
- listening

再触发主动视觉，会造成：

- 插播
- 串话
- 状态混乱

所以主动视觉只在空闲时触发更稳。

---

## 11. 典型工程难点

### 11.1 首帧为空

原因：

- 摄像头线程和业务线程初始化不同步

解决：

- `condition_variable + ready/valid + timeout`

### 11.2 回答旧帧

原因：

- 视觉问答误用了历史帧

解决：

- latest-only 缓存 + 时间戳新鲜度判断

### 11.3 `cv::Mat` 跨线程隐患

原因：

- `cv::Mat` 默认浅拷贝 / 引用计数

解决：

- 写入和读取都 `clone()`

### 11.4 请求串台

原因：

- 旧视觉请求晚返回
- 旧 TTS 晚到

解决：

- `request_id`
- active request 校验
- TTS owner / interrupt 机制

### 11.5 视觉和语音链路冲突

原因：

- OpenCV 主动事件与 speaking/thinking 冲突

解决：

- 状态机检查
- cooldown
- 只在 idle 触发主动视觉

---

## 12. 高频面试问题与答题思路

### Q1：这个项目是你自己做的还是老师项目？

推荐说法：

> 这是我自己自学并持续完善的项目，不是老师直接给的成品项目。我参考过开源代码和资料，但项目理解、功能集成、稳定性优化和问题解决主要是我自己推进的。

### Q2：开发中有没有用 AI 辅助？

推荐说法：

> 有，但我把 AI 当作分析和查缺补漏工具，主要用来辅助梳理思路、做代码 review、比较方案，不是直接替我完成项目。最终接入、调试和工程化落地还是我自己完成的。

### Q3：为什么不用端侧直接跑大模型？

推荐说法：

> RV1106 更适合做实时采集、OpenCV / YOLO 检测和边缘事件触发，不适合直接跑大语言模型或大视觉语言模型。所以我采用端云协同：端侧负责实时感知，云端负责语义理解。

### Q4：为什么不用帧队列，而只保留最新帧？

推荐说法：

> 因为视觉问答要的是当前场景，而不是历史场景。用队列会导致帧堆积和延迟扩大，所以我采用 latest-only 缓存，并通过时间戳保证新鲜度。

### Q5：为什么 JSON 和 binary 分开？

推荐说法：

> JSON 适合信令和状态同步，binary 更适合高频音频流。这样协议职责更清晰，也能减少音频传输开销。

### Q6：为什么 500ms 合理？

推荐说法：

> 500ms 是一个实时性和可用性的工程折中值。太小会频繁误判无帧，太大又会明显变成旧画面，所以我把它作为“当前场景仍然可信”的阈值。

---

## 13. 你最值得讲的亮点

建议优先讲这 5 个：

1. 端云协同多模态架构
2. WebSocket 文本 / 二进制双通道协议
3. LatestFrameBuffer 解决首帧为空和旧帧问题
4. 状态机驱动语音与视觉链路协作
5. request_id 解决多模态请求串台

---

## 14. 一分钟面试总结

> 这个项目是一个基于 RV1106 的端云协同多模态机器人系统。端侧用 C++、Linux、OpenCV、WebSocket、PortAudio 和 RKNN/YOLO，负责音视频采集、本地视觉检测、状态机调度和实时通信；云端用 Python 负责 ASR、LLM、视觉理解和 TTS。  
> 我重点做的是客户端多模态链路和工程化稳定性，包括 WebSocket JSON/二进制协议、语音录制播放链路、摄像头线程接入、最新帧缓存、被动视觉问答、主动视觉触发、状态机协同以及 request_id 防串台。  
> 项目里比较典型的难点是首帧为空、旧帧误答、`cv::Mat` 跨线程风险和多模态状态冲突，我通过 `condition_variable`、latest-only frame buffer、`clone()`、状态机和 request_id 等机制把这些问题逐步收敛掉了。

