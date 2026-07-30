# 多线程语音交互状态机 — 知识卡片

## 1. 一句话概括

7 状态有限状态机（FSM），`state_trans_thread` 以 100ms 轮询事件队列，Idle/LISTENING/SPEAKING/THINKING 各自 spawn 独立线程执行，跨线程通过 `eventQueue_` 传递事件触发状态迁移。

---

## 2. 为什么需要它

语音交互不是线性的——用户说话时不能同时检测唤醒词，播放 TTS 时不能同时录音做 ASR。状态机确保任何时候只有一个活跃状态，状态切换清晰可控。

---

## 3. 七状态图

```
         [启动]
           │
           ▼
      ┌─ Startup ──────┐
      │  WebSocket 连接  │
      └────────┬────────┘
               │ connected
               ▼
      ┌─ Idle ───────────────────────┐
      │ KWS 唤醒词检测 (NPU ~50ms)     │
      │ 录音开                         │
      │ 接受主动视觉事件 (person/face)  │
      └─┬────┬──────────┬─────────────┘
        │    │          │
   wake │    │ vision_  │ fault_
   detec│    │ detect   │ happen
        │    │          │
        ▼    ▼          ▼
   Listening  Thinking  Fault → Startup(重连)

Listening:                              Speaking:         Thinking:
  录音 → Opus → 云端 ASR                 收到 Opus → decode   截帧 → HW JPEG → base64
  ASR 完成 → event(dialogue_end)          → 播放               → WebSocket → 云端 VLM
    → 自动转到 Speaking?                   播放完 → Idle       → event(dialogue_end)
                                                                 → Speaking(TTS)
```

---

## 4. 核心数据结构

```cpp
// StateMachine.h
class StateMachine {
    std::unordered_map<int, std::unique_ptr<State>> states_;
    std::map<std::pair<int, int>, int> transitions_;     // (from_state, event) → to_state
    int current_state_;
};

// StateConfig.cc — 全部转移规则在此
state_machine.RegisterTransition(
    AppState::idle,           // from
    AppEvent::wake_detected,   // event
    AppState::listening       // to
);

// AppEvents.h
enum AppEvent {
    connected = 1,
    wake_detected,
    dialogue_end,
    vision_detected,        // person / face detected by NPU
    fault_happen,
    // ...
};
```

---

## 5. 各状态线程

| 状态 | 线程 | 干什 | 退出条件 |
|------|------|------|---------|
| Idle | 独立 spawn | KWS 检测唤醒词 | 命中 → wake_detected → join |
| Listening | 独立 spawn | 录音→Opus→云端 ASR | ASR 完成 → dialogue_end → join |
| Thinking | 无独立线程（Enter 同步执行） | 截帧→VLM→发送 | SubmitVisionQuestion 返回后切状态 |
| Speaking | 独立 spawn | 收 Opus→decode→播放 | TTS 完成 → dialogue_end → join |
| Fault | 无独立线程 | 等待重连 | 连上 → connected → Startup |
| Startup | 无独立线程 | 初始化 + 连接 Server | 连上 → connected → Idle |
| Stop | 无独立线程 | 清理 | 不自动切状态 |

---

## 6. 跨线程事件流

```
Camera 线程                IdleState 线程           ws_msg_thread
══════════                ════════════           ══════════
NPU 检测到人
  ↓
SubmitActiveVisionEvent()
  ↓ QueuePendingVisionQuestion(prompt, source)
  ↓ eventQueue_.Enqueue(vision_detected)  ──────────────→ state_trans_thread
                                                           ↓ 100ms 轮询
                                                         HandleEvent()
                                                           ↓ idle + vision_detected → Thinking
                                                         Thinking::Enter()
                                                           ↓ ConsumePendingVisionQuestion
                                                           ↓ SubmitVisionQuestion → DescribeCurrentScene
                                                           ↓ 异步

KWS 命中 wakeword                                        WebSocket 断开
  ↓                                                        ↓
eventQueue_.Enqueue(wake)                              on_close_ → Enqueue(fault)
  ─────────────→ idle + wake → Listening                 ──────────→ 当前状态 → Fault
```

---

## 7. 线程间通信方式

```
Camera → state_trans:  eventQueue_.Enqueue(vision_detected)   线程安全队列
IdleState → state_trans: eventQueue_.Enqueue(wake_detected)   同队列
ws_msg → state_trans:   eventQueue_.Enqueue(fault_happen)     同队列
Camera → thinking:      VisionFrameBuffer (mutex + cv)        间接——Enter 时消费 pending question
IdleState/Listening:    recordedAudioQueue (mutex + cv)        同 AudioProcess 下的队列
Speaking:                playbackQueue (mutex)                 同 AudioProcess
```

---

## 8. 涉及源码

| 文件 | 内容 |
|------|------|
| `StateMachine/StateMachine.h` | 泛型状态机模板 |
| `StateMachine/StateMachine.cc` | HandleEvent + RegisterTransition |
| `Application/StateConfig.cc` | 全部状态注册 + 转移规则 |
| `Application/Application.cc:402-418` | Run() → state_trans_thread 主循环 |
| `UserStates/Idle.cc` | KWS 线程 spawn |
| `UserStates/Listening.cc` | 录音→Opus→SendBinary |
| `UserStates/Thinking.cc` | ConsumePending → SubmitVisionQuestion |
| `UserStates/Speaking.cc` | TTS 播放等待 |
| `Application/WS_Handler.cc` | ws_msg 事件处理 |
| `Events/AppEvents.h` | 事件枚举 |
| `Events/EventQueue.h` | 线程安全事件队列 |

---

## 9. 常见 Bug

| Bug | 原因 | 修复 |
|-----|------|------|
| 状态机卡死 | HandleEvent 返回 false，未处理的 (state, event) 对 → 无转移 → 卡住 | 每个事件必须有对应转移 |
| 唤醒风暴 | KWS 回到 Idle 后立即误唤醒 | 加冷却窗口 2-3s |
| 视觉事件丢失 | idle 之外的 state 直接丢弃 | 延迟到 idle 再触发 |
| 析构崩溃 | StopCamera 后 camera 线程还在写 VisionFrameBuffer | 先 StopCamera → join → 再析构 |

---

## 10. 面试问题

1. **为什么不用 if-else 链？** 7 状态 × 6 事件 = 42 种组合，if-else 维护灾难。FSM 表驱动。
2. **各状态为什么要 spawn 线程？** 每个状态的 IO 模式不同（Idle 等音频 + KWS，Listening 等 ASR 结果 + 发送），独立线程可以各自阻塞各自的事。
3. **Thinking 为什么没有独立线程？** 截帧→VLM 发送是同步 HTTP 请求，发完就等→切状态，不需要独立线程。
4. **eventQueue_ 是线程安全的吗？** 是，内部有 mutex 保护。
5. **状态过渡期间的事件怎么处理？** 当前直接丢弃（`Suppress vision event while app is busy`），需改进为延迟触发。
6. **主动视觉 vs 被动视觉的触发链路差异？** 主动=camera 线程 → event(active_opencv) → Thinking::Enter → pending question。被动=ws_msg → look_at_environment → SubmitVisionQuestion(passive_voice) → 直接截帧。
