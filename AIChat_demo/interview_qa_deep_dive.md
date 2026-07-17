# 多模态机器人项目面试问答深挖

这份文档不是项目简介，而是面向面试官追问的问答手册。重点覆盖：

- WebSocket 协议怎么设计
- 线程关系怎么讲清楚
- 异常场景怎么处理
- 嵌入式、多线程、视觉理解、状态机、端云协同会被追问什么

建议用法：

- 先通读一遍，建立“答题地图”
- 再挑高频问题重点背
- 面试时按“现象 -> 原因 -> 设计 -> 权衡”四步回答

---

## 一、项目总览类问题

### Q1：你这个项目到底是什么，不要泛泛地说“机器人”

**答：**

这是一个基于 RV1106 的端云协同多模态机器人系统。端侧用 C++ / Linux / OpenCV / WebSocket / RKNN-YOLO 负责音视频采集、本地视觉检测、最新帧缓存、状态机调度和 TTS 播放；服务端用 Python WebSocket 接入 VAD、ASR、LLM、VLM 和 TTS 服务，实现语音对话、视觉问答和主动视觉事件提醒。

更精炼一点说：

> 这是一个嵌入式端侧实时感知 + 云端大模型理解的多模态交互系统。

---

### Q2：这个项目最有价值的地方是什么？

**答：**

不是“接了一个大模型”，而是把语音、视觉、线程调度、最新帧一致性、主动/被动视觉链路和请求防串台做成了一个完整系统。项目价值主要在三个方面：

1. 端云协同设计合理，符合 RV1106 算力边界  
2. 多线程下解决了旧帧、空帧、状态冲突、请求串台问题  
3. 不是纯聊天，而是具备主动视觉触发能力

---

### Q3：你在项目中主要负责什么？

**答：**

我主要负责嵌入式客户端多模态链路的设计与优化，包括：

- Application 编排层
- 状态机
- 摄像头线程与最新帧缓存
- 被动视觉问答
- 主动视觉触发
- C / C++ 接口桥接
- request_id 防串台
- 线程安全和生命周期管理

如果面试官更关注服务端，也可以补一句：

> 我也参与了服务端视觉链路和 TTS / WebSocket 协议的联调与增强。

---

## 二、协议设计类问题

### Q4：你们客户端和服务端之间用的是什么协议？

**答：**

底层是 WebSocket 长连接，应用层是“JSON 信令 + 二进制音频流”的混合协议。

- JSON 用来传控制消息、状态消息、视觉请求、ASR 结果、TTS 状态、对话结束等
- Binary 用来传 Opus 编码后的音频帧

所以它不是简单的“都用 JSON”，也不是简单的“都传二进制”，而是一个混合协议。

---

### Q5：JSON 信令怎么定义的？

**答：**

目前主要有这些消息类型：

- `hello`：客户端启动后上报 API key 和音频参数
- `functions_register`：注册 function_call 工具列表
- `state`：客户端状态变化，例如 idle / listening / thinking / speaking
- `describe_scene`：客户端发起视觉问答，请求服务端调用视觉模型
- `asr`：服务端返回语音识别结果
- `tts`：服务端返回 TTS 状态，比如 start / end / interrupted
- `chat`：服务端返回对话结束等控制信息
- `vision_result`：服务端返回视觉理解文本
- `error`：服务端错误

我会强调一点：

> JSON 信令不是随便拼字符串，而是把状态流转、视觉请求、异步结果回传都协议化了。

---

### Q6：二进制音频协议怎么定义？

**答：**

客户端和服务端之间传音频时，先用 Opus 编码，然后再包装成一个自定义二进制协议头。

当前头部结构大概是：

```cpp
struct BinProtocol {
    uint16_t version;
    uint16_t type;
    uint32_t payload_size;
    uint8_t payload[];
} __attribute__((packed));
```

含义是：

- `version`：协议版本
- `type`：帧类型，当前音频用 `type=0`
- `payload_size`：Opus 负载长度
- `payload`：真正的 Opus 编码数据

客户端录音流程：

```text
PCM -> Opus encode -> PackBinFrame -> WebSocket Binary
```

服务端回音频流程：

```text
TTS PCM -> Opus encode -> pack_bin_frame -> WebSocket Binary
```

---

### Q7：图片怎么传给大模型？

