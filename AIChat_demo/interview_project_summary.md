# 智能多模态语音对话机器人面试总结

## 1. 项目定位

本项目是一个基于 RV1106 边缘设备的端云协同多模态机器人系统。端侧负责实时感知和交互控制，云端负责大模型语义理解。

项目能力包括：

- 语音唤醒、录音、上传、ASR 识别、LLM 对话、TTS 播放。
- 摄像头采集、OpenCV 预处理、本地视觉检测。
- 用户语音触发视觉问答，例如“你看到了什么”。
- OpenCV / YOLO 检测驱动的主动视觉触发，例如检测到运动或人进入画面后主动上报。
- WebSocket 长连接通信，支持 JSON 信令和二进制 Opus 音频流。
- 状态机管理 startup、idle、listening、thinking、speaking 等交互状态。

面试一句话描述：

> 这是一个基于 RV1106 的端云协同多模态机器人系统。端侧用 C++、Linux、OpenCV、WebSocket 和 RKNN/YOLO 做音视频采集、本地检测、最新帧缓存和交互调度；服务端用 Python WebSocket 接入 ASR、LLM、VLM 和 TTS，实现语音对话、视觉问答和主动环境感知。

## 2. 技术栈

客户端：

- C++ / C
- Linux
- OpenCV
- WebSocket++
- PortAudio
- Opus
- JsonCpp
- pthread / std::thread
- mutex / condition_variable / atomic
- RKNN / YOLOv5
- LVGL UI，集成在 DeskBot_demo 中

服务端：

- Python
- asyncio
- websockets
- ThreadPoolExecutor
- VAD / ASR / Chat / TTS / VisionService
- DashScope qwen-vl-plus 视觉大模型接口

构建与集成：

- AIChat_demo/Client 使用 CMake 构建为 AIChatCore 和 AIchat-c-interface。
- DeskBot_demo 通过 add_subdirectory 引入 AIChat_demo/Client。
- DeskBot_demo 的 LVGL 页面通过 C 接口调用 C++ Application。

## 3. 实际入口与参数传递

项目有两个入口，需要在面试里区分清楚。

### 3.1 AIChat 独立调试入口

文件：

- `AIChat_demo/Client/main.cc`

用途：

- 独立运行 AIChat 客户端。
- 通过命令行传入 server address、port、token、yolov5 model path。

调用链：

```text
main.cc
  -> 解析 address / port / token / model_path
  -> 创建 Application
  -> app.Run()
```

### 3.2 DeskBot 实际 UI 入口

实际产品运行时，入口是 DeskBot_demo 的 LVGL UI 页面，而不是 AIChat_demo/Client/main.cc。

关键文件：

- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/ui_ChatBotPage.c`
- `DeskBot_demo/gui_app/pages/ui_ChatBotPage/app_ChatBotPage.c`
- `AIChat_demo/Client/c_interface/AIchat_c_interface.cc`

实际调用链：

```text
用户进入 ChatBot 页面
  -> ui_ai_chat_app_init()
  -> start_ai_chat(...)
  -> create_aichat_app(...)
  -> new Application(...)
  -> pthread_create(ai_chat_thread)
  -> run_aichat_app(app_instance)
  -> Application::Run()
```

参数来源：

- `DeskBot_demo/utils/system_para.conf`
- `sys_load_system_parameters()`
- `ui_system_para.aichat_app_info`

配置示例：

```ini
AIChat_server_url=192.168.31.158
AIChat_server_port=8000
AIChat_server_token=123456
AIChat_Client_ID=00:11:22:33:44:55
aliyun_api_key=...
AIChat_protocol_version=2
AIChat_sample_rate=16000
AIChat_channels=1
AIChat_frame_duration=40
```

注意：

- DeskBot UI 集成路径当前没有显式传入 `yolov5_model_path`。
- C 接口仍走旧构造函数，Application 默认使用 `./model/yolov5.rknn`。
- 可优化点：把 `AIChat_yolov5_model_path=./model/yolov5.rknn` 加入 system_para.conf，并扩展 C 接口传入。

## 4. 核心架构

整体架构：

```text
DeskBot LVGL UI
  -> C 接口 AIchat_c_interface
  -> C++ Application
  -> AudioProcess / WebSocketClient / StateMachine / VisionFrameBuffer / AIcamera
  -> Python WebSocket Server
  -> ASR / LLM / VLM / TTS
