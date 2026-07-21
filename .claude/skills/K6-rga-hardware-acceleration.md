# RGA 硬件加速 (Rockchip Graphics Acceleration) — 知识卡片

## 1. 一句话概括

RGA 是 Rockchip SoC 内置的 2D 加速器，通过 `importbuffer_fd` 将 dma-buf fd 映射为硬件可操作的 buffer，`improcess` 单次调用完成颜色空间转换 (CSC) + 缩放 (scaler) + 裁剪 (crop)，源和目标都通过 fd 传递，全程零 CPU 拷贝。

---

## 2. 为什么需要它

| 操作 | CPU (OpenCV) | RGA 硬件 |
|------|-------------|---------|
| NV12→RGB 640×480 | ~2ms | **~500μs** |
| RGB→RGB565 + resize 640→320 | ~1.5ms | **~300μs** |
| RGB→BGR 640×640 | ~1ms + 1.2MB memcpy | **~300μs, fd→fd** |
| CPU 占用 | 100% (A7 单核) | **0%** (硬件独立执行) |
| 功耗 | A7 @900MHz | RGA 独立时钟域 |

RV1106 单核 A7 用 OpenCV 处理每帧 ~10MB 像素拷贝，帧率只能 5fps。RGA 替代后 CPU 完全释放给 NPU 推理和业务逻辑。

---

## 3. Linux 底层原理

### 3.1 硬件架构

```
Userspace (librga.so)
    │
    │ ioctl(/dev/rga)
    ↓
Kernel (drivers/video/rockchip/rga3/)
    │
    ├─ rga_job_queue        → 任务队列，支持异步 + 同步
    ├─ rga_mm (memory mgr)  → dma-buf 导入/映射，物理地址解析
    ├─ rga_hw_config        → 配置 CSC/缩放/裁剪/旋转 寄存器
    └─ rga_irq_handler      → 完成中断 → 唤醒 waitqueue
                                │
                         ┌──────┴──────┐
                         │   RGA 硬件   │
                         │  AXI master  │ → DMA 读 src (通过 dma-buf 物理地址)
                         │  CSC 模块    │ → 颜色空间转换 (矩阵乘法)
                         │  Scaler      │ → 双线性/最近邻 缩放
                         │  Crop/Rotate │ → 裁剪/旋转
                         │  AXI master  │ → DMA 写 dst (通过 dma-buf 物理地址)
                         └──────────────┘
```

### 3.2 fd→fd 零拷贝路径原理

```
CPU 路径 (慢):
  fd → mmap → virt_addr → cv::cvtColor(virt, virt) → 新 cv::Mat → 新 virt_addr
  两次读物理内存 + 写物理内存 = 4 次 DDR 访问 × 像素总数

RGA fd→fd 路径 (快):
  importbuffer_fd(src_fd) → 内核查 dma-buf 物理地址 → RGA 直接 DMA 读
  importbuffer_fd(dst_fd) → 内核查 dma-buf 物理地址 → RGA 直接 DMA 写
  improcess → 一次硬件执行
  DDR 访问: RGA AXI 读 1 次 + 写 1 次 = 2 次 (且不经过 CPU cache)
```

### 3.3 RGA 版本差异

```
RV1106: RGA2 (librga.so, /dev/rga)
  - handle 模式 (importbuffer_fd + wrapbuffer_handle)
  - 需要 LIBRGA_IM2D_HANDLE 宏
  - 同步 improcess 阻塞等待完成

RK3588: RGA3 (librga3.so)
  - 纯 fd 模式，不需要 handle
  - 支持异步 + callback
  - 支持更多格式 (NV15/NV21 10bit)
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 用户态 API 调用链

```c
// ① 导入源 fd → RGA handle
im_handle_param_t src_param = {
    .width  = 640, .height = 480,
    .format = RK_FORMAT_YCbCr_420_SP,   // NV12
};
rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, &src_param);
// 内核: 通过 fd 找到 dma_buf → 获取物理地址 → 建立 RGA 内部映射

// ② 导入目标 fd (同上)
rga_buffer_handle_t dst_handle = importbuffer_fd(dst_fd, &dst_param);