**答：**

端侧先从最新帧缓存取出当前帧，用 OpenCV 压成 JPEG，再转成 Base64 字符串，通过 JSON 的 `image` 字段发给服务端。

客户端步骤：

```text
cv::Mat
  -> cv::imencode(".jpg")
  -> base64_encode
  -> JSON["image"] = base64 string
```

服务端收到后，再按视觉大模型的接口协议包装成：

```json
{
  "image": "data:image/jpeg;base64,...",
  "text": "请描述当前画面"
}
```

所以模型并不是“猜这是图片”，而是协议层通过 `image` 字段明确告诉它这是图像输入。

---

### Q8：request_id 怎么传？为什么要传？

**答：**

request_id 是视觉链路里非常关键的字段。

客户端发视觉请求时：

```json
{
  "type": "describe_scene",
  "request_id": "vision-17",
  "source": "passive_voice",
  "frame_seq": 883,
  "captured_ms": 12345678,
  "prompt": "...",
  "image": "..."
}
```

服务端返回时，`vision_result`、`tts`、`chat` 都尽量带同一个 `request_id`。

作用是：

- 区分旧请求和新请求
- 防止异步结果乱序返回时串台
- 防止旧视觉请求晚返回覆盖当前问题

客户端只接受：

```text
request_id == active_vision_request_id_
```

不匹配就丢弃。

---

### Q9：结果怎么回？

**答：**

视觉链路结果回传分两部分：

1. 先回 JSON 文本结果：
   - `vision_result`
   - `chat(dialogue=end)`
   - `tts(start/end/interrupted)`

2. 再回二进制音频：
   - TTS PCM 编码成 Opus
   - 通过 Binary frame 回到客户端播放

也就是说结果不是一个包解决，而是：

```text
文本控制信令 + 音频流
```

---

### Q10：为什么不用 HTTP，而要用 WebSocket？

**答：**

因为这是一个长连接、低延迟、双向流式交互场景。

如果只用 HTTP，会遇到：

- 频繁建连开销
- 音频流不自然
- 状态切换和服务端主动返回不方便
- 主动视觉事件和 TTS 流式回传不够顺

WebSocket 更适合：

- 长连接
- 小包频繁交互
- 双向实时通信
- JSON + Binary 混合协议

---

## 三、线程关系类问题

### Q11：你们项目里有哪些线程？

**答：**

客户端主要有：

- LVGL UI 主线程
- `ai_chat_thread`：DeskBot 中创建，用来运行 AIChat Application
- `state_trans_thread_`：Application 内部状态机线程
- `ai_camera_thread`：摄像头采集线程
- WebSocket 网络线程：WebSocket++ / asio 内部线程
- PortAudio 录音/播放回调执行线程

服务端主要有：

- asyncio 主线程
- `AudioSendThread`
- `ThreadPoolExecutor` worker 线程

---

### Q12：这些线程是谁创建的？

**答：**

客户端：

- LVGL 主线程：DeskBot 主程序创建
- `ai_chat_thread`：`start_ai_chat()` 通过 `pthread_create` 创建
- `state_trans_thread_`：`Application::Run()` 里用 `std::thread` 创建
- `ai_camera_thread`：`start_ai_camera()` 里用 `pthread_create` 创建
- WebSocket 线程：`WebSocketClient::Run()` 内部启动
- PortAudio 回调线程：PortAudio 内部管理

服务端：

- asyncio 主线程：`asyncio.run(main())`
- `AudioSendThread`：`main.py` 启动
- worker 线程：`TaskManager(ThreadPoolExecutor)` 创建

---

### Q13：哪个线程会阻塞？哪个线程不能阻塞？

**答：**

最不能阻塞的是：

- LVGL UI 主线程
- WebSocket 事件循环线程

因为：

- UI 线程阻塞，界面卡死
- 事件循环阻塞，网络收发延迟或停住

可以长期运行、阻塞等待的：

- `ai_chat_thread`
- `state_trans_thread_`
- `ai_camera_thread`
- worker 线程

但它们阻塞的是自己，不会直接阻塞别的线程。

---

### Q14：为什么 AIChat 线程不会阻塞摄像头线程和 WebSocket 线程，但会阻塞 LVGL？

**答：**

`Application::Run()` 是阻塞式函数，阻塞的是调用它的线程。