```

核心模块：

### 4.1 Application

职责：

- 客户端应用编排层，不是 TCB，也不是单纯主线程类。
- 管理音频、WebSocket、状态机、摄像头、最新帧缓存、视觉请求、request_id 和 TTS 接收门控。

面试说法：

> Application 是应用层 Runtime Context / Orchestrator。它不保存线程寄存器、栈指针、调度优先级等 TCB 信息，而是统一协调业务模块和线程生命周期。

### 4.2 C 接口层

关键文件：

- `AIChat_demo/Client/c_interface/AIchat_c_interface.h`
- `AIChat_demo/Client/c_interface/AIchat_c_interface.cc`

设计思路：

- DeskBot 是 C / LVGL 工程，AIChat 核心是 C++ Application。
- C 不能直接调用 C++ class，所以用 `extern "C"` 暴露 C ABI。
- C++ 内部 `new Application(...)`，返回 `void*` 句柄给 C。
- C 侧只保存 `void* app_instance`，调用 run/stop/destroy/get_state 时再传回 C++。

典型模式：

```text
Application* <-> void*
```

面试说法：

> C 接口层使用不透明句柄模式，把 C++ Application 封装成 C 可以操作的 void* handle。这样 LVGL C 页面只负责启动、停止、查询状态和获取意图，不需要知道 C++ 类内部结构。

### 4.3 StateMachine

核心状态：

```text
startup -> idle -> listening -> thinking -> speaking -> idle
```

职责：

- 控制语音交互生命周期。
- 避免录音、思考、播报状态混乱。
- 主动视觉触发时先检查状态，只有 idle 才允许提交。

### 4.4 VisionFrameBuffer

职责：

- 线程安全的 latest-only frame buffer。
- 只保存最新帧，不堆积历史帧。
- 支持首帧等待、帧新鲜度判断、cv::Mat 深拷贝。

核心字段：

```cpp
struct FrameSnapshot {
    cv::Mat image;
    uint64_t seq;
    int64_t captured_ms;
    bool valid;
};
```

核心同步机制：

- `std::mutex`
- `std::condition_variable`
- `cv::Mat::clone()`
- `steady_clock` 时间戳

### 4.5 AIcamera_c_interface

职责：

- 启动摄像头线程。
- OpenCV VideoCapture 采集图像。
- 向 Application 推送最新帧。
- 做运动检测 / YOLO 检测。
- 触发主动视觉事件。
- 支持 attach/detach consumer，避免摄像头线程长期持有失效 Application 指针。

### 4.6 WebSocket 通信层

客户端：

- WebSocketClient 负责连接、发送 JSON、发送二进制音频。
- WS_Handler 负责处理服务端返回。

服务端：

- WebSocketServer 负责连接、鉴权、消息分发和发送队列。
- AudioHandler 处理二进制音频。
- TextHandler 处理 JSON 消息，例如 describe_scene。
- TaskManager 使用 ThreadPoolExecutor 执行耗时任务。

## 5. 关键运行链路

### 5.1 语音对话链路

```text
PortAudio 采集 PCM
  -> Opus 编码
  -> WebSocket Binary 发送
  -> Server AudioHandler 解码
  -> VAD 判断语音结束
  -> ASR 生成文本
  -> ChatService 调用 LLM
  -> TTSService 生成语音
  -> AudioSendThread 编码 Opus
  -> WebSocket Binary 返回
  -> Client WS_Handler 解码
  -> AudioProcess 播放
```

### 5.2 被动视觉问答链路

```text
用户说“你看到了什么”
  -> 服务端识别 function_call: look_at_environment
  -> 客户端 WS_Handler 收到 function_call
  -> Application::SubmitVisionQuestion()
  -> VisionFrameBuffer::GetLatest(snapshot, 500)
  -> JPEG 压缩 + Base64
  -> WebSocket JSON: describe_scene
  -> Server TextHandler
  -> TaskManager 提交 vision_start_task
  -> VisionService 调用 qwen-vl-plus
  -> 返回 vision_result 和 TTS
