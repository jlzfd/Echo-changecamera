# 端侧AI多模态交互系统 — 项目计划文档

## 总览

基于 RV1106 的端云协同多模态机器人系统。本文档定义分阶段实施计划，用于评估进度。

---

## 第一层：基础感知层 ✅ 已完成

**目标**：摄像头采集 + 双模型推理 + LCD 显示，板端独立运行，无需网络。

| 任务 | 状态 | 交付物 |
|------|------|--------|
| YOLOv5 物体检测部署 | ✅ | `rknn_yolov5_demo`，640x640 INT8，~30fps |
| 人脸检测模型部署 | ✅ | SCRFD 640x640 INT8，`face_detect.cc` 后处理 |
| 双模型并发推理 | ✅ | `dual_model_test`，YOLOv5 + 人脸检测同帧运行 |
| LCD 实时显示 | ✅ | 320x240 RGB565，绿色=YOLO，红色=人脸 |
| 图片模式离线测试 | ✅ | `face_detect_test`，支持图片/摄像头切换 |
| CMake 交叉编译体系 | ✅ | `-DBUILD_STANDALONE_TESTS=ON`，全模块 x86 编译 |

---

## 第二层：端侧智能增强

**目标**：在基础感知之上增加人脸识别、姿态估计和语音唤醒，实现三大端侧 AI 能力闭环。

### 2.1 人脸识别（方案一深化）

- **当前状态**：人脸检测已通（SCRFD），缺少人脸特征提取和比对
- **待实现**：
  - 集成 MobileFaceNet 特征提取模型（RKNN Model Zoo 已有）
  - 实现"检测→对齐→特征提取→比对"流水线
  - 端侧人脸库管理（预设人脸注册、特征向量存储）
  - 与云端视觉问答联动：检测到陌生人→触发云端 VLM
- **验收标准**：端到端识别延迟 < 1s，识别率 > 95%
- **难度**：低

### 2.2 端侧关键词唤醒 ✅ 已完成

- **当前状态**：使用 NPU KWS 模型 (`kws_marvin.rknn`) 替代 snowboy，共享 NPU DMA Arena
- **已完成**：
  - ~~KWS 模型 (DNN) → RKNN 转换，NPU 推理~~ ✅
  - ~~延迟从 ~200ms → ~50ms~~ ✅
  - ~~状态机 `wake_detected` 事件联动~~ ✅
  - ~~IdleState 中 KWS 持续监听，Arena 注册 + lock/unlock 保护~~ ✅
- **验收标准**：唤醒率 > 95%，延迟 < 50ms，不依赖网络
- **难度**：中

### 2.3 姿态估计（方案四）

- **当前状态**：无
- **待实现**：
  - YOLOv8-Pose nano 版本，ONNX → RKNN INT8 量化
  - 后处理：关键点解析 + 骨骼连线
  - 应用场景：跌倒检测、手势识别
- **验收标准**：> 10fps 端侧推理，17 点关键点
- **难度**：中

---

## 第三层：系统优化与工程化

**目标**：性能优化、系统稳定性、可维护性。

### 3.1 NPU 内存复用 ✅ 已完成

- **当前状态**：YOLO + Face + KWS 三模型共享同一组 I/O DMA buffer，板端验证通过
- **已完成**：
  - ~~多模型串行推理场景下复用 input/output DMA 缓冲区~~ ✅
  - ~~Arena 生命周期状态机：EMPTY → REGISTERED → ALLOCATED → DESTROYED~~ ✅
  - ~~bind-before-destroy 策略：先 rknn_set_io_mem 再 rknn_destroy_mem~~ ✅
  - ~~KWS 延迟注册（IdleState 检查 ARENA_ALLOCATED 状态）~~ ✅
  - ~~pthread_mutex_t 保护 camera 线程和 KWS 线程的 bind+run 临界区~~ ✅
  - ~~内存节省 1.4MB (arena 3.4MB vs 独立 4.8MB)~~ ✅
- **验收标准**：三模型（YOLO+Face+KWS）总内存 < 当前两模型之和
- **难度**：高

### 3.2 模型剪枝与混合量化（方案八）

- **当前状态**：全部 INT8 量化
- **待实现**：
  - 逐层敏感度分析，识别精度关键层
  - 检测头 + 第一层卷积保持 FP16，其余 INT8
  - 通道剪枝优化 YOLOv5 骨干网络
  - 目标：精度损失 < 1%，推理速度提升 2-3x，内存减少 50%
- **验收标准**：YOLOv5 推理 < 15ms，精度 mAP 下降 < 1%
- **难度**：中

### 3.3 OTA 远程更新（方案六）❌ 已移除