如果在 LVGL 主线程直接调用：

```text
LVGL 线程被卡住，UI 不刷新
```

项目里是：

```c
pthread_create(&ai_chat_thread, ..., ai_chat_thread_func, ...)
```

所以实际阻塞的是后台 `ai_chat_thread`，不是 UI 线程。

摄像头线程和 WebSocket 线程是独立线程，由内核分别调度，因此不会被 `Application::Run()` 的阻塞直接拖死。

---

### Q15：线程之间共享什么数据？

**答：**

共享最多的是这些：

- `VisionFrameBuffer.latest_`：摄像头线程写，视觉问答线程读
- `eventQueue_`：WS 回调线程投递事件，状态机线程消费事件
- `IntentQueue_`：C++ 侧写入 function_call，C UI 侧读取执行
- 音频播放队列：WS_Handler / TTS 音频线程写，播放线程读
- 视觉请求上下文：request_id / 最近视觉结果

---

### Q16：怎么同步这些共享数据？

**答：**

主要用四类机制：

1. `mutex`  
   保护最新帧、视觉上下文、TTS owner 等共享状态。

2. `condition_variable`  
   用于首帧等待。

3. `atomic`  
   用于线程停止标志和接收开关，比如 `accept_incoming_audio_`、`threads_stop_flag_`。

4. 线程安全队列  
   用于 `eventQueue_`、`IntentQueue_`、服务端各种 queue。

---

### Q17：线程怎么停？

**答：**

不是用强杀，而是协作式停止。

例如：

- Application 停止：投递 `to_stop` 事件，状态机进入 stopping
- 摄像头线程：`ai_camera_stop.store(true)`
- 服务端音频线程：`stop_event.set()`
- WebSocket：显式 `Close()`

这比直接杀线程安全，因为资源可以有序释放。

---

## 四、异常场景类问题

### Q18：摄像头打不开怎么办？

**答：**

摄像头线程打开失败时：

- 记录错误日志
- 回滚 `ai_camera_running`
- 释放图像缓冲
- 清空 consumer 指针

避免出现：

```text
状态显示“摄像头已启动”
但实际没有线程在推帧
```

这是我专门修过的点。

---

### Q19：首帧超时怎么办？

**答：**

Application 启动摄像头后不会盲目继续，而是调用：

```cpp
WaitFirstFrame(1500)
```

如果 1500ms 内没有帧：

- 返回 false
- 打 warning
- 系统继续运行，但视觉问答会因为没有 fresh frame 而拒绝请求

这样不会永久阻塞，也不会错误地用空帧。

---

### Q20：大模型超时怎么办？

**答：**

服务端 `VisionService` / API 请求都有超时配置。

如果视觉服务异常或超时：

- 记录 error log
- 返回 `error` JSON
- 客户端收到后如果不是 stale request，就进入 fault 或提示失败

后续更完善的做法可以加：

- 重试
- 降级回复
- 模型切换

---

### Q21：旧请求晚返回怎么办？

**答：**

靠 `request_id` 过滤。

客户端维护当前 active vision request，返回结果如果：

```text
root["request_id"] != active_vision_request_id_
```

就认定是 stale request，直接丢弃。

这能避免：

- 旧视觉结果覆盖新问题
- 旧 TTS 控制消息扰乱当前状态

---

### Q22：正在 speaking 时又触发视觉怎么办？

**答：**

主动视觉事件必须先过状态机：

```cpp
if (client_state_.GetCurrentState() != idle) {
    suppress
}
```

也就是说：

- speaking / thinking / listening 时不允许主动视觉事件直接提交
- 只有 idle 才允许触发

这样可以避免正在播报时又插入新的视觉播报。

---

### Q23：如果用户说话打断 TTS，怎么处理？

**答：**

现在已经接入了 barge-in 基础能力：

- 服务端对输入音频做轻量 RMS 门限判断
- 如果检测到较强语音能量且当前有 active TTS request
- 就触发 `interrupt_active_tts("barge_in_voice")`

服务端会：

- 清空待发送音频队列
- 关闭当前 TTS 流
- 回 `tts: interrupted`

客户端收到后：

- 调用 `InterruptTTS()`
- 清空播放队列
- 关闭接收旧音频