```

### 5.3 主动视觉触发链路

```text
摄像头线程持续采集
  -> OpenCV / YOLO 检测运动或目标
  -> Application::SubmitActiveVisionEvent()
  -> 检查当前状态是否 idle
  -> 检查 cooldown
  -> SubmitVisionQuestion(prompt, "active_opencv")
  -> 发送最新帧给服务端视觉大模型
  -> TTS 主动播报
```

### 5.4 首帧就绪链路

```text
Application::StartCamera()
  -> start_ai_camera()
  -> frame_buffer_.WaitFirstFrame(1500)

Camera thread:
  -> cap >> bgr
  -> app->UpdateLatestFrame(bgr)
  -> VisionFrameBuffer::Push()
  -> ready_ = true
  -> latest_.valid = true
  -> frame_ready_cv_.notify_all()
```

## 6. 技术亮点

### 6.1 端云协同架构

端侧负责实时、确定性任务：

- 摄像头采集
- 音频采集
- OpenCV / YOLO 检测
- 最新帧缓存
- 主动事件触发

云端负责计算和生成任务：

- ASR
- LLM
- VLM
- TTS

面试说法：

> 我没有把大模型强行部署在 RV1106 上，而是把 RV1106 定位为边缘感知节点，负责实时采集、轻量检测和事件过滤，云端负责生成式语义理解。

### 6.2 latest-only 最新帧缓存

为什么不用队列：

- 队列会积压历史帧。
- 用户问的是“当前画面”，旧帧没有价值。
- 队列会增加延迟和内存占用。

设计：

```text
摄像头线程 Push 最新帧，覆盖旧帧
业务线程 GetLatest 读取快照
用 captured_ms 判断是否超过 500ms
```

### 6.3 首帧同步

问题：

- 摄像头线程异步启动。
- 主线程启动后立刻读取可能拿到空帧。

解决：

- `condition_variable::wait_for`
- `ready_ && latest_.valid`
- 有首帧立即唤醒，无首帧超时返回。

### 6.4 cv::Mat 跨线程安全

问题：

- `cv::Mat` 是引用计数对象。
- 摄像头线程可能复用底层 buffer。
- 业务线程如果直接引用，可能读到被修改的数据。

解决：

```cpp
latest_.image = frame.clone();
out.image = latest_.image.clone();
```

面试说法：

> 写入和读取都做 clone，牺牲一点拷贝开销，换取跨线程图像快照的确定性。

### 6.5 request_id 防串台

问题：

- WebSocket 异步返回。
- 旧视觉请求可能比新请求晚返回。
- 旧 TTS 音频可能继续播放。

解决：

- 每个视觉请求生成 `request_id`。
- 客户端维护 `active_vision_request_id_`。
- 返回的 `vision_result`、`tts`、`chat` 必须匹配当前 request_id。
- 不匹配则丢弃。

### 6.6 主动视觉触发和状态机融合

主动视觉不是直接打断系统，而是进入 Application 统一调度：

```text
检测到事件
  -> 检查状态是否 idle
  -> 检查 cooldown
  -> 抓取最新帧
  -> 发给大模型