// ③ 包装为 RGA buffer
rga_buffer_t src_buf = wrapbuffer_handle(src_handle, 640, 480, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst_buf = wrapbuffer_handle(dst_handle, 640, 640, RK_FORMAT_RGB_888);

// ④ 设置矩形区域
im_rect src_rect = {0, 0, 640, 480};
im_rect dst_rect = {0, 0, 640, 640};

// ⑤ 硬件处理 — 同步阻塞
IM_STATUS ret = improcess(src_buf, dst_buf, pat, src_rect, dst_rect, prect, 0);
// ioctl(RGA_BLIT_SYNC) → 等 RGA 中断 → 返回

// ⑥ 释放 handle
releasebuffer_handle(src_handle);
releasebuffer_handle(dst_handle);
```

### 4.2 格式映射表 (get_rga_fmt)

```c
IMAGE_FORMAT_GRAY8       → RK_FORMAT_8_8
IMAGE_FORMAT_RGB888      → RK_FORMAT_RGB_888
IMAGE_FORMAT_BGR888      → RK_FORMAT_BGR_888
IMAGE_FORMAT_RGBA8888    → RK_FORMAT_RGBA_8888
IMAGE_FORMAT_RGB565      → RK_FORMAT_RGB_565
IMAGE_FORMAT_YUV420SP_NV12 → RK_FORMAT_YCbCr_420_SP
IMAGE_FORMAT_YUV420SP_NV21 → RK_FORMAT_YCrCb_420_SP
```

### 4.3 图像描述符

```c
typedef struct {
    int width;              // 图像宽
    int height;             // 图像高
    int width_stride;       // 行步长 (有 padding 时 > width)
    int height_stride;      // 列步长
    image_format_t format;  // IMAGE_FORMAT_xxx
    unsigned char* virt_addr; // CPU 虚拟地址 (可 NULL if fd)
    int size;               // 总字节
    int fd;                 // dma-buf fd (-1 if virt only)
} image_buffer_t;
```

---

## 5. 完整调用流程 (以 NV12→RGB 为例)

```
convert_image(&src, &dst, NULL, NULL, 0)

① get_rga_fmt(src->format) → RK_FORMAT_YCbCr_420_SP
   get_rga_fmt(dst->format) → RK_FORMAT_RGB_888

② src->fd = 3 > 0 → importbuffer_fd(3, &param)
   → 内核 RGA_IOCTL_IMPORT_DMA_BUF → 解析 dma-buf → 获取 sg_table
   → 返回 rga_handle_src (内核内部编号)

③ dst->fd = 6 > 0 → importbuffer_fd(6, &param)
   → 同步骤②

④ wrapbuffer_handle → 填充 rga_buffer_t 结构
   (宽/高/格式/stride — RGA 硬件需要这些来算 DMA burst)

⑤ improcess(src_buf, dst_buf, pat, src_rect, dst_rect, prect, 0)
   → ioctl(RGA_BLIT_SYNC)
   → 内核: rga_hw_config() 写入 CSC/缩放寄存器
   → 内核: rga_dma_start() 发起 DMA
   → 内核: wait_for_completion() 等中断
   → 返回用户态

⑥ releasebuffer_handle → 内核释放 dma-buf 引用
```

---

## 6. 我的项目中使用 (4 条 RGA 路径)

```
每帧循环 (_inference_loop_zero_copy):

┌─────────────────────────────────────────────────────────┐
│ 路径 ①: ISP→Arena (NV12→RGB)                           │
│ src: V4L2 isp_fd, NV12 640×480                          │
│ dst: Arena CMA fd, RGB 640×640                          │
│ 操作: CSC + resize + fd→fd                              │
│ 代码: AIcamera_c_interface.cc:302                        │
│ 耗时: ~500μs                                             │
├─────────────────────────────────────────────────────────┤
│ 路径 ②: Arena→BGR (RGB→BGR)                            │
│ src: Arena CMA fd, RGB 640×640                          │
│ dst: CMA[0]/[1] double-buffer, BGR 640×640              │
│ 操作: CSC fd→fd                                         │
│ 代码: AIcamera_c_interface.cc:394                        │
│ 耗时: ~300μs                                             │
├─────────────────────────────────────────────────────────┤
│ 路径 ③: Arena→LCD (RGB→RGB565)                         │
│ src: Arena CMA fd, RGB 640×640                          │
│ dst: LCD CMA fd, RGB565 320×240                         │
│ 操作: CSC + resize + fd→fd                              │
│ 代码: AIcamera_c_interface.cc:404                        │
│ 耗时: ~300μs                                             │
├─────────────────────────────────────────────────────────┤
│ 路径 ④: BGR→NV12 (HW JPEG 预处理)                      │
│ src: BGR CMA fd, BGR 640×640                            │
│ dst: NV12 CMA fd, NV12 448×448                          │
│ 操作: CSC + resize + fd→fd                              │
│ 代码: hw_jpeg_encoder.cc:203                             │
│ 耗时: ~500μs                                             │
└─────────────────────────────────────────────────────────┘
```

---

## 7. 涉及源码

| 层 | 路径 | 关键内容 |
|----|------|---------|
| 用户态封装 | `AIChat_demo/Client/rga_convert.c` | `convert_image()` 独立实现 |
| 用户态封装 | `yolov5_demo/cpp/utils/image_utils.c` | 原始 `convert_image` (含 CPU fallback) |
| RGA 头文件 | `3rdparty/librga/include/im2d.h` | `importbuffer_fd`, `wrapbuffer_handle`, `improcess` |
| RGA 头文件 | `3rdparty/librga/include/drmrga.h` | `rga_buffer_t`, `im_rect`, `IM_STATUS` |
| RGA 内核 | `drivers/video/rockchip/rga3/` | RGA 驱动 + 中断处理 |
| DMA 导入 | `drivers/dma-buf/dma-buf.c` | `dma_buf_get` → 解析 fd 获取物理地址 |

---

## 8. 常见 Bug 及调试方法

### 8.1 importbuffer_fd 返回 -1

```
原因: fd 无效 / CMA 未分配 / fd 不属于 dma-buf
调试:
  ① ls -la /proc/<pid>/fd 确认 fd 有效
  ② cat /sys/kernel/debug/dma_buf/bufinfo 看 dma-buf 列表
  ③ dmesg | grep -i "rga.*import" 看内核错误
```

### 8.2 improcess 失败 — 格式不支持

```
原因: 格式组合不在 RGA 支持列表中
常见不支持: GRAY8→RGB565, NV12→BGR888 (需要两步)
调试:
  printf("src fmt=0x%x dst fmt=0x%x\n", get_rga_fmt(src), get_rga_fmt(dst));
  printf("RGA error: %s\n", imStrError(ret));
```

### 8.3 转换后颜色偏色 / 花屏

```
原因: 源格式填错了 (NV12 vs NV21, RGB vs BGR)
  NV12 = Y + UV 交替, NV21 = Y + VU 交替
  RGB888 vs BGR888: R 和 B 通道反了
修复: 确认 IMAGE_FORMAT_xxx 的值和实际内存布局一致
```

### 8.4 importbuffer_virtualaddr 慢 / 失败

```
原因: virt_addr 对应的物理内存不连续，RGA 需要 IOMMU map
  RV1106 没有 IOMMU → 只能用 fd (CMA 保证连续)
最佳实践: 永远用 fd 路径，virt_addr 设为 NULL 或仅做 fallback 标识
```

### 8.5 rga_convert.c 与 image_utils.c 冲突

```
rga_convert.c 是我们写的独立版本:
  - 去掉了 CPU fallback (convert_image_cpu)
  - 去掉了 stb_image/turbojpeg 依赖
  - 去掉了 rotate/flip/letterbox 等不用的功能
  - 保留了 get_rga_fmt + improcess 核心路径
  - RV1106 永远走 fd 路径 → 不需要 CPU 兜底
```

---

## 9. 性能优化点

| 优化 | 说明 | 效果 |
|------|------|------|
| fd→fd 路径 | 跳过 CPU mmap → 直接 DMA | 零 CPU, 快 3-4x |
| 合并操作 | CSC+resize 单次 improcess | 省一次 RGA 调用 |
| handle 模式 | importbuffer_fd 预导入 | 复用 handle，省 import 开销 |
| 避免 virt_addr | RV1106 无 IOMMU, virt path 慢 | 只用 fd |
| src_fd 对齐 | stride 16 对齐 → RGA burst 更高效 | ~10% 吞吐提升 |

---

## 10. 面试高频问题

1. **RGA 是什么？为什么比 OpenCV 快？** Rockchip 2D DMA 引擎，硬件做 CSC+缩放，不经 CPU memcpy。

2. **fd→fd 零拷贝路径原理？** importbuffer_fd 把 dma-buf 的物理地址导入 RGA，RGA 的 AXI master 直接 DMA 读源写目标。

3. **RGA 和 GPU 的区别？** RGA 是固定功能 2D 加速器 (CSC/Blit/Scale)，GPU 是可编程 3D 渲染。RGA 功耗更低，做 2D 操作更快。

4. **RV1106 的 RGA 有什么限制？** RGA2，单通道，不支持异步，无 IOMMU。必须用 CMA/fd 路径。

5. **importbuffer_fd 做了什么？** 内核通过 dma-buf API 解析 fd → 获取 scatter-gather table → 得到物理地址 → 存到 RGA 内部。

6. **RGA 支持哪些格式转换？** RGB↔BGR、NV12/NV21↔RGB、RGB→RGB565、灰度转换。不支持 NV12→NV21 (无意义)。
   
7. **rga_convert.c 为什么去掉了 CPU fallback？** RV1106 只用 fd 路径，RGA 从不失败。原版 CPU fallback 依赖 stb_image/turbojpeg → 增加 500KB 依赖 → 不划算。

8. **四条 RGA 路径能否并行？** 不能。RGA 是单通道硬件，每次 improcess 同步阻塞。如果同时提交两次 → 第二次排队等。

9. **RGA 和 MPP 的区别？** RGA 做图像前/后处理 (CSC/缩放)，MPP 做视频编解码 (MJPEG/H.264)。BGR→NV12→MPP 这条链：RGA 预处理 + MPP 编码。

10. **为什么 NV12 是 YUV420SP，不是 YUV420P？** SP = Semi-Planar (Y 独立, UV 交错)。RGA 和 MPP 都接受 SP 格式，P 格式需要额外转换。

---

## 11. 补充学习

- RGA 数据手册 (TRM 中 Multimedia → RGA 章节)
- 颜色空间: RGB/YUV 转换矩阵 (BT.601 vs BT.709)
- dma-buf 框架: `Documentation/driver-api/dma-buf.rst`
- RGA 内核驱动: `drivers/video/rockchip/rga3/rga_drv.c`
- NV12 内存布局: Y 在前 (w×h bytes), UV 交错在后 (w×h/2 bytes)