不过严格意义上的“speaking 状态持续本地录音监听打断”，还可以继续优化，这也是后续增强点。

---

### Q24：如果视觉上下文缓存造成旧语义污染怎么办？

**答：**

要强调两个边界：

1. 当前图像仍然必须是 500ms 内的新鲜帧  
2. 视觉上下文只是辅助，不替代当前图像

当前风险是“旧语义污染”，不是“旧帧直接复用”。

缓解方式：

- 上下文只在 follow-up 问句触发
- 有 TTL
- 仅对 passive voice 类型使用
- prompt 中明确声明“current image is the source of truth”

---

### Q25：如果 WebSocket 断开怎么办？

**答：**

客户端注册了 close callback：

```cpp
ws_client_.SetCloseCallback(...)
```

连接断开时会投递 fault 事件，进入 fault / stop 流程。

服务端断开时会：

- cancel send task
- reset services
- 清理 ASR / VAD / chat 上下文

---

## 五、视觉与大模型类问题

### Q26：你这个视觉理解是不是 MCP？

**答：**

不是。当前是：

```text
自定义 WebSocket 业务协议
  -> 服务端 Python
  -> requests.post 调用视觉大模型 HTTP API
```

所以本质是：

```text
业务协议 + 第三方多模态 HTTP API
```

不是 MCP。

---

### Q27：你现在接的模型支持视觉理解吗？

**答：**

支持。服务端配置的是视觉模型，比如 `qwen-vl-plus`。

判断依据：

- 模型名本身就是 Vision-Language
- 请求体内容是：
  - `{"image": "data:image/jpeg;base64,..."}`
  - `{"text": "..."}`

这说明是图文联合输入。

---

### Q28：大模型怎么知道你传的是图片？

**答：**

不是模型“猜出来”的，而是接口协议明确告诉它这是图片。

服务端发给视觉模型时内容长这样：

```json
[
  {"image": "data:image/jpeg;base64,..."},
  {"text": "请描述当前画面"}
]
```

`image` 字段就是图像输入，`text` 字段就是文本提示词。

---

### Q29：你为什么把图像转成 JPEG + Base64？

**答：**

因为：

- `cv::Mat` 不能直接通过 JSON 发
- WebSocket 文本消息不能直接安全携带原始二进制图像
- 视觉模型 API 通常支持 URL / Base64 / 文件上传

所以采用：

```text
cv::Mat -> JPEG -> Base64 -> JSON image field
```

这样兼容性最好，也便于走统一 JSON 协议。

---

### Q30：为什么不是端侧直接调视觉模型？

**答：**

因为 RV1106 更适合做：

- 采集
- 预处理
- 轻量检测
- 事件触发

不适合直接跑大视觉语言模型。

把模型调用放服务端有三个好处：

- 算力和内存压力小
- 更容易切换模型供应商
- 协议、日志、重试、超时都更容易统一治理

---

## 六、最新帧缓存类问题

### Q31：LatestFrameBuffer 是怎么实现的？

**答：**

项目里叫 `VisionFrameBuffer`，本质是一个 latest-only frame buffer。

核心数据：

```cpp
FrameSnapshot {
    cv::Mat image;
    uint64_t seq;
    int64_t captured_ms;
    bool valid;
}
```

机制：

- 摄像头线程 `Push(frame.clone())`
- 业务线程 `GetLatest(out, max_age_ms)`
- 用 mutex 保护共享状态
- 用 condition_variable 通知首帧到达
- 只保留最新帧，不保留历史队列

---

### Q32：为什么不用队列？

**答：**

因为视觉问答要的是“当前画面”，不是历史画面。

如果用队列：

- 摄像头高帧率持续入队
- 业务处理慢会导致积压
- 最后读到的是历史帧

所以用 latest-only buffer 更适合实时视觉问答。

---

### Q33：500ms 新鲜度判断是怎么做的？

**答：**

写入帧时：

```cpp
latest_.captured_ms = NowMs();
```

读取帧时：

```cpp
if (NowMs() - latest_.captured_ms > 500) return false;
```

也就是：

```text
当前时间 - 帧写入时间 <= 500ms 才允许使用
```

这样可防止把历史帧发给大模型。

---

### Q34：为什么用 steady_clock？

**答：**