```

关键修复点：

- 先检查 busy 状态，再更新 cooldown。
- 如果 speaking/thinking 时 suppress 事件，不消耗冷却时间。

## 7. 工程难点与解决方案

### 难点 1：旧帧问题

现象：

- 用户问“现在画面里有什么”，系统可能拿到几秒前的帧。

解决：

- VisionFrameBuffer 只保留最新帧。
- 每帧记录 `captured_ms`。
- 读取时判断 `NowMs() - captured_ms <= 500`。
- 超过 500ms 返回 false，不上传旧图。

### 难点 2：首帧为空

现象：

- 摄像头线程还没准备好，Application 已经开始读取。

解决：

- `WaitFirstFrame(timeout)`。
- 摄像头线程采到第一帧后 `notify_all()`。
- 超时不阻塞主流程，记录 warning。

### 难点 3：多线程数据安全

涉及线程：

- LVGL UI 线程
- AIChat 后台线程
- Application 状态机线程
- 摄像头线程
- WebSocket 线程
- PortAudio 回调线程
- Server 线程池 worker

解决：

- mutex 保护共享帧。
- condition_variable 通知首帧。
- atomic 控制线程停止标志。
- EventQueue 解耦线程间事件。
- request_id 管理异步请求。

### 难点 4：语音与视觉链路冲突

现象：

- 正在 speaking 时主动视觉又触发，导致串话。

解决：

- 主动视觉事件先检查状态机。
- 只有 idle 状态才允许提交。
- 使用 cooldown 去抖。
- busy 状态下不刷新 cooldown。

### 难点 5：C/C++ 混合工程集成

问题：

- DeskBot UI 是 C/LVGL。
- AIChat Application 是 C++ 类。

解决：

- 用 `extern "C"` 暴露 C ABI。
- 用 `void*` 不透明句柄保存 `Application*`。
- 用 C struct `IntentData` 把 Json::Value 转成 C 可用结构。

## 8. 相关知识点

### 8.1 用户线程与内核调度

项目中的 pthread/std::thread 是应用创建、运行在用户态的线程，但在 Linux 下通常是一对一映射到内核调度实体。

不要和纯用户级线程库混淆：

- 纯用户级线程：内核不知道线程，一个线程阻塞系统调用可能阻塞整个进程。
- Linux pthread/std::thread：内核知道每个线程，一个线程阻塞通常只阻塞该线程。

面试说法：

> 项目中的线程是用户态应用线程，但由 Linux 内核按线程调度，不是完全由用户态库调度的绿色线程。

### 8.2 为什么 AIChat 线程不阻塞摄像头和 WebSocket

核心原则：

```text
阻塞函数只阻塞调用它的线程。
```

`Application::Run()` 内部会 join 状态机线程，是阻塞式函数。

- 如果在 LVGL 线程直接调用，UI 会卡死。
- 项目中通过 `pthread_create` 创建 ai_chat_thread 调用，所以只阻塞 AIChat 后台线程。
- 摄像头线程和 WebSocket 线程是独立线程，不会被直接阻塞。

### 8.3 condition_variable 触发逻辑

等待方：

```cpp
frame_ready_cv_.wait_for(lock, timeout, [] {
    return ready_ && latest_.valid;
});
```

触发方：

```cpp
ready_ = true;
latest_.valid = true;
frame_ready_cv_.notify_all();
```

注意：

- `notify_all()` 只负责唤醒。
- predicate 负责判断条件是否真的成立。
- wait 时会自动释放 mutex，唤醒后重新加锁。

### 8.4 时间戳 500ms 判断逻辑

写帧时：

```cpp
latest_.captured_ms = NowMs();
```

读帧时：

```cpp
if (NowMs() - latest_.captured_ms > max_age_ms) {
    return false;
}
```

如果传入 `max_age_ms = 500`：

```text
当前时间 - 帧缓存时间 <= 500ms，可以用
当前时间 - 帧缓存时间 > 500ms，认为是旧帧
```

使用 `steady_clock`，因为它单调递增，不受系统时间校准影响。

## 9. 常见面试问题与回答

### Q1：这个项目整体架构是什么？

答：

> 项目是端云协同架构。RV1106 端负责音频采集、TTS 播放、摄像头采集、OpenCV/YOLO 检测、最新帧缓存和 WebSocket 通信；服务端负责 VAD、ASR、LLM、VLM 和 TTS。端侧做实时感知和事件过滤，云端做大模型语义理解。

### Q2：实际入口是哪里？

答：

> 独立 AIChat 调试入口是 `AIChat_demo/Client/main.cc`，但完整 DeskBot 产品实际入口是 `DeskBot_demo` 的 LVGL ChatBot 页面。页面进入后从 `system_para.conf` 加载 server 地址、端口、token、设备 ID 和音频参数，通过 `start_ai_chat()` 调用 AIChat C 接口创建 C++ Application。

### Q3：C 接口层为什么存在？

答：

> 因为 DeskBot UI 是 C/LVGL，AIChat 核心是 C++ Application。C 不能直接调用 C++ 类，所以用 `extern "C"` 暴露 C ABI，用 `void*` 不透明句柄保存 Application 对象，实现 C UI 和 C++ 业务层解耦。

### Q4：Application 类是不是 TCB？

答：

> 不是。TCB 是操作系统内核用于线程调度的数据结构，保存寄存器、栈指针、优先级等信息。Application 是应用层 Runtime Context / Orchestrator，负责管理音频、视觉、WebSocket、状态机和请求生命周期。

### Q5：怎么保证视觉问答拿到最新帧？

答：

> 摄像头线程持续 Push 最新帧到 VisionFrameBuffer，每帧记录 seq 和 captured_ms。视觉问答时调用 GetLatest(snapshot, 500)，只有当前时间和 captured_ms 的差值不超过 500ms 才返回，否则认为是旧帧并拒绝发送。

### Q6：为什么不用帧队列？

答：

> 视觉问答要的是当前画面，不需要历史帧。队列会造成延迟堆积，可能拿到几秒前的画面。所以我设计 latest-only buffer，每次新帧覆盖旧帧。

### Q7：首帧为空怎么解决？

答：

> Application 启动摄像头后调用 WaitFirstFrame(timeout)，摄像头线程采到第一帧后写入缓存，设置 ready 和 valid，然后 notify_all 唤醒等待线程。这样比 sleep 更可靠，也不会永久阻塞。

### Q8：cv::Mat 跨线程为什么要 clone？

答：

> cv::Mat 是引用计数对象，直接赋值可能共享底层 buffer。摄像头线程下一帧可能复用或覆盖数据，所以写入缓存和读取快照时都 clone，保证业务线程拿到独立图像。

### Q9：主动视觉怎么避免打断语音？

答：

> 主动视觉事件进入 Application 后先检查状态机，只有 idle 才允许触发。如果当前 speaking、thinking 或 listening，就 suppress。cooldown 只在真正提交事件后更新，避免 busy 状态消耗冷却时间。

### Q10：request_id 解决什么问题？

答：

> 解决异步请求串台。视觉请求发出后服务端可能乱序返回，所以每个请求带 request_id。客户端只接受当前 active request 的 vision_result、tts、chat 结束消息，不匹配的旧请求直接丢弃。

### Q11：项目线程属于用户线程吗？

答：

> 它们是应用创建、运行在用户态的 pthread/std::thread/Python threading 线程，但在 Linux 下通常由内核一对一调度，不是纯用户级绿色线程。因此一个线程阻塞通常只阻塞该线程，不会阻塞整个进程。

### Q12：为什么 AIChat 线程不会阻塞摄像头线程和 WebSocket 线程？

答：

> Application::Run 是阻塞函数，阻塞的是调用它的线程。项目中 DeskBot 通过 pthread_create 创建独立 ai_chat_thread 来运行它，所以不会阻塞 LVGL UI。摄像头线程和 WebSocket 线程也是独立线程，由内核分别调度，因此不会被 AIChat 线程的 join 直接阻塞。

### Q13：服务端线程池怎么实现？

答：

> 服务端 TaskManager 使用 Python `ThreadPoolExecutor(max_workers=5)`。AudioHandler 或 TextHandler 收到耗时任务后调用 `submit_task`，把 chat_start_task 或 vision_start_task 放到线程池执行，避免阻塞 WebSocket 事件循环。

### Q14：图像怎么传给大模型？

答：

> 端侧从 VisionFrameBuffer 获取最新帧后 resize 到 448x448，使用 OpenCV imencode 编码成 JPEG，再 Base64 编码，封装进 JSON 的 image 字段，通过 WebSocket 发送 describe_scene。服务端 VisionService 把它组装成 data:image/jpeg;base64 格式，调用 qwen-vl-plus。

### Q15：RV1106 能不能做 AI 边缘部署？

答：

> 可以，但适合轻量端侧感知，不适合直接跑大语言模型。RV1106 适合做摄像头采集、OpenCV 预处理、RKNN YOLO 检测、事件触发和音频采集播放。LLM/VLM/TTS 放云端更合理。

## 10. 简历项目描述

推荐版本：

```text
2025.12 - 至今    基于 RV1106 的智能多模态语音对话机器人    嵌入式端负责人