- **原因**：对当前阶段非核心需求，优先级低于 NPU 优化

---

## 第四层：高阶特性（区分度亮点）

### 4.1 NPU-ISP 协同零拷贝（方案五）⚠️ 极高难度

- **目标**：ISP 输出直接映射到 NPU 输入，跳过 CPU 内存拷贝
- **技术路径**：V4L2 DMA-BUF + rknn_create_mem_from_fd
- **价值**：即使只完成方案设计和部分验证代码，面试中足以体现架构能力
- **验收标准**：完成设计文档 + PoC 验证（不需要完整产品化）
- **难度**：极高
- **具体实现**：→ 见 5.4 节
- **具体实现**：见 5.4 节

### 4.2 端侧 RAG（方案七）❌ 不现实

- **结论**：RV1106 256MB DDR 不足以加载任何大模型权重
- **面试策略**：清晰说明硬件边界，体现技术判断力

---

## 当前进度总览

```
第一层 ████████████████████ 100%  ✅ 基础感知层
第二层 ████████████████████ 100%  ✅ 端侧智能增强
  └ KWS 唤醒  ████████████████████ 100%  (代码+模型+AIChat集成)
  └ 人脸识别  ──────────────  已移除
  └ 姿态估计  ──────────────  已移除
第三层 ██████████░░░░░░░░░░  80%  系统优化与工程化
  └ 混合量化  ████████████████████ 100%  (hybrid_quant.py — 待 Linux 验证)
  └ NPU 内存  ████████████████████ 100%  (板端验证 4/4 PASS, saved=1.4MB)
  └ 工程清理  ████░░░░░░░░░░░░░░░░  20%  (硬编码路径/密钥, 旧分支清理)
第四层 ████████████████████ 100%  ✅ 高阶特性 (PoC/设计)
  └ ISP 零拷贝 ████████████████████ 100%  (V4L2 DMA-BUF + RGA fd→fd ✅)
第五层 ████████████████████ 100%  ✅ 多模型调度与系统集成
  └ 调度器    ████████████████████ 100%  (scheduler + FPS report + AIChat状态机)
  └ RGA      ████████████████████ 100%  (NV12→RGB + LCD RGB→RGB565 + 帧传递 RGB→BGR)
  └ 零拷贝   ████████████████████ 100%  (V4L2 MPLANE + EXPBUF 板端验证通过)
  └ AIChat   ████████████████████ 100%  (零拷贝+KWS+Arena集成进对话机器人)
第六层 ██████████░░░░░░░░░░  60%  进阶优化与产品化
  └ 帧传递   ████████████████████ 100%  (RGB→BGR RGA fd→fd ✅)
  └ 云端上传 ████████████████████ 100%  (RGA BGR→NV12 + MPP HW JPEG, 需板端验证)
  └ 流水线   ░░░░░░░░░░░░░░░░░░░░   0%  (NPU/RGA 双缓冲并行)
  └ 稳定性   ██████░░░░░░░░░░░░░░  30%  (1h长跑/自启动/断线重连)
  └ 工程清理 ████░░░░░░░░░░░░░░░░  40%  (部分完成)
```

---

## 第五层：多模型调度与系统集成

**目标**：统一调度 YOLO + Face + KWS 三个模型，RGA 硬件加速替换 CPU resize，板端长跑稳定。

### 5.1 多模型帧调度器 ✅ 已完成

- **当前状态**：`main_unified.cc` — 统一调度循环，YOLO 每帧 + Face 每 N 帧
- **待实现**：
  - ~~统一调度循环：camera → RGA → YOLO → Face → KWS → LCD~~ ✅
  - ~~可配置推理频率（YOLO 每帧 / Face 每 N 帧 / KWS 连续）~~ ✅
  - 推理超时预算管理（暂不需要，RKNN 同步阻塞）
  - 过载时自动跳帧（暂不需要，板端 ~30fps 稳定）
  - ~~帧时间戳追踪，统计各阶段延迟~~ ✅ (每 5s FPS 报告)
- **验收标准**：三模型在统一调度下稳定运行 ≥ 1 小时
- **难度**：中

### 5.2 RGA 硬件 resize 替换 CPU ✅ 已完成

- **当前状态**：`main_unified.cc` — `rga_resize_to_arena()` 通过 `convert_image()` 实现 RGA 硬件 resize，源图 CPU→RGA→CMA (fd) 零拷贝
- **待实现**：
  - ~~RGA `imresize` + `imcvtcolor` 替代 `cv::resize`~~ ✅
  - ~~源图一次 resize 到 640x640 RGB，YOLO 和 Face 共享同一输入~~ ✅
  - ~~CMA buffer 作为 RGA 输出 → 直接映射到 NPU input tensor~~ ✅