因为 steady_clock 单调递增，适合做时间间隔判断，不受系统时间被 NTP 校准或用户手动改时间影响。

---

## 七、C/C++ 混合工程类问题

### Q35：为什么 DeskBot 能调用 C++ Application？

**答：**

因为中间有一层 C 接口桥接：

- `AIchat_c_interface.h`
- `AIchat_c_interface.cc`

通过 `extern "C"` 暴露 C ABI，再把 `Application*` 包装成 `void*` 句柄给 C 侧使用。

---

### Q36：为什么 C 接口用 void*？

**答：**

因为 C 代码不认识 C++ class。

所以做法是：

```text
create_aichat_app() 在 C++ 里 new Application
返回 void* 给 C
C 只保存 app_instance 句柄
run/stop/destroy 时再传回 C++ 转回 Application*
```

这是典型的不透明句柄模式。

---

### Q37：为什么 get_aichat_app_intent 不直接返回 Json::Value？

**答：**

因为 C UI 不认识 `Json::Value`、`std::string`、`std::optional` 这些 C++ 类型。

所以接口层把 function_call 转成纯 C 结构体 `IntentData`：

- `function_name`
- `argument_keys`
- `argument_values`
- `argument_count`

这样 UI 层就能直接消费。

---

## 八、状态机类问题

### Q38：为什么需要状态机？

**答：**

因为这个系统不是单线程脚本，而是多模态交互系统。没有状态机会很容易出现：

- 正在播报时又开始录音
- 正在思考时又触发主动视觉
- 对话结束和 speaking_end 冲突

状态机让每个时刻只有一个主状态，降低系统混乱概率。

---

### Q39：状态机状态有哪些？

**答：**

主要有：

- `startup`
- `idle`
- `listening`
- `thinking`
- `speaking`
- `fault`
- `stopping`

必要时还可以扩展：

- `vision_pending`
- `barge_in_handling`
- `reconnecting`

---

### Q40：状态机和线程是什么关系？

**答：**

状态机本身不是线程，但项目里有专门的状态机线程：

```cpp
state_trans_thread_
```

它循环消费 `eventQueue_`，按事件触发状态迁移。

所以线程解决的是并发执行问题，状态机解决的是业务流程一致性问题。

---

## 九、你可以主动提出来的反问型亮点

### Q41：如果让我继续优化这个项目，我会做什么？

**答：**

我会优先做这几件事：

1. 二进制音频协议增加 request_id / stream_id  
2. C 接口改成不透明结构体句柄，而不是裸 `void*`  
3. 把模型路径也纳入 DeskBot 配置系统  
4. 把主动视觉抽象成统一 `VisionEvent` 中心  
5. 完整实现 barge-in，包括 speaking 状态下持续监听打断  
6. 增加任务超时、重试和降级策略  
7. 增加核心单元测试和协议文档

这个回答会让面试官觉得你对项目边界和后续演进有判断。

---

## 十、最后的答题框架

很多问题都可以套这四步：

### 1. 先说现象

例如：

```text
多模态项目里容易出现旧帧、串台、状态冲突
```

### 2. 再说原因

例如：

```text
因为摄像头线程、语音线程、网络线程和状态机线程是异步运行的
```

### 3. 再说设计

例如：

```text
我用 VisionFrameBuffer + request_id + 状态机来分别解决
```

### 4. 最后说权衡

例如：

```text
当前上下文缓存提升了 follow-up 理解，但也有旧语义污染风险，所以我加了 TTL 和触发条件限制
```

这样你的回答会显得非常像工程师，而不是背稿。

---

## 十一、建议重点熟背的问题

如果时间有限，优先把下面这些背熟：

1. 实际入口在哪里，参数怎么传  
2. C 接口层为什么存在，怎么桥接 C 和 C++  
3. WebSocket 协议：JSON 怎么定义，Binary 音频怎么定义  
4. 图片怎么传给大模型，模型怎么知道是图片  
5. 线程有哪些，谁创建，谁阻塞，怎么停  
6. 为什么 AIChat 线程不阻塞摄像头线程和 WebSocket 线程  
7. VisionFrameBuffer 为什么不用队列，500ms 如何判断  
8. request_id 为什么重要，怎么防串台  
9. 主动视觉触发如何避免打断语音  
10. RV1106 为什么适合做边缘感知而不是直接跑大模型
