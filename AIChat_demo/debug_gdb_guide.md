# GDB 调试指南 — 端侧多模态 AI 交互系统

## 一、构建工作流

**原则：所有模块在 x86 PC 上交叉编译，编译产物直接部署到开发板，不在开发板上做任何编译。**

```
┌────────────────────┐                    ┌──────────────────┐
│  x86 PC (编译机)    │                    │  RV1106 开发板     │
│                    │  scp / adb push    │                  │
│  cmake + make      │ ─────────────────> │  ./bin/main      │
│  arm-gcc 交叉编译   │                    │  gdbserver :1234 │
│  arm-gdb 调试服务端  │ <───────────────── │                  │
└────────────────────┘    gdbserver 连接    └──────────────────┘
```

### 1.1 一键交叉编译所有模块

项目包含三个子模块，全部在 PC 上交叉编译为 ARM 二进制文件：

```bash
# ============================
# 步骤 1：yolov5_demo (YOLOv5 + 人脸检测视觉库)
# ============================
cd yolov5_demo/cpp
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=../toolchain.cmake
make -j$(nproc)

# 产物：libyoloCameraCore.a、rknn_yolov5_demo、face_detect_test

# ============================
# 步骤 2：AIChat_demo (音频/WebSocket/状态机/Application)
# ============================
cd AIChat_demo/Client
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTARGET_ARM=ON
make -j$(nproc)

# 产物：libAIChatCore.a、AIChatClient

# ============================
# 步骤 3：DeskBot (LVGL UI + 集成全部模块)
# ============================
cd DeskBot_demo
mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DTARGET_ARM=ON
make -j$(nproc)

# 产物：bin/main (最终可执行文件)
#       bin/model/*.rknn (自动从 yolov5_demo/model/ 复制)
#       bin/lib/*.so (自动从 yolov5_demo/cpp/3rdparty/ 复制)
```

### 1.2 交叉编译工具链

三个模块共用同一套 Rockchip SDK 工具链：

```
toolchain.cmake 配置:
  CMAKE_C_COMPILER   = arm-rockchip830-linux-uclibcgnueabihf-gcc
  CMAKE_CXX_COMPILER = arm-rockchip830-linux-uclibcgnueabihf-g++
  CMAKE_SYSROOT      = rv1106-sdk/.../sysroot
```

### 1.3 Debug 编译选项

```bash
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-g3 -O0 -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-g3 -O0 -fno-omit-frame-pointer"
```

| 选项 | 说明 |
|------|------|
| `-g3` | 最详细调试信息，包含宏定义 |
| `-O0` | 关闭优化，变量不会被优化掉 |
| `-fno-omit-frame-pointer` | 保留帧指针，保证 backtrace 完整 |

Debug 模式自动加 `-g`。Release 模式会优化内联导致断点不准，**调试务必用 Debug**。

### 1.4 部署到开发板

```bash
# 方式一：scp 整个 bin 目录
scp -r DeskBot_demo/bin/* root@192.168.1.xxx:/app/

# 方式二：adb push
adb push DeskBot_demo/bin/* /app/

# 方式三：NFS 挂载（开发阶段最方便，改完 PC 上编译，板端直接运行）
# 板端：mount -t nfs 192.168.1.yyy:/home/xxx/DeskBot_demo/bin /app -o nolock
```

---

## 二、调试方式：gdbserver 远程调试

RV1106 内存有限，不能跑完整 GDB。PC 端用交叉工具链自带的 `arm-gdb`，板端跑 `gdbserver`。

### 2.1 启动 gdbserver（开发板端）

```bash
# 启动程序并等待 GDB 连接
gdbserver :1234 /app/main

# 或者 attach 到已运行的进程
gdbserver --attach :1234 $(pidof main)
```

### 2.2 连接 GDB（PC 端）

```bash
arm-rockchip830-linux-uclibcgnueabihf-gdb /path/to/DeskBot_demo/bin/main

(gdb) target remote 192.168.1.xxx:1234
(gdb) continue
```

### 2.3 自动连接脚本 `gdb_connect.gdb`

```
set sysroot /home/xxx/rv1106-sdk/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/arm-buildroot-linux-uclibcgnueabihf/sysroot
set solib-search-path /app/lib
target remote 192.168.1.xxx:1234
```

使用：
```bash
arm-rockchip830-linux-uclibcgnueabihf-gdb -x gdb_connect.gdb /app/main
```

---

## 三、状态机调试

状态机是项目最容易出 bug 的地方（竞态、卡住、跳转异常）。

### 3.1 查看当前状态