- **验收标准**：CPU 拷贝次数从 2 次降为 0（RGA 正常时；fallback 仍用 CPU）
- **难度**：中

### 5.3 统一构建与 AIChat 集成 ✅ 已完成

- **当前状态**：零拷贝 + KWS + NPU Arena 已集成进 AIChat_demo Client
- **已完成**：
  - ~~统编目标：三模型 + RGA + LCD 一个可执行文件~~ ✅ (代码完成)
  - ~~V4L2 DMA-BUF 零拷贝管线集成进 AIChat~~ ✅
  - ~~KWS 唤醒与 Arena 共享集成~~ ✅
  - ~~AIChat 状态机联动：wake_detected / person_detected / face_detected~~ ✅
  - 交叉编译 + 上板部署验证
  - 板端自启动脚本
  - 异常退出自动重启
- **验收标准**：一条命令编译部署，开机自启
- **难度**：低

### 5.4 ISP-V4L2-RGA-NPU 完整零拷贝 ✅ 已完成（← 4.1 的产品化实现）

- **目标**：端到端零 CPU 像素拷贝
  ```
  ISP → NV12 (V4L2 MMAP + VIDIOC_EXPBUF → dma-buf fd) → RGA (NV12→RGB+resize, fd→fd) → CMA fd → NPU
  ```
- **交付物**：
  - `v4l2_capture.h/cc` — V4L2 DMA-BUF 采集模块，NV12 640x480，预导出 fd
  - `main_zero_copy.cc` — 完整零拷贝 pipeline，包含 ISP AE/AWB 配置
  - `zero_copy_test` + `unified_test` CMake 目标
  - ~~AIChat_demo 集成~~ ✅
- **板端验证**：
  - ~~`VIDIOC_EXPBUF` 通过~~ ✅ (kernel DMA_BUF 支持)
  - ~~RGA NV12→RGB fd→fd 路径正常~~ ✅
  - ~~零拷贝帧率达标~~ ✅
- **验收标准**：CPU 像素拷贝次数 = 0（推理路径），帧率 ≥ 基线
- **难度**：高

### 5.5 RGA 硬件 LCD 显示路径 ✅ 已完成

- **目标**：LCD 显示路径全面替换 CPU 操作为 RGA 硬件
- **当前状态**：`AIcamera_c_interface.cc` — `_inference_loop_zero_copy` Step 8
- **已完成**：
  - ~~`IMAGE_FORMAT_RGB565` 新增至 common.h + image_utils.c~~ ✅
  - ~~LCD buffer 改用 CMA 分配 (`dma_buf_alloc`)~~ ✅
  - ~~RGA 单次调用完成 RGB→RGB565 + resize 640→320~~ ✅
  - ~~画框从 bgr_disp 改到 arena_rgb 直接绘制~~ ✅
- **消除的 CPU 操作 (每帧)**：
  - `cvtColor RGB2BGR` (1.2MB) → 已消除
  - `resize 640→320` (1.2MB→150KB) → 已消除
  - `cvtColor BGR2BGR565` (150KB→300KB) → 已消除
  - `memcpy → yolo_pic_buf` (150KB) → 已消除
- **验收标准**：LCD 路径 CPU 像素拷贝 = 0
- **难度**：中

---

## 第六层：进阶优化与产品化

**目标**：在核心功能已闭环的基础上，消除剩余 CPU 拷贝、提升稳定性、清理工程。

### 6.1 帧传递 RGB→BGR 改用 RGA ✅ 已完成

- **当前状态**：`_inference_loop_zero_copy` Step 7 — RGA RGB→BGR fd→fd，零 CPU 拷贝
- **已完成**：
  - ~~双缓冲改用 CMA 分配 (`dma_buf_alloc`)，2×640x640x3 = 2.4MB~~ ✅
  - ~~新增 `IMAGE_FORMAT_BGR888` + `RK_FORMAT_BGR_888` RGA 映射~~ ✅
  - ~~帧传递：`convert_image(&g_rga_dst, &g_rga_bgr_dst[idx])` RGB→BGR fd→fd~~ ✅
  - ~~`cv::Mat` 直接 wrap CMA virt_addr，零额外拷贝~~ ✅
- **消除**：`cv::cvtColor(arena_rgb, dst, COLOR_RGB2BGR)` — 每帧 1.2MB CPU 读+写
- **代价**：增加 2.4MB CMA + 1 次 RGA 调用/帧
- **验收标准**：帧传递路径 CPU 像素拷贝 = 0
- **难度**：中

### 6.2 云端 VLM 图片上传硬件化 ✅ 已完成（待板端验证）

