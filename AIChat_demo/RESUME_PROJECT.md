# 端侧AI多模态交互系统 — 简历项目描述

## 项目概述

基于 **Rockchip RV1106**（Cortex-A7 + 0.5T NPU，256MB DDR）的端云协同多模态对话机器人系统，实现"摄像头采集 → NPU 推理 → 云端 VLM → 语音交互"的完整闭环。核心亮点是 **端到端零 CPU 像素拷贝管线** 和 **多模型 NPU 内存复用**，在极致内存约束下完成 YOLO 物体检测 + 人脸检测 + 关键词唤醒三模型并发推理。

**技术栈**：C/C++、RKNN、RGA (Rockchip Graphics Acceleration)、V4L2、MPP、CMake、GDB

---

## 个人负责模块

### 1. 摄像头驱动 — V4L2 MIPI CSI 采集

- 基于 V4L2 框架开发 `/dev/video11` 摄像头采集驱动接口
- 实现 **DMA-BUF 零拷贝采集**：通过 `VIDIOC_EXPBUF` 导出 ISP 输出的 NV12 帧的 dma-buf fd，跳过 CPU 拷贝
- 支持 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` 多平面格式和 ISP 3A (AE/AWB) 配置
- 关键文件：`v4l2_capture.h/cc` — 封装 V4L2 初始化、帧获取/释放、fd 生命周期管理

```
ISP → NV12 (V4L2 MMAP + VIDIOC_EXPBUF → dma-buf fd)
```

### 2. SPI-LCD 屏驱动 — RGA 硬件加速显示

- 实现 **RGA 硬件 RGB→RGB565 转换 + resize 640→320**，替代 `cv::cvtColor` + `cv::resize`
- LCD buffer 使用 CMA 分配 (`dma_buf_alloc`)，实现 fd→fd 零 CPU 拷贝显示路径
- 支持 NV12→RGB→RGB565 完整格式链的单次 RGA 操作，消除每帧 4 次 CPU 内存操作
- 消除的 CPU 操作（每帧）：`cvtColor RGB2BGR` (1.2MB)、`resize 640→320` (1.2MB)、`cvtColor BGR2BGR565` (300KB)、`memcpy` (150KB)

```
NPU 输出 RGB CMA fd (640x640)
  ↓ RGA fd→fd (RGB→RGB565 + resize 640→320)
