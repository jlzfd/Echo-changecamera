# Echo Mate 驱动与底层学习文档索引

本目录围绕 RV1106 Echo Mate 项目的实际代码组织。建议先建立 Linux 驱动公共模型，再分别阅读摄像头、显示、音频和 NPU，最后回到 DMA-BUF 理解跨硬件共享与同步。

## 1. 推荐阅读顺序

1. [Linux 驱动开发基础与项目映射](LINUX_DRIVER_FUNDAMENTALS.md)
2. [摄像头与音频完整链路](CAMERA_AUDIO_DRIVER_STACK.md)
3. [MIPI 摄像头项目学习指南](MIPI_PROJECT_LEARNING_GUIDE.md)
4. [MIPI CSI-2 与 Linux 驱动指南](MIPI_CSI_LINUX_DRIVER_GUIDE.md)
5. [SPI/ST7789V 驱动修改说明](SPI_ST7789V_DRIVER_GUIDE.md)
6. [RV1106 音频与 ASoC 驱动指南](AUDIO_ASOC_DRIVER_GUIDE.md)
7. [DMA-BUF、Cache 与同步指南](DMA_BUF_AND_SYNC_GUIDE.md)
8. [NPU 架构与内存复用](NPU_ARCHITECTURE.md)

面向项目复盘、面试和调试的补充材料：

- [嵌入式项目架构学习指南](embedded_interview_architecture_guide.md)
- [项目知识点手册](interview_knowledge_points_handbook.md)
- [项目总结](interview_project_summary.md)
- [项目问答深入解析](interview_qa_deep_dive.md)
- [RKNN量化学习指南](interview_quantization_guide.md)
- [GDB调试指南](debug_gdb_guide.md)

## 2. 文档职责

| 文档 | 主要回答的问题 |
|---|---|
| `LINUX_DRIVER_FUNDAMENTALS.md` | DTS 如何变成 device，driver 如何匹配，probe、文件接口、中断、DMA和并发怎样串起来 |
| `CAMERA_AUDIO_DRIVER_STACK.md` | 从板级配置、DTS、内核到摄像头和音频应用的端到端总览 |
| `MIPI_PROJECT_LEARNING_GUIDE.md` | Sensor、D-PHY、CSI、CIF、ISP、V4L2以及一帧图像如何流动 |
| `MIPI_CSI_LINUX_DRIVER_GUIDE.md` | MIPI带宽、Media Controller、异步组装和驱动调用细节 |
| `SPI_ST7789V_DRIVER_GUIDE.md` | 当前ST7789V专用驱动、双缓冲、poll、spi_async和最新帧策略 |
| `AUDIO_ASOC_DRIVER_GUIDE.md` | simple-card、CPU DAI、Codec、DMAengine PCM、ALSA、PortAudio完整音频链路 |
| `DMA_BUF_AND_SYNC_GUIDE.md` | fd如何被各驱动导入、IOMMU映射、Cache一致性、Fence与当前项目同步方案 |
| `NPU_ARCHITECTURE.md` | RKNN执行、Arena复用、模型切换和NPU内存管理 |

## 3. 当前项目的四条核心数据链

```text
摄像头：Sensor → D-PHY → CSI-2 Host → CIF → ISP → V4L2 → RGA → NPU/LCD

显示：  RGB Buffer → RGA RGB565 → ST7789V DMA双缓冲 → SPI异步发送

录音：  Mic → Codec ADC → I2S RX → DMA → ALSA PCM → PortAudio → Opus/WebSocket

播放：  WebSocket/Opus → PCM队列 → PortAudio → ALSA PCM → DMA → I2S TX → Codec DAC/PA
```

## 4. 阅读时应区分的三组概念

### 控制面和数据面

- I2C、DTS属性、ioctl和寄存器配置通常属于控制面。
- MIPI像素、I2S PCM、SPI像素和DMA搬运属于数据面。

### 内存共享和同步

- DMA-BUF fd解决“如何引用同一块内存”。
- DMA API和`DMA_BUF_IOCTL_SYNC`解决CPU Cache可见性。
- Fence、完成量、回调或严格所有权解决“什么时候可以访问”。

### 当前实现和后续设计

- 当前Camera/RGA/NPU主要使用同步调用、Buffer所有权、互斥锁和状态变量。
- 当前LCD UAPI预留`in_fence_fd`，应用仍传`-1`，依赖RGA同步返回后再QUEUE。
- 显式Fence属于异步流水线的后续能力，不能当作当前已经启用的功能。

## 5. 源码入口

```text
内核DTS： rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/
内核驱动：rv1106-sdk/sysdrv/source/kernel/drivers/
ASoC驱动：rv1106-sdk/sysdrv/source/kernel/sound/soc/
摄像头应用：yolov5_demo/cpp/v4l2_capture.cc
零拷贝主链：yolov5_demo/cpp/AIcamera_c_interface.cc
DMA分配器：yolov5_demo/cpp/3rdparty/allocator/dma/dma_alloc.cpp
音频应用：AIChat_demo/Client/Audio/AudioProcess.cc
```



