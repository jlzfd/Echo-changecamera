# Echo-Mate — RV1106 端侧AI多模态交互系统

基于 Rockchip RV1106（Cortex-A7 + 0.5T NPU，256MB DDR）的端云协同对话机器人系统，实现「摄像头采集 → NPU 推理 → 云端 VLM → 语音交互」完整闭环。

## 硬件方案

| 模块 | 型号 | 接口 |
|------|------|------|
| SoC | Rockchip RV1106 | Cortex-A7 + NPU |
| 摄像头 | SmartSens SC3336 | MIPI CSI-2 2-lane |
| 显示屏 | Sitronix ST7789V | SPI |
| 音频 | RV1106 内置 Codec | I2S0 + 模拟 MIC/SPK |

## 软件架构

```
Camera Thread              State Machine Thread        WebSocket Thread
═════════════              ═════════════════        ════════════════

V4L2 DMA-BUF (ISP)         eventQueue_               ws_client_.run()
  ↓ RGA NV12→RGB fd→fd       ↓                         ↓
Arena CMA (RGB 640×640)    State: Idle               收到消息:
  ├→ NPU YOLO 推理          ├ KWS 唤醒词检测             ├ 二进制→Opus decode→播放
  ├→ NPU Face 推理          ├ wake_detected → Listening ├ JSON intent
  ├→ RGA→BGR (帧传递)       ├ Listening → Thinking        ├ look_at_environment
  ├→ RGA→LCD (RGB565)       ├ Thinking → Speaking           → DescribeCurrentScene
  └→ HW JPEG (MPP)          └ Speaking → Idle               → HW JPEG→base64→发送
         ↓                                              └ 状态事件→eventQueue_
    VisionFrameBuffer
         ↓
    DescribeCurrentScene → WebSocket → 云端 VLM
```

## 核心特性

- **零拷贝视觉管线**: V4L2 DMA-BUF + RGA fd→fd + RKNN, CPU 像素拷贝 0 次
- **NPU Arena 内存复用**: YOLOv5+SCRFD+KWS 三模型共享 DMA buffer, 节省 29%
- **MPP 硬件 JPEG**: RGA BGR→NV12 + MPP MJPEG, 零 CPU 编码
- **主动/被动双视觉触发**: NPU 检测主动上报 + 云端指令按需取帧
- **7 状态语音交互 FSM**: Idle↔Listening↔Thinking↔Speaking, KWS 唤醒

## 目录结构

```
├── AIChat_demo/          # 主程序 (状态机 + WebSocket + 音频 + 视觉)
│   ├── Client/
│   │   ├── Application/  # 状态机、视觉帧缓冲、事件处理
│   │   ├── Audio/        # PortAudio 录音/播放 + Opus 编解码
│   │   ├── WebSocket/    # WebSocket 客户端
│   │   ├── StateMachine/ # 通用状态机框架
│   │   ├── c_interface/  # C 导出接口
│   │   └── rga_convert.c # RGA 硬件转换封装
│   └── Server/           # Python 服务端 (ASR/VLM/TTS)
├── yolov5_demo/cpp/      # NPU 推理 + V4L2 采集 + RGA/MPP
│   ├── AIcamera_c_interface.cc  # 零拷贝推理主循环
│   ├── v4l2_capture.cc          # V4L2 DMA-BUF 采集
│   ├── hw_jpeg_encoder.cc       # MPP 硬件 JPEG
│   ├── npu_memory_reuse.cc      # NPU Arena 内存复用
│   ├── kws_detector.cc          # KWS 关键词唤醒
│   └── 3rdparty/allocator/dma/  # CMA/DMA-BUF 分配器
├── DeskBot_demo/         # LVGL GUI 桌面应用
└── .claude/skills/       # 知识卡片 (V4L2/ISP/RGA/CMA/MPP)
```

## 交叉编译

```bash
cd AIChat_demo/Client/build
cmake -DTARGET_ARM=ON \
  -DMPP_RELEASE_DIR=/path/to/rv1106-sdk/media/mpp/release_mpp_rv1106_arm-* \
  ..
make AIChatClient
```

## 板端运行

```bash
# 仅 YOLO
AIChatClient 172.32.0.100 8000 123456 /root/model/yolov5.rknn

# YOLO + 人脸检测
AIChatClient 172.32.0.100 8000 123456 /root/model/yolov5.rknn /root/model/face_detect.rknn
```

## 性能指标

| 指标 | 数值 |
|------|------|
| YOLOv5 推理 | ~15ms (INT8, 640×640) |
| 人脸检测推理 | ~12ms (INT8, 640×640) |
| KWS 延迟 | ~50ms |
| RGA 格式转换 | ~500μs |
| HW JPEG 编码 | ~2ms |
| 整体帧率 | ~25-30fps |
| DMA 内存节省 | 1.4MB (29%) |
| CPU 像素拷贝 | 0 次/帧 |
