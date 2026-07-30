# WebSocket 通信 — 知识卡片

## 1. 一句话概括

基于 websocketpp 库，`ws_msg_thread` 独立线程接收云端消息，分为文本帧 (JSON 状态/意图/VLM 结果) 和二进制帧 (Opus 音频)，通过回调分发到状态机和音频链路。

---

## 2. 为什么需要 WebSocket

HTTP 短连接每次握手 200ms+，不适合持续语音流。WebSocket 长连接：一次握手永久保活，双向异步推送，文本+二进制双通道——JSON 控状态，二进制传音频。

---

## 3. 协议结构

```
客户端 → 服务端:

  文本帧 (JSON):
    {"type":"state", "state":"idle"}
    {"type":"asr", "text":"用户说了什么"}
    {"type":"describe_scene", "request_id":"vision-1", 
     "source":"active_opencv", "prompt":"...", 
     "image":"/9j/4AAQ...base64..."}           ← HW JPEG 编码后

  二进制帧 (Opus 音频):
    [packet_header] [opus_payload ~80bytes]

服务端 → 客户端:

  文本帧 (JSON):
    {"type":"function_call", "function_call":{"name":"look_at_environment"}}
    {"type":"vision_result", "request_id":"vision-1",
     "text":"前面有一个红色杯子..."}

  二进制帧 (Opus 音频 + TTS 语音):
    [packet_header] [opus_payload]
```

---

## 4. 核心数据结构

```cpp
// WebsocketClient.h
class WebSocketClient {
    client_t ws_client_;                     // websocketpp::client (ASIO)
    std::shared_ptr<std::thread> thread_;    // 后台收消息线程
    
    message_callback_t on_message_;          // 收到消息 → ws_msg_handle()
    close_callback_t   on_close_;            // 连接断开 → Enqueue(fault)
    
    std::atomic<bool> is_connected_;
};

// 初始化 + 注册回调
ws_client_.SetMessageCallback([this](const std::string& msg, bool is_binary) {
    app_handler.ws_msg_handle(msg, is_binary, this);   // WS_Handler.cc
});
ws_client_.SetCloseCallback([this]() {
    eventQueue_.Enqueue(AppEvent::fault_happen);       // 断线回调
});
```

## 5. 消息分发

```
ws_msg_handle(msg, is_binary, app)
  │
  ├─ is_binary=true:
  │     UnpackBinaryFrame → Opus decode → addFrameToPlaybackQueue
  │     (60% 的流量走这：ASR 处理完的音频回传 / TTS 语音)
  │     会触发 barge-in 检测：如果设备正在播放 → 清空播放队列 → 准备打断
  │
  └─ is_binary=false (JSON):
        │
        ├─ "state"
        │     → eventQueue_.Enqueue(wakeup_end / dialogue_end)
        │
        ├─ "function_call"
        │     → name == "look_at_environment"
        │        → SubmitVisionQuestion("passive_voice")    ← 被动视觉！
        │     → name == "intent_xxx"
        │        → IntentHandler::HandleIntent()
        │
        ├─ "vision_result"
        │     → UpdateVisionContext()    ← 缓存最新视觉描述
        │
        └─ "tts" / "asr"
              → 状态机 → event queue
```

---

## 6. 连接生命周期

```
Startup::Enter()
  → ws_client_.Connect(address, port)
     → websocketpp::get_connection(uri) + append_header("Bearer", token)
     → ws_client_.connect(con)
     → ws_client_.start_perpetual()
     → thread_ = new std::thread([this]{ ws_client_.run(); })
        //  ↑ 独立线程：内部 ASIO io_context.run() 死循环收消息

断线: 内部检测到 close frame / timeout
     → on_close_ 回调 → eventQueue_.Enqueue(fault_happen)
     → state_trans_thread → 当前状态 → Fault
     → Fault::Enter() → 等待 3s → Enqueue(connected) → Startup
     → Startup::Enter() → 重新 Connect(address, port)
```

---

## 7. 主动/被动视觉触发对比

| | 主动视觉 (NPU 触发) | 被动视觉 (云端触发) |
|---|---|---|
| 谁发起 | camera 线程 NPU 检测到人/脸 | 云端返回 `look_at_environment` |
| 入口 | `AIcamera_c_interface.cc:322` | `WS_Handler.cc:172` |
| source | `"active_opencv"` | `"passive_voice"` |
| prompt | 自动拼：”Visual event detected: person_detected, confidence=0.7. Please describe...” | 固定：“Please describe the current camera frame in Chinese.” |
| 冷却 | 5000/8000ms | 无冷却 |
| 路径 | SubmitActiveVisionEvent → event( vision_detected) → idle → Thinking → ConsumePending → SubmitVisionQuestion | 云端判决意图 → ws_msg → look_at_environment → SubmitVisionQuestion → 当场截帧发图 |

---

## 8. 涉及源码

| 文件 | 内容 |
|------|------|
| `WebSocket/WebsocketClient.h` | WebSocketClient 类, 成员 |
| `WebSocket/WebsocketClient.cc` | Run(), Connect(), 回调注册 |
| `Application/WS_Handler.cc` | ws_msg_handle() — 全消息分发 |
| `Application/WS_Handler.h` | WSHandler 全局对象 |
| `Application/Application.cc:70-72` | 注册 close + message 回调 |
| `Application/Application.cc:314-368` | DescribeCurrentScene — HW JPEG + SendText |
| `Application/Application.cc:236-248` | SubmitVisionQuestion — 截帧入口 + 发图 |

---

## 9. 常见 Bug

| Bug | 原因 | 修复 |
|-----|------|------|
| 连接超时 | 网络不通 / token 不对 | 检查 IP/端口/token |
| 断线后卡在 Fault | Fault → Startup 转移失败 | 加连接超时 fallback |
| 大图阻塞音频 | DescribeCurrentScene 同步发大 JSON (70KB)，排队阻塞 | 分优先级：音频>图片 |
| VLM 结果错配 | 两次 vision request 乱序回传 | UpdateVisionContext 比对 frame_seq |
| 二进制帧解包失败 | 协议头不匹配 | 打日志确认 header |

---

## 10. 面试问题

1. **WebSocket 为什么不用 HTTP？** 长连接免握手，双向推送，适合持续语音流。
2. **websocketpp 内部用什么线程模型？** ASIO io_context.run() 在独立线程死循环，收到消息回调我们注册的函数。
3. **主动和被动视觉的区别？** 主动=NPU 检测到人→事件→Idle→Thinking→发图；被动=云端判断用户想要看图→ws_msg→立即截帧发图。
4. **断开连接怎么恢复？** on_close → Event(fault) → Fault 等待 → connected → Startup 重连。
5. **为什么用二进制帧传音频？** JSON 只能文本，二进制帧可以直接塞 Opus 压缩数据，协议头轻量。
6. **DescribeCurrentScene 的发送顺序？** HW JPEG → base64 → JSON → ws SendText (同步阻塞至send 完成)。
7. **vision_result 是谁发过来的？** 云端 VLM 分析完图片后，服务端回传 JSON 结果。