LCD CMA fd (320x240, RGB565) → SPI LCD
```

### 3. 音频驱动 — I2S + PortAudio 采集/播放

- 基于 **PortAudio** 框架实现 16kHz 单声道音频采集与播放
- 集成 **Opus** 低延迟语音编解码（40ms 帧长）
- 音频采集与推理/显示线程解耦，回调式数据处理
- 回声消除与音频缓冲区管理

### 4. NPU 多模型推理调度

- 统一调度 YOLOv5（物体检测，640x640 INT8，~30fps）、SCRFD（人脸检测，640x640 INT8）、KWS（关键词唤醒）三模型
- 可配置推理频率：YOLO 每帧 / Face 每 N 帧 / KWS 连续监听
- 每 5s FPS 报告，追踪各阶段延迟
- 关键文件：`AIcamera_c_interface.cc` — `_inference_loop_zero_copy` 统一调度循环

### 5. NPU 内存复用 (Arena)

- 三模型（YOLO+Face+KWS）**共享同一组 input/output DMA 缓冲区**，节省 1.4MB（3.4MB vs 独立 4.8MB）
- Arena 生命周期状态机：EMPTY → REGISTERED → ALLOCATED → DESTROYED
- bind-before-destroy 策略：先 `rknn_set_io_mem` 再 `rknn_destroy_mem`，确保引用安全
- `pthread_mutex_t` 保护 camera 线程和 KWS 线程的 bind+run 临界区
- 板端验证 4/4 PASS

### 6. ISP-V4L2-RGA-NPU 端到端零拷贝

- **完整管线 CPU 像素拷贝次数 = 0**：
  ```
  ISP → NV12 (V4L2 DMA-BUF fd)
    ↓ RGA (NV12→RGB + resize 640x640, fd→fd)
  CMA fd (RGB 640x640)
    ├→ NPU input (YOLO + Face, 零拷贝)
    ├→ VisionFrameBuffer (云端 VLM, 零拷贝)
    └→ LCD (RGA RGB→RGB565, 零拷贝)
  ```
- `rknn_create_mem_from_fd` 将 RGA 输出的 CMA fd 直接映射为 NPU input tensor
- 帧传递 RGB→BGR 改用 RGA fd→fd，消除 `cv::cvtColor` (每帧 1.2MB)
- 板端验证 `VIDIOC_EXPBUF` 通过、RGA NV12→RGB fd→fd 正常、帧率达标

**面试话术**：端到端零拷贝意味着推理延迟 = NPU 计算时间 + DMA 传输时间，CPU 完全不参与像素搬运。在 256MB 内存约束下，传统的 `cv::resize + cvtColor` 每帧涉及 ~10MB 的 CPU memcpy，零拷贝路线将此降为 0。

### 7. 云端 VLM 图片上传硬件化

- MPP MJPEG 硬件编码 + RGA 格式转换，实现 **零 CPU 拷贝 JPEG 编码**：
  ```
  BGR CMA fd (640x640)
    ↓ RGA fd→fd (BGR→NV12 + resize 640→448)
  NV12 CMA fd (448x448, ~302KB)
    ↓ MPP EXT_DMA import (零拷贝)
    ↓ mpp_init(MJPEG) + encode()
  JPEG bitstream → base64 → WebSocket → 云端 VLM
  ```
- `hw_jpeg_encoder.h/cc` — 独立 C API：`init` / `encode` / `deinit`
- 硬件路径优先，失败自动 fallback CPU `cv::imencode`，保证鲁棒性
- 消除的 CPU 操作（每 5s）：`cv::resize 640→448` + `cv::imencode .jpg`

### 8. 语音交互状态机

- 7 状态有限状态机：Fault → Startup → Idle ↔ Listening ↔ Thinking ↔ Speaking ↔ Stop
- KWS 关键词唤醒触发 `wake_detected` 事件：Idle → Listening
- NPU 视觉触发 `person_detected` / `face_detected` 主动视觉问答
- 状态间线程安全，WebSocket 断线自动重连

---

## 技术难点与解决方案

| 难点 | 方案 | 效果 |
|------|------|------|
| 256MB 内存无法加载三模型独立 DMA | NPU Arena 共享内存，bind-before-destroy 策略 | 节省 1.4MB (29%) |
| CPU resize/cvtColor 每帧 ~10MB | RGA 硬件 fd→fd 全格式链 | CPU 像素拷贝 → 0 |
| MPP MJPEG encode 类型不安全 (C++ enum) | 正确使用 MPP_OK/(MppPollType) 强制转换 | 编译通过 |
| KWS 与 YOLO 线程竞争 NPU | pthread_mutex 保护 bind+run 临界区 | 零死锁 |
| 交叉编译 MPP/RGA 链路 | CMake find_path 自动检测 + 完整路径 fallback | 一键编译 |

---

## 程序跑通后可能遇到的工程问题（基于实际代码分析）

### 一、NPU 内存复用 (Arena) 相关问题

#### 1.1 两个线程绑定同一个 Arena → 画面闪烁 / NPU 返回错误

**现象**：camera 线程（YOLO/Face）和 IdleState 线程（KWS）交替调用 `rknn_set_io_mem` 重新绑定 DMA 缓冲区。如果绑定发生在线程 A 的 `rknn_run` 期间，线程 B 发出的 `rknn_set_io_mem` 会覆盖正在使用的 IO 映射，导致当前帧的推理输出写到 KWS 的输出区域，产生错乱的检测结果。

**代码位置**：
```cpp
// AIcamera_c_interface.cc:313 — camera 线程
npu_arena_lock(arena);
npu_arena_bind_yolov5(arena, &rknn_app_ctx);  // 重新绑定 Arena → YOLO ctx
inference_yolov5_model(&rknn_app_ctx, &od_results);
npu_arena_unlock(arena);