```
(gdb) p app->client_state_.current_state_
(gdb) p app->eventQueue_

# 断点：每次状态转换
(gdb) b StateMachine::TransitionTo
(gdb) commands
> printf "Transition: %d\n", new_state
> bt 5
> continue
> end
```

| 状态值 | 含义 |
|--------|------|
| 0 | fault |
| 1 | startup |
| 2 | stopping |
| 3 | idle |
| 4 | listening |
| 5 | thinking |
| 6 | speaking |

### 3.2 状态机卡住排查

最常见的是 event 未被消费：

```
(gdb) bt
(gdb) info threads
(gdb) thread apply all bt
```

重点关注：`ListeningState::Run` 阻塞在 `getRecordedAudio`、idle 循环是否有条件死等。

### 3.3 关键断点位置

| 位置 | 文件 | 用途 |
|------|------|------|
| `StateMachine::TransitionTo` | StateMachine.cc | 所有状态转移 |
| `ListeningState::Enter` | Listening.cc:9 | 进入录音态 |
| `ListeningState::Run` | Listening.cc:25 | 录音循环 |
| `SpeakingState::Exit` | Speaking.cc | 播放结束 |
| `WSHandler::ws_msg_handle` | WS_Handler.cc:17 | WebSocket 消息入口 |

---

## 四、多线程调试

项目线程：状态机主线程、录音线程、播放线程、WebSocket 线程、Camera 线程。

### 4.1 线程列表

```
(gdb) info threads
  Id   Target Id         Frame
  1    Thread ...         main (main.c:xxx)
  2    Thread ...         WebSocketClient::Run (WebsocketClient.cc:34)
  3    Thread ...         ListeningState::Run (Listening.cc:25)
  4    Thread ...         AudioProcess::startPlaying (AudioProcess.cc)
  5    Thread ...         ai_camera_thread (AIcamera_c_interface.cc)
```

### 4.2 死锁排查

```
(gdb) thread apply all bt
```

看每个线程卡在哪个 `mutex` / `condition_variable::wait` 上。重点关注：
- `VisionFrameBuffer::Push` / `GetLatest` — mutex_ 竞争
- `AudioProcess::getRecordedAudio` — recordedAudioCV 永远不被 notify
- `IntentQueue_` / `eventQueue_` — 生产者-消费者 mismatch

### 4.3 只跟踪单个线程

```
(gdb) set scheduler-locking on
(gdb) thread 3
(gdb) step       # 只单步 thread 3，其他线程挂起
```

---

## 五、WebSocket 通信调试

### 5.1 消息发送/接收

```
# 发出去的文本消息
(gdb) b WebSocketClient::SendText
(gdb) p message

# 发出去的二进制帧（音频）
(gdb) b WebSocketClient::SendBinary
(gdb) p size

# 收到消息的分发
(gdb) b WSHandler::ws_msg_handle
(gdb) p message
(gdb) p is_binary
```

### 5.2 查看 JSON 内容

```
(gdb) p root.toStyledString()
```

### 5.3 网络断连排查

```
(gdb) b WebSocketClient::on_close
(gdb) bt
```

---

## 六、NPU / 推理调试

### 6.1 RKNN 初始化失败

```
(gdb) b rknn_init
(gdb) run
(gdb) finish
(gdb) p ret
# ret != 0 则初始化失败
```

常见原因：模型路径不对、`librknnmrt.so` 未加载、NPU 已被占用。

### 6.2 推理结果异常

在 [postprocess.cc](D:\echo\yolov5_demo\cpp\postprocess.cc) 中：

```
# INT8 反量化参数
(gdb) b process_i8_rv1106
(gdb) p output_attrs[0].zp
(gdb) p output_attrs[0].scale

# NMS 后最终检测结果
(gdb) b nms
(gdb) p det_results->count
```

### 6.3 零拷贝内存检查

```
(gdb) p ctx.input_mems[0]->virt_addr
(gdb) p ctx.input_mems[0]->size
(gdb) p ctx.output_mems[0]->virt_addr
```

---

## 七、音频链路调试

### 7.1 Opus 编解码

```
(gdb) b AudioProcess::encode
(gdb) p pcm_frame.size()
(gdb) p pcm_frame[0]@10    # 前 10 个 PCM 采样

(gdb) b AudioProcess::PackBinFrame
(gdb) p payload_size

(gdb) b AudioProcess::decode
(gdb) p opus_data_size
```

### 7.2 音频队列状态