项目技术：C++、Linux、OpenCV、RKNN/YOLOv5/RetinaFace、WebSocket、PortAudio、Opus、JsonCpp、Python

项目描述：基于 RV1106 边缘设备构建端云协同多模态机器人系统，端侧负责语音采集、TTS 播放、摄像头采集、YOLOv5 目标检测、人脸检测、最新帧缓存和 WebSocket 实时通信，云端负责 ASR、大模型对话、视觉理解和 TTS 生成，实现实时语音对话、被动视觉问答和主动视觉事件提醒。

主要工作：
- 设计 C++ 客户端 Application 编排层，统一管理音频、WebSocket、状态机、摄像头线程、视觉帧缓存和多模态请求生命周期。
- 基于状态机实现 startup -> idle -> listening -> thinking -> speaking 对话流程管理，协调语音采集、ASR、LLM 推理与 TTS 播放。
- 设计线程安全的 VisionFrameBuffer 最新帧缓存，使用 mutex、condition_variable、cv::Mat clone 解决首帧为空、旧帧误读和跨线程访问问题。
- 在 RV1106 NPU 上部署 YOLOv5 目标检测和人脸检测双模型，使用零拷贝 rknn_create_mem + rknn_set_io_mem 方案，配合 int8 非对称量化，后处理中逐层 zp/scale 反量化解析检测结果。
- 研究混合精度量化：通过逐层敏感度分析识别精度关键层，检测头和第一层卷积保持 FP16，其余层 INT8，精度损失 <1%，推理速度提升 3 倍。
- 实现语音触发视觉问答链路，支持用户提问时获取 500ms 内最新画面，JPEG/Base64 编码后通过 WebSocket 发送至云端视觉大模型。
- 实现 OpenCV/YOLOv5 主动视觉触发 + 人脸检测事件，结合 cooldown 和状态机抑制策略避免重复触发与语音播报冲突。
- 优化 WebSocket 通信协议，JSON 信令与二进制 Opus 音频流混合传输，引入 request_id 机制防串台。
```

## 11. 一分钟面试自我讲解

> 我做的是一个基于 RV1106 的端侧 AI 多模态交互系统。端侧用 C++、Linux、OpenCV 和 RKNN，负责音频采集、TTS 播放、摄像头采集、YOLOv5 和 RetinaFace 人脸检测的双模型推理、主动视觉事件触发和 WebSocket 通信；服务端用 Python，接 ASR、大模型对话、视觉理解和 TTS。
>
> 我主要负责嵌入式客户端多模态链路和端侧 AI 部署。视觉链路上，我做了人脸检测模型从 ONNX 到 rknn 的部署，和后处理中 int8 反量化解码完整实现。同时我还研究了混合精度量化——通过敏感度分析让检测头保持 FP16、其余层 INT8，在精度损失不到 1% 的前提下把推理速度提升 3 倍。
>
> 除了模型部署，我还处理了多线程下的帧缓存一致性问题。VisionFrameBuffer 做了 latest-only 设计，摄像头线程 Push 带时间戳的最新帧，业务线程用 500ms 新鲜度判断，读写各做一次 cv::Mat clone 防止 VideoCapture 复用 buffer 导致画面撕裂。状态机方面，主动视觉事件只在 idle 触发，cooldown 只在提交成功后才更新，保证 speak/think 状态下的 suppress 不消耗冷却期。
>
> 整体思路就是把嵌入式 NPU 定位为边缘 AI 感知节点，做人脸识别和关键词唤醒这类实时推理，大模型真正兜底。

> 这是一个端侧 AI 多模态交互系统。我在 RV1106 NPU 上部署了 YOLOv5 目标检测和人脸检测双模型，实现了实时语音对话和主动/被动视觉问答。核心技术点包括：RKNN 零拷贝推理优化（rknn_create_mem 预分配 DMA 内存，cv::Mat 直接 wrap virt_addr，省去 CPU memcpy）、int8 量化模型的反量化后处理（逐层 zp/scale 解码）、以及混合精度量化（敏感度分析 + 检测头 FP16 + 中间层 INT8）。工程上还做了线程安全的 latest-only 帧缓存、状态机驱动的 cooldown 抑制、request_id 防串台。项目核心思路是把 AI 前移到端侧 NPU 做实时感知，云端做大模型理解兜底。

## 12. 后续可优化点

- 将 `yolov5_model_path` 纳入 DeskBot 的 `system_para.conf`，通过 C 接口传入 Application。
- 用不透明结构体句柄替代裸 `void*`，提升 C 接口类型安全。
- `stop_ai_chat()` 成功路径补充 `return 0`，并考虑是否需要 pthread_join。
- 将 `volatile int is_running` 改为 mutex 或 C11 atomic，提升线程状态安全。
- 对二进制 TTS 音频协议增加 request_id，彻底解决旧音频帧串台。
- 主动视觉事件可抽象成 `VisionEvent`，支持更多类别和不同 cooldown 策略。
- API key 不应硬编码，应改为配置文件或安全存储。