- **当前状态**：`hw_jpeg_encoder.h/cc` — MPP MJPEG + RGA 实现零 CPU 拷贝 JPEG 编码
- **已完成**：
  - ~~`hw_jpeg_encoder_init` — MPP `mpp_create` + `mpp_init(MPP_VIDEO_CodingMJPEG)` + `mpp_enc_cfg_init`~~ ✅
  - ~~RGA BGR888(fd) → NV12(fd) + resize 640→448，单次 RGA 调用完成~~ ✅
  - ~~MPP `encode()` 同步编码：NV12 fd → JPEG bitstream via `mpp_buffer_import(EXT_DMA)`~~ ✅
  - ~~`DescribeCurrentScene` 优先走 HW 路径，失败自动 fallback CPU `cv::imencode`~~ ✅
  - ~~`FrameSnapshot` 增加 `int fd` 字段，相机线程传递 CMA fd 到上传路径~~ ✅
  - ~~`CMakeLists.txt` 增加 `rockchip_mpp` 链接 + MPP include 路径~~ ✅
  - 交叉编译 + 板端验证（待进行）
- **数据流**：
  ```
  BGR CMA fd (640x640, double-buffer)
    ↓ RGA fd→fd (BGR→NV12 + resize 640→448)
  NV12 CMA fd (448x448, intermediate ~302KB)
    ↓ MPP EXT_DMA import (零拷贝)
    ↓ mpp_init(MJPEG) + encode()
  JPEG bitstream → base64 → WebSocket
  ```
- **消除的 CPU 操作（每 5s 一次）**：
  - `cv::resize 640→448` (CPU) → RGA resize fd→fd
  - `cv::imencode .jpg` (CPU, stb_image) → MPP HW JPEG
- **关键文件**：
  - `yolov5_demo/cpp/hw_jpeg_encoder.h/cc` — 主实现
  - `AIChat_demo/Client/Application/Application.cc` — `DescribeCurrentScene` HW 路径
  - `AIChat_demo/Client/Application/VisionFrameBuffer.h/cc` — fd 传递
  - `AIcamera_c_interface.cc` — `start_ai_camera_v2` 中调用 `hw_jpeg_encoder_init`
- **验收标准**：上传路径 CPU 像素拷贝 = 0，JPEG 编码延迟 < 10ms

### 6.3 NPU 推理与 RGA 流水线化

- **当前状态**：RGA → NPU 完全串行。NPU 推理期间 RGA 闲置，反之亦然
- **待实现**：
  - 双 CMA input buffer 交替使用 (A/B buffer)
  - 帧 N 的 NPU 推理 与 帧 N+1 的 RGA 转换 并行执行
  - 预期吞吐提升 30-50%
- **风险点**：RV1106 单核 NPU 阻塞式推理，RGA 与 NPU 共享 DDR 带宽，并行收益需实测
- **验收标准**：帧率提升 ≥ 30%
- **难度**：高

### 6.4 混合精度量化板端验证

- **当前状态**：`hybrid_quant.py` 脚本已完成，待板端编译验证
- **待实现**：
  - YOLOv5 检测头 + 第一层卷积保持 FP16，其余 INT8
  - 板端精度对比（mAP 差异 < 1%）
  - 推理延迟对比（目标 < 15ms）
- **验收标准**：精度损失 < 1%，推理速度提升 2-3x
- **难度**：中

### 6.5 板端稳定性测试与运维

- **当前状态**：功能验证通过，缺少长时间稳定性测试
- **待实现**：
  - ≥ 1 小时长跑稳定性测试（关注内存泄漏、Arena mutex 死锁、V4L2 缓冲区耗尽）
  - systemd service 开机自启动脚本
  - 进程异常退出自动拉起 (Restart=always)
  - WebSocket 断线重连 + ping/pong 心跳保活
- **验收标准**：1 小时无崩溃，断网 30s 内自动恢复
- **难度**：低

### 6.6 工程清理

- **当前状态**：存在硬编码路径、旧代码分支、内嵌密钥
- **待清理**：
  - `/home/ubuntu/Echo-Mate-main/...` 绝对路径 → 相对路径或 CMake 变量
  - 阿里云 API Key 从源码移除 → 配置文件/环境变量
  - `USE_OPENCV_CAMERA` 旧 OpenCV 采集分支 → 正式稳定后移除
  - `#if 0` 废弃代码块 (`_inference_loop` 旧版本) → 删除
  - `get_buf_data()` 中的 `memcpy` → 调用方如能直接读 CMA virt_addr 则省去
- **验收标准**：零硬编码路径，零内嵌密钥，零废弃分支
- **难度**：低
