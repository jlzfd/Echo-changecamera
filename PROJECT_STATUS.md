# 端侧AI多模态交互系统 — 项目完成情况

## 总进度

```
代码完成度:  ██████████████████░  95%
板端验证:    ██████████░░░░░░░░░░  50%
整体:        ████████████░░░░░░░░  70%
```

## 一、已完成的模块

### 基础感知层 ✅ 100%
- YOLOv5 物体检测（640x640 INT8，~30fps）
- SCRFD 人脸检测（640x640 INT8）
- 双模型并发推理（`dual_model_test`）
- LCD 实时显示（320x240 RGB565）
- CMake 交叉编译体系

### 端侧智能增强 ✅ 100%
- KWS 关键词唤醒（NPU 推理，延迟 ~50ms，替代 snowboy）
- IdleState 中 KWS 持续监听 + Arena 注册 + lock/unlock 保护

### NPU 内存复用 ✅ 100% (板端验证通过)
- 三模型（YOLO+Face+KWS）共享同一组 I/O DMA 缓冲区
- 节省 1.4MB（arena 3.4MB vs 独立 4.8MB）
- Arena 生命周期状态机 + bind-before-destroy 策略
- pthread_mutex_t 保护多线程竞争

### 端到端零拷贝 ✅ 100% (板端验证通过)
```
ISP → NV12 (V4L2 DMA-BUF fd)
  ↓ RGA fd→fd (NV12→RGB + resize)
CMA fd (RGB 640x640)
  ├→ NPU input (YOLO + Face)
  ├→ VisionFrameBuffer (云端 VLM)
  └→ LCD (RGA RGB→RGB565)
```
- V4L2 MPLANE + EXPBUF 零拷贝采集
- RGA NV12→RGB→BGR fd→fd 全链路
- LCD 路径 RGB→RGB565 硬件加速
- 每帧消除 ~10MB CPU memcpy

### AIChat 系统集成 ✅ 100% (代码完成)
- 7 状态 FSM：Fault→Startup→Idle↔Listening↔Thinking↔Speaking↔Stop
- WebSocket 云端通信 + 音频采集播放 + TTS

### HW JPEG 硬件编码 ✅ 100% (代码完成，待板端验证)
- RGA BGR→NV12 fd→fd + MPP MJPEG encode
- 零 CPU 拷贝 JPEG 编码
- `DescribeCurrentScene` 优先 HW 路径，失败 fallback CPU

---

## 二、未完成的模块

| 模块 | 进度 | 说明 |
|------|------|------|
| **板端部署验证** | 0% | 新编译的 AIChatClient 崩于 libcrypto SIGILL，未进 main() |
| **NPU/RGA 双缓冲流水线** | 0% | 帧 N 的 NPU 推理与帧 N+1 的 RGA 转换并行 |
| **1h 长跑稳定性** | 0% | 内存泄漏、Arena mutex 死锁、V4L2 缓冲区耗尽 |
| **工程清理** | 20% | 硬编码路径、内嵌 API Key、`#if 0` 废弃分支 |
| **混合量化板端验证** | 脚本完成 | `hybrid_quant.py` 已写，待板端编译验证 |

---

## 三、当前卡点

**libcrypto SIGILL 崩溃**：新编译的 AIChatClient 在 `main()` 之前崩于 `/usr/lib/libcrypto.so.1.1`，旧版正常。MD5 相同的库在旧程序中不崩，初步判断新工具链触发了不同的静态初始化路径。下一步 GDB 中跑 `x/i $pc` 定位非法指令。