// Idle.cc:69 — KWS 线程
if (g_kws_uses_arena && arena) npu_arena_lock(arena);
kws_feed_frame(&g_kws_ctx, data.data(), &kw_res);   // 内部 bind + run
if (g_kws_uses_arena && arena) npu_arena_unlock(arena);
```

**当前状态**：`pthread_mutex_t run_mutex` 已经在 bind+run 临界区加锁。但如果某处代码 `lock` 后遇到错误直接 `return` 而没有 `unlock`，会死锁整个 NPU 管线。

**面试话术**：对于多线程共享硬件加速器的场景，mutex 是最小侵入的保护方式。但更工程化的做法是引入"绑定租约"（bind lease）——一旦绑定成功，自旋锁持有者独享 NPU 直到 run 完成，其他线程排队等待。避免频繁 rebind 的开销（每次 bind 涉及内核 ioctl 调用，约 200-500μs）。

---

#### 1.2 内存大小不匹配导致的静默数据损坏

**现象**：NPU 驱动不会在校验 `rknn_set_io_mem` 的 buffer 大小是否匹配模型 tensor 的实际大小。如果 Arena 分配的 buffer 小于某个模型的 output tensor，`rknn_run` 会写越界，但不会报错——现象是输出 tensor 数据末尾被截断或邻居 buffer 被覆盖。

**代码位置**：
```cpp
// npu_memory_reuse.cc:195 — max 追踪
if (in_size > a->max_input_size) a->max_input_size = in_size;
// npu_memory_reuse.cc:189 — output 比较
if (out_sz > a->max_output_sizes[i]) a->max_output_sizes[i] = out_sz;
```

**当前状态**：`max_input_size` 和 `max_output_sizes[]` 跟踪所有已注册模型的最大尺寸。但如果模型注册顺序改变（比如 KWS 在 Arena allocate 之后才 register），`check_output_compat` 只 warn 不 abort。输出被截断时，后处理的 `detection_count` 字段可能读到一个合法但错误的值，导致画框偏移越界。

**防范**：`check_output_compat` 应该从 warn 改成 assert；如果 late-register 模型超出 arena 容量，直接拒绝注册而非降级运行。

---

#### 1.3 Arena 销毁时序与模型 release 的竞态

**现象**：`npu_arena_destroy` 先 NULL 掉所有 adopted ctx 的 mem 指针，再 `rknn_destroy_mem`。但如果 camera 线程在 destroy 过程中正好拿到 lock 并执行 `npu_arena_bind_yolov5`（它读取 `a->input_mem`），就会出现 use-after-free。

**代码位置**：
```cpp
// npu_memory_reuse.cc:443-453
for (int s = 0; s < a->num_slots; s++) {
    if (slot->adopted_input_mems)  slot->adopted_input_mems[0] = nullptr;
    // ...
}
// 此时 camera 线程如果拿到 lock，bind 会读到 nullptr → crash
```

**当前状态**：`npu_arena_destroy` 没有加 `run_mutex` 锁。应该在 destroy 开头先 lock，防止和外部的 bind+run 竞态。

---

### 二、网络相关（WebSocket）

#### 2.1 断线重连时状态机"卡住"

**现象**：用户正在说话（Speaking 状态），WebSocket 突然断开 → `fault_happen` 事件入队 → 状态切换到 Fault → 清空播放队列 → TTS 中断。如果重连成功回到 Idle，用户会有"话说一半被掐断"的体验。

**代码位置**：
```cpp
// Application.cc:70
ws_client_.SetCloseCallback([this]() {
    eventQueue_.Enqueue(static_cast<int>(AppEvent::fault_happen));
});
```

**改进方向**：Fault 处理时应先尝试静默重连（不通知用户），只在重连失败超过阈值（比如 3 次/30s）时才中断交互。Speaking 状态下尤其应该保持已有的播放队列，重连后恢复。

---

#### 2.2 WebSocket 消息乱序 → vision 结果错配

**现象**：云端 VLM 返回结果时，如果短时间内发送了两次 vision request（比如 person_detected 和 face_detected 几乎同时触发），服务器返回的消息可能因为网络原因乱序到达。如果 request_id 校验不严，可能把旧帧的结果当作新帧的视觉上下文缓存。

**代码位置**：
```cpp
// Application.cc:187-197 — IsActiveVisionRequest 只检查 request_id 非空且匹配
bool Application::IsActiveVisionRequest(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(vision_request_mutex_);
    return !request_id.empty() && request_id == active_vision_request_id_;
}
```

**当前状态**：`IsActiveVisionRequest` 配合 `ClearActiveVisionRequest` 能过滤大部分乱序。但 frame_seq 没有参与 request_id 去重逻辑——如果两帧的 request_id 被同时 accept，`UpdateVisionContext` 会无条件覆盖缓存。

**加固**：`UpdateVisionContext` 应比对新旧 frame_seq，只接受更新的结果。

---

#### 2.3 大图传输阻塞音频

**现象**：HW JPEG 编码输出 50-150KB 的 base64 图像，通过 WebSocket 发送。RV1106 的 WiFi 模块（USB/RTL8188）实际有效带宽约 1-2MB/s，传输 150KB 的 base64 字符串（~200KB）约需 100-200ms。在传输期间，WebSocket 的发送队列如果和音频 Opus 帧共享同一个连接，音频帧会被排在图片后面，导致语音响应延迟。

**当前状态**：`DescribeCurrentScene` 直接 `SendText` 发大 JSON（含 base64 图片）。音频帧通过同一个 WebSocket 连接发送。

**改进**：图片上传和音频发送应分优先级——音频帧优先，图片以分片方式插入空闲时隙。或者利用 WebSocket 的双通道（文本帧 vs 二进制帧）分离控制消息和数据。

---

### 三、V4L2 / DMA-BUF 相关问题

#### 3.1 V4L2 缓冲区耗尽 → 采集卡死

**现象**：推理线程处理慢（比如 YOLO+Face 连续推理 + motion detect + LCD 耗时 60ms），而 camera 驱动以 30fps（33ms/帧）输出。如果 `v4l2_capture_put_frame` 延迟归还 buffer，驱动在 3-4 帧后会因为没有空闲 buffer 而丢帧。如果连续丢帧 10 次以上，某些 V4L2 驱动会进入 ERROR 状态需要重新 `VIDIOC_STREAMOFF/ON`。

**当前状态**：
```cpp
// AIcamera_c_interface.cc:412
usleep(10000);  // 每帧固定 sleep 10ms，相当于硬性限流
```

**风险**：10ms sleep 只是经验值。当 NPU 推理变慢（模型切换/内存碎片），实际循环时间可能超过 33ms，缓冲区逐渐耗尽。没有监控 `v4l2_capture_get_frame` 的失败次数。

**加固**：当连续 3 次 `get_frame` 返回 -1 时，说明 V4L2 buffer 已耗尽，应主动 `STREAMOFF → STREAMON` 重置 pipeline。

---

#### 3.2 fd 泄漏 → 系统运行几小时后 /dev/video11 不可用

**现象**：每次 `v4l2_capture_get_frame` 获取的 isp_fd 如果在 RGA 失败路径上没有被正确释放，fd 会泄漏。Linux 进程 fd 上限默认 1024，按 30fps 计算，如果 RGA 每 10 帧失败 1 次，约 5 分钟耗尽。

**代码位置**：
```cpp
// AIcamera_c_interface.cc:296-306
buf_idx = v4l2_capture_get_frame(&g_v4l2_cap, &isp_fd);
int rga_ret = convert_image(&g_rga_src, &g_rga_dst, NULL, NULL, 0);
if (rga_ret != 0) {
    v4l2_capture_put_frame(&g_v4l2_cap, buf_idx);  // 正确归还了 buf
    continue;  // 但 isp_fd 是否被 V4L2 模块内部管理？
}
```

**当前状态**：RGA 失败时调用 `v4l2_capture_put_frame` 归还了 V4L2 buffer。但如果 `dma_sync_device_to_cpu` 之后的路径（比如 NPU 推理崩溃）不经过正常的 `put_frame`，fd 泄漏依然存在。`stop_ai_camera` 中的 `v4l2_capture_deinit` 会释放所有 buffer，但长期运行中累积泄漏不可逆。

---

### 四、RGA 硬件转换相关问题

#### 4.1 RGA 隐式失败 → CPU fallback 路径存在但被 rga_convert.c 移除了

**现象**：RGA `importbuffer_fd` 返回负值（handle ≤ 0），但我们的 `rga_convert.c` 直接 return -1。原始的 `image_utils.c` 版本有 CPU fallback（`convert_image_cpu`），但我们为了避免 turbojpeg/stb_image 依赖把它删了。

**代码位置**：`rga_convert.c:126` — `if (rga_handle_src <= 0) { return -1; }`

**后果**：如果某帧的源 fd 对应的 CMA buffer 被 NPU 占用（DMA 未完成），RGA import 可能失败。在原始版本中，`convert_image` 会 fallback 到 CPU 路径（vir_addr 直接 memcpy）。我们的版本直接返回错误，导致该帧的 YOLO 输入保持上一帧数据，产生虚假检测。

**最佳修复**：恢复 CPU fallback（当 src 和 dst 的 `virt_addr` 都非空时直接 memcpy），但不依赖 stb_image/turbojpeg。只需要 `image_utils.c` 中的 `crop_and_scale_image_c` 那部分。

---

#### 4.2 两条 RGA 管线共享硬件 → 调度冲突

**现象**：camera 线程中同时有两条 RGA 路径：
```
1. ISP NV12 → Arena RGB (用于 NPU 输入)
2. Arena RGB → LCD RGB565 (用于显示)
```
两次 `convert_image` 调用之间没有检查 RGA 硬件是否空闲。RGA 是单通道硬件，如果第一次 `improcess` 还没完成就发起第二次，第二次会被排队等待。RV1106 的 RGA 通过内核驱动调度，用户态不需要手动排队，但如果第一次调用触发了软件 fallback（`imStrError`），可能导致时序混乱。

---

### 五、MPP HW JPEG 编码相关问题

#### 5.1 首帧 JPEG SOI 丢失 → 云端 VLM 解码失败

**现象**：MPP MJPEG 编码器的第一帧有时不输出 JPEG SOI 头（0xFF 0xD8）。我们已经在代码中加了 `jpeg_valid` 检查（`hw_jpeg_encoder.cc:285-292`），但只 warn 不修复。

**当前代码**：
```cpp
if (!jpeg_valid) {
    HW_LOG("WARNING: JPEG SOI marker missing — MPP may need jpeg:header_mode config");
    // Don't fail; some MPP versions strip the header on first frame
}
```

**加固**：设置 `jpeg:header_mode=1` 的 encoder config（在 `mpp_enc_cfg_set_s32` 中添加）。或者如果 SOI 缺失，CPU 手动插入 `\xFF\xD8` 前缀，避免传到云端后才报错。

---

#### 5.2 MPP encode 阻塞超时

**现象**：`hw_jpeg_encode` 中有互斥锁保护 `g_mutex`。如果 MPP 编码卡住（硬件繁忙、驱动 bug），锁持有期间 `DescribeCurrentScene` 会阻塞，影响状态机的主循环（state_trans_thread 的 100ms 轮询被拖慢）。

**代码位置**：`hw_jpeg_encoder.cc:181`
```cpp
int hw_jpeg_encode(int src_fd, uint8_t** jpeg_data, size_t* jpeg_size) {
    std::lock_guard<std::mutex> lock(g_mutex);  // 阻塞其他调用者
```

**改进**：加超时机制——如果 `encode_put_frame` + `poll` 超过 1000ms 没有返回，释放锁、返回错误，让 CPU fallback 接管。

---

### 六、音频相关

#### 6.1 PortAudio 缓冲欠载 → 播放断断续续

**现象**：TTS 音频帧通过 `addFrameToPlaybackQueue` 入队，PortAudio 回调从队列取出播放。如果网络波动导致音频帧到达不及时，队列会短暂为空，播放出现"咔嗒"声。openwrt/uclibc 下 audio 驱动稳定性可能不如 desktop Linux。

**当前状态**：`audio_processor_.clearPlaybackAudioQueue()` 在主循环中被调用，但没有缓冲区水位监控。

**改进**：当播放队列低于阈值时，插入静音帧做填补，避免硬件 underrun 导致的 pop 噪音。

---

#### 6.2 KWS 误唤醒风暴

**现象**：KWS 模型在某些环境噪声（风扇、键盘敲击）下会连续误检测。IdleState::Run 中检测到 wake → `wake_detected` 事件入队 → Idle Exit → Listening → ... → 回到 Idle → 立即再次误检测 → 循环。

**代码位置**：`Idle.cc:72-75`
```cpp
if (kw_res.detected) {
    app->eventQueue_.Enqueue(static_cast<int>(AppEvent::wake_detected));
    break;
}
```

**当前状态**：没有冷却机制。返回 Idle 后立即开始新一轮 KWS 监听，如果环境噪声持续，会触发误唤醒风暴。

**改进**：加入唤醒冷却窗口（返回 Idle 后 2-3s 内忽略 KWS 输出），减少连续误唤醒。

---

### 七、线程与状态机

#### 7.1 状态机过渡期事件丢失

**现象**：`state_trans_thread` 以 100ms 间隔轮询事件队列。如果在 Idle → Listening 过渡期间（KWS 重置、Opus 编码器初始化）产生了 `vision_detected` 事件，该事件可能在 Listening 状态下被处理，但 Listening 状态不处理视觉事件（被 `Suppress vision event while app is busy` 丢弃）。

**代码位置**：`Application.cc:252-255`
```cpp
if (client_state_.GetCurrentState() != static_cast<int>(AppState::idle)) {
    USER_LOG_INFO("Suppress vision event while app is busy.");
    return false;
}
```

**改进**：事件应带有过期时间而非无条件丢弃。某些高优先级事件（比如人脸检测到熟人）应延迟到 Idle 后再触发。

---

#### 7.2 析构顺序导致的 crash

**现象**：`Application::~Application()` 中先 join 线程，再 `StopCamera()`。如果 camera 线程正在 `app->UpdateLatestFrame`（写入 `frame_buffer_`），而主线程在析构 `frame_buffer_`，发生 data race。

**代码位置**：`Application.cc:75-81`
```cpp
Application::~Application() {
    threads_stop_flag_.store(true);
    if (ws_msg_thread_.joinable()) ws_msg_thread_.join();
    if (state_trans_thread_.joinable()) state_trans_thread_.join();
    StopCamera();  // 此时 camera 线程可能仍在访问 app 的成员
}
```

**改进**：应该先 `StopCamera()`（内部 pthread_join camera 线程），再销毁 WebSocket 和其他成员。

---

## 当前未完成项

1. **libcrypto SIGILL 崩溃**：新 AIChatClient 在 `main()` 前崩于 libcrypto，旧版正常，需定位非法指令
2. 1 小时长跑稳定性测试
3. NPU/RGA 双缓冲流水线化（预期吞吐提升 30-50%）
4. 上述工程问题的加固与修复