```
(gdb) p app->audio_processor_.recordedAudioQueue.size()
(gdb) p app->audio_processor_.playbackQueue.size()
(gdb) p app->audio_processor_.isRecording
(gdb) p app->audio_processor_.isPlaying
```

---

## 八、图片链路调试

### 8.1 帧缓存状态

```
(gdb) b VisionFrameBuffer::GetLatest
(gdb) p latest_.valid
(gdb) p latest_.seq
(gdb) p latest_.captured_ms
(gdb) p max_age_ms
```

### 8.2 图片编码发送

```
(gdb) b Application::DescribeCurrentScene
(gdb) p image.cols
(gdb) p image.rows
(gdb) p buf.size()         # JPEG 编码后大小
(gdb) p img_base64.size()  # base64 字符串长度
```

### 8.3 视觉请求串台排查

```
(gdb) p app->active_vision_request_id_
(gdb) b WSHandler::IsStaleRequest
(gdb) p root["request_id"].asString()
```

---

## 九、崩溃现场分析

### 9.1 Core Dump（开发板端）

```bash
# 开启 core dump
ulimit -c unlimited
echo "/data/core.%e.%p" > /proc/sys/kernel/core_pattern

# 运行到崩溃
/app/main

# 产生 /data/core.main.xxx 后，scp 回 PC 分析
```

### 9.2 PC 端分析 core dump

```bash
arm-rockchip830-linux-uclibcgnueabihf-gdb /path/to/bin/main /path/to/core.main.xxx
(gdb) bt full
(gdb) info registers
(gdb) info threads
(gdb) thread apply all bt full
```

### 9.3 常见崩溃排查

| 信号 | 原因 | 先看 |
|------|------|------|
| `SIGSEGV` | 空指针/野指针/栈溢出 | `bt full`，检查指针是否为 0 |
| `SIGABRT` | assert 失败或 double free | `bt full`，看 `__assert_fail` 参数 |
| `SIGBUS` | 未对齐内存访问 | 检查 `__attribute__((packed))` 结构体 |
| `SIGPIPE` | 向已关闭 socket 写数据 | WebSocket 断连未检查 |

---

## 十、条件断点

减少循环中的无效命中：

```
# 特定 request_id
(gdb) b Application::DescribeCurrentScene if request_id == "abc123"

# 特定状态
(gdb) b WSHandler::ws_msg_handle if app->client_state_.current_state_ == 3

# 非空帧
(gdb) b VisionFrameBuffer::Push if !frame.empty()

# 高置信度人脸
(gdb) b SubmitActiveVisionEvent if confidence > 0.8
```

---

## 十一、观测点（Watchpoint）

追踪变量被谁修改：

```
(gdb) watch app->client_state_.current_state_
(gdb) watch -l app->active_vision_request_id_

(gdb) commands
> bt 3
> continue
> end
```

硬件 watchpoint 通常只有 4 个，用完会变软件 watchpoint 极慢。

---

## 十二、运行日志

项目内置日志宏 `USER_LOG_INFO` / `USER_LOG_WARN` / `USER_LOG_ERROR` / `LV_LOG_ERROR`。

```bash
# 开发板端：重定向到文件
/app/main 2>&1 | tee /data/debug.log

# PC 端事后过滤
grep "WARN\|ERROR" /data/debug.log
grep "describe_scene\|vision_result" /data/debug.log
grep "Transition\|state" /data/debug.log
```

运行时用 GDB 打印内部状态：

```
(gdb) call printf("active_vision_request_id: %s\n", app->active_vision_request_id_.c_str())
```

---

## 十三、实用 .gdbinit

在项目根目录创建 `.gdbinit`：

```
set print pretty on
set print thread-events off
set pagination off
set history save on
set history filename ~/.gdb_history
set history size 10000

# 交叉调试 sysroot
# set sysroot /home/.../rv1106-sdk/sysdrv/.../sysroot

set target-async on
set non-stop on

# 一键查看项目全局状态
define projstate
  printf "=== State Machine ===\n"
  p app->client_state_.current_state_
  printf "=== Vision ===\n"
  p app->vision_frame_buffer_.latest_.valid
  p app->vision_frame_buffer_.latest_.seq
  printf "=== Audio ===\n"
  p app->audio_processor_.isRecording
  p app->audio_processor_.isPlaying
  p app->audio_processor_.recordedAudioQueue.size()
  p app->audio_processor_.playbackQueue.size()
  printf "=== WebSocket ===\n"
  p app->ws_client_.is_connected_
  printf "=== Thread ===\n"
  info threads
end
```

使用：`(gdb) projstate` 一键打印全部关键状态。
