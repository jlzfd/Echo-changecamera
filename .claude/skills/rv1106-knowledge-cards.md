# RV1106 项目知识卡片 Skill

调用此 skill 时，根据用户指定的知识点输出对应卡片。

## 可用知识点

用户可指定以下任一知识点（或说"全部"输出所有）：

| 编号 | 知识点 | 一句话 |
|------|--------|--------|
| K1 | V4L2 DMA-BUF 零拷贝采集 | ISP 输出的 NV12 帧通过 dma-buf fd 传递，不经过 CPU |
| K2 | RGA 硬件加速 | 全志/瑞芯微 2D 加速器，fd→fd 零拷贝格式转换+缩放 |
| K3 | CMA/DMA-BUF 内存管理 | Linux 连续物理内存分配器，用户态通过 fd 访问硬件可见内存 |
| K4 | NPU Arena 内存复用 | 多模型共享同一组 I/O DMA buffer，节省 29% 内存 |
| K5 | MPP 硬件 JPEG 编码 | 调用 MJPEG 硬件编码器，NV12 fd 输入→JPEG bitstream 输出 |
| K6 | cv::Mat 零拷贝帧传递 + 双缓冲 | 引用计数保证安全，双缓冲隔离读写，72 字节头替代 1.2MB 拷贝 |
| K7 | ALSA + PortAudio 音频链路 | I2S DMA→ALSA ring buffer→libasound→PortAudio 回调 |
| K8 | DTS 驱动适配 | 不改驱动代码，只写 DTS 让原厂驱动在自定义板子上工作 |
| K9 | 多线程状态机 + 主动/被动视觉 | 7 状态 FSM，camera/audio/websocket 三线程协作 |
| KA | WebSocket + Opus 语音交互 | 16kHz PCM→Opus 编码→二进制帧→云端 ASR→TTS 回传 |

---

## K1: V4L2 DMA-BUF 零拷贝采集

### 1. 一句话概括
Linux V4L2 框架下，通过 `VIDIOC_EXPBUF` 将 kernel DMA buffer 导出为 dma-buf fd，用户态拿到 fd 后传给 RGA/NPU，全程不经过 CPU memcpy。

### 2. 为什么需要它
传统 `cv::VideoCapture >> cv::Mat` 每帧 640×480×1.5(NV12) = 460KB + resize 拷贝。在 256MB RV1106 上，零拷贝省 CPU、省内存带宽、省延迟。

### 3. Linux 底层原理
```
Kernel space:
  V4L2 驱动 (rkisp) → vb2_queue → vb2_dma_contig → dma_alloc_coherent → CMA
                                              ↓
                    VIDIOC_EXPBUF → dma_buf_fd(dmabuf->file->fd)
                                              ↓
Userspace:            fd=3  ─→  RGA importbuffer_fd(3) → DMA 读 NV12
```
dma-buf 是 Linux 3.3 引入的跨设备共享框架。EXPBUF 把 vb2 buffer 的 dma-buf 文件描述符导出给用户态。

### 4. 核心数据结构/关键 API
```c
// 请求导出 fd
struct v4l2_exportbuffer exp = {
    .type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .index = buf_idx,         // 第几个 buffer
    .flags = O_RDWR,
};
ioctl(fd, VIDIOC_EXPBUF, &exp);  // → exp.fd

// 归还 buffer
ioctl(fd, VIDIOC_QBUF, &buf);    // 重新入队给驱动填充
```

### 5. 完整调用流程
```
v4l2_capture_init("/dev/video11", 640, 480)
  → open → VIDIOC_QUERYCAP → VIDIOC_S_FMT(NV12, MPLANE)
  → VIDIOC_REQBUFS(count=4, MEMORY_MMAP)
  → for i in 0..3: VIDIOC_QUERYBUF → mmap → VIDIOC_QBUF
  → VIDIOC_STREAMON
  → for i in 0..3: VIDIOC_EXPBUF → 预导出 fd[i]

每帧:
  v4l2_capture_get_frame()
    → VIDIOC_DQBUF (阻塞等驱动填完) → 返回 buf_idx, fd
  ... 用户使用 fd ...
  v4l2_capture_put_frame()
    → VIDIOC_QBUF (归还)
```

### 6. 我的项目中使用
- [v4l2_capture.cc](D:\echo\yolov5_demo\cpp\v4l2_capture.cc) — 封装 V4L2 初始化和帧获取
- [AIcamera_c_interface.cc:297](D:\echo\yolov5_demo\cpp\AIcamera_c_interface.cc#L297) — 主循环中 `get_frame(&isp_fd)` → fd 传给 RGA
- [AIcamera_c_interface.cc:309](D:\echo\yolov5_demo\cpp\AIcamera_c_interface.cc#L309) — RGA 完成后立即 `put_frame`

### 7. 涉及源码
| 文件 | 关键函数 |
|------|---------|
| `v4l2_capture.h` | `v4l2_capture_t` 结构体 |
| `v4l2_capture.cc` | `v4l2_capture_init()`, `_get_frame()`, `_put_frame()` |
| `AIcamera_c_interface.cc` | `_inference_loop_zero_copy()` 第 1-3 步 |
| 内核: `drivers/media/platform/rockchip/cif/` | ISP 驱动 |

### 8. 常见 Bug 及调试方法
| Bug | 原因 | 调试 |
|-----|------|------|
| `VIDIOC_EXPBUF` 返回 -ENOTTY | 内核没开 `CONFIG_DMA_BUF` | `zcat /proc/config.gz \| grep DMA_BUF` |
| DQBUF 阻塞超时 | ISP 没输出，检查 sensor 是否 streaming | `v4l2-ctl --stream-mmap --stream-count=1` |
| fd=-1 | EXPBUF 失败但没检查返回值 | 加 assert(fd >= 0) |
| buffer 耗尽 | get/put 不配对 | `cat /sys/kernel/debug/vb2/` 看队列状态 |

### 9. 性能优化点
- EXPBUF 一次性预导出，不要每帧调 ioctl
- put_frame 在 RGA 完成后立即调用，不要拖到循环末尾
- V4L2 buffer count 设置 3-4 个够用，多了浪费 CMA

### 10. 面试高频问题
1. V4L2 的 MMAP vs DMA-BUF 区别？
2. EXPBUF 导出的是什么？fd 的生命周期？
3. 为什么 put_frame 要尽早归还？
4. vb2_queue 的 buffer 状态机：QUEUED→ACTIVE→DONE→QUEUED
5. ISP pipeline：sensor→csi→cif→isp 各层做什么？
6. V4L2_CAP_VIDEO_CAPTURE_MPLANE vs SINGLE_PLANE？
7. 如何确认 DMA-BUF 是真正的零拷贝？
8. VIDIOC_STREAMON/OFF 过程中 driver 做了什么？
9. 如果连续多帧 DQBUF 超时，如何恢复？
10. dma-buf 的引用计数机制？什么时候真正释放？

### 11. 补充学习
- Linux `Documentation/driver-api/dma-buf.rst`
- V4L2 spec: `Documentation/userspace-api/media/v4l/`
- `v4l2-ctl` 调试工具使用
- RKISP 3A (AE/AWB/AF) 算法流程

---

## K2: RGA 硬件加速 fd→fd

### 1. 一句话概括
Rockchip RGA (2D Graphics Accelerator) 实现颜色空间转换+缩放+裁剪的单次硬件调用，支持 fd→fd 零 CPU 拷贝。

### 2. 为什么需要它
替代 `cv::cvtColor + cv::resize`，每条路径每帧节省 ~1.2MB CPU memcpy。ISP 输出是 NV12，NPU 只吃 RGB，LCD 只吃 RGB565——没有 RGA 的话每个转换都要 CPU 做。

### 3. Linux 底层原理
```
RGA 硬件 (SoC 内部 IP):
  AXI 总线 master → 通过 dma-buf fd 获取物理地址 → DMA 读源 → 
  内部 CSC(颜色空间) + scaler(缩放) → DMA 写目标 → 中断通知完成
```
RGA 在内核中注册为 misc device (`/dev/rga`)，用户态通过 librga 的 ioctl 或 wrapper API 调用。

### 4. 核心数据结构/关键 API
```c
// RGA 源/目标描述
image_buffer_t src = {
    .format   = IMAGE_FORMAT_NV12,     // 源格式
    .width    = 640, .height = 480,
    .fd       = isp_fd,                // dma-buf fd，零拷贝关键
    .virt_addr = NULL,                 // fd 有效时可以为 NULL
};

// 核心调用
int convert_image(&src, &dst, NULL, NULL, 0);
// 内部: importbuffer_fd → wrapbuffer_handle → improcess → releasebuffer_handle
```

### 5. 完整调用流程
```
convert_image(src_fd, dst_fd)
  → get_rga_fmt(src.format) → 查表: NV12→RK_FORMAT_YCbCr_420_SP
  → importbuffer_fd(src_fd, &param)  → rga_handle_src ← 内核映射 fd→物理地址
  → importbuffer_fd(dst_fd, &param)  → rga_handle_dst
  → wrapbuffer_handle()              → rga_buffer_t 封装
  → improcess(src, dst, rect, usage) → ioctl(RGA_BLIT_SYNC) 同步等硬件完成
  → releasebuffer_handle()           → 释放内核 handle
```

### 6. 我的项目中使用（四条 RGA 路径）
| 路径 | 源→目标 | 代码位置 |
|------|--------|---------|
| ISP→Arena | NV12 640×480 → RGB 640×640 | AIcamera_c_interface.cc:302 |
| Arena→BGR | RGB 640×640 → BGR 640×640 fd→fd | AIcamera_c_interface.cc:394 |
| Arena→LCD | RGB 640×640 → RGB565 320×240 fd→fd | AIcamera_c_interface.cc:404 |
| BGR→NV12 (HW JPEG) | BGR 640×640 → NV12 448×448 fd→fd | hw_jpeg_encoder.cc:203 |

独立封装: [rga_convert.c](D:\echo\AIChat_demo\Client\rga_convert.c)

### 7. 涉及源码
| 文件 | 关键函数 |
|------|---------|
| `rga_convert.c` | `convert_image()`, `get_rga_fmt()` |
| `3rdparty/librga/include/im2d.h` | RGA handle API |
| `3rdparty/librga/include/drmrga.h` | rga_buffer_t, im_rect |
| 内核: `drivers/video/rockchip/rga3/` | RGA 内核驱动 |

### 8. 常见 Bug 及调试方法
| Bug | 原因 | 调试 |
|-----|------|------|
| `importbuffer_fd` 返回 -1 | fd 无效或 CMA 未分配 | `ls -la /proc/<pid>/fd` 确认 fd 有效 |
| `improcess` 返回错误 | 格式不支持或 stride 不对齐 | 打印 `imStrError()` |
| 转换后画面花/偏色 | 格式 enum 写错 | 对比 `RK_FORMAT_xxx` 和实际格式 |
| `size_t` 未定义 | 缺少 `#include <stddef.h>` | 在 drmrga.h 之前 include |
| 两条 RGA 管线冲突 | 软件 fallback 路径不支持 fd | 确保 fd 有效时走硬件路径 |

### 9. 性能优化点
- fd→fd 是最高效路径（跳过 CPU 映射），profile 确认 `importbuffer_fd` < 50μs
- 合并多次操作为一次：RGB→RGB565+resize 在同一个 improcess 完成
- 避免 `importbuffer_virtualaddr`（需要内核 iommu map）

### 10. 面试高频问题
1. RGA fd→fd 为什么是零拷贝？
2. improcess 内部做了什么？
3. importbuffer_fd 的参数含义？
4. NV12 和 YUV420SP 的关系？
5. RGA 支持哪些格式转换组合？
6. dma_sync_device_to_cpu 在 RGA 之后为什么需要？
7. rga_buffer_handle_t 和 rga_buffer_t 的区别？
8. 如何判断 RGA 支持 handle 模式？（`LIBRGA_IM2D_HANDLE`）
9. RV1106 的 RGA 是哪个版本？有什么限制？
10. 如果 RGA 失败，CPU fallback 怎么实现？

### 11. 补充学习
- RGA 数据手册中的格式转换矩阵
- YUV 色彩空间原理（BT.601 vs BT.709）
- DMA fence/reservation 对象

---

## K3: CMA/DMA-BUF 内存管理

### 1. 一句话概括
CMA (Contiguous Memory Allocator) 从预留物理连续区域分配内存，通过 dma-buf 框架导出 fd 给用户态，硬件通过物理地址直接访问。

### 2. 为什么需要它
NPU/RGA/MPP 需要物理连续内存才能 DMA。普通 `malloc` 得到的虚拟地址背后可能是离散物理页，DMA 引擎读不了。CMA 保证连续。

### 3. Linux 底层原理
```
启动时:
  kernel cmdline: "cma=64M" → 预留 64MB 连续物理内存
  cma_declare_contiguous() → 从 memblock 切一块

运行时:
  dma_buf_alloc(heap_path, size, &fd, &virt)
    → 打开 /dev/dma_heap/linux,cma
    → ioctl(DMA_HEAP_IOCTL_ALLOC) → 分配连续物理页 → 返回 fd
    → mmap(fd) → virt_addr
```
dma-buf 用引用计数管理生命周期：fd 打开一次 count+1，所有 fd 关闭后内核释放物理内存。

### 4. 核心数据结构/关键 API
```c
// 分配 CMA buffer——项目核心封装
int dma_buf_alloc(const char *heap_path, size_t size,
                  int *fd, void **virt_addr);
// 返回: fd=3(硬件能DMA), virt_addr=0xb6600000(CPU能读写的虚拟映射)

void dma_buf_free(size_t size, int *fd, void *virt);
// munmap + close(fd) → 引用计数-1 → 归零后 CMA 回收

void dma_sync_device_to_cpu(int fd);  // 确保 CPU 能读到硬件刚写的数据
void dma_sync_cpu_to_device(int fd);  // 确保硬件能读到 CPU 刚写的数据
```

### 5. 完整调用流程
```
dma_buf_alloc(RV1106_CMA_HEAP_PATH, 1228800, &fd, &virt)
  → fd = open("/dev/dma_heap/linux,cma", O_RDWR)
  → ioctl(DMA_HEAP_IOCTL_ALLOC, {size=1228800}) → 物理连续页
  → virt = mmap(fd, MAP_SHARED) → 建立用户态虚拟映射
  → *fd = 3, *virt = 0xb6600000

dma_buf_free(size, &fd, virt)
  → munmap(virt, size)  → 解除虚拟映射
  → close(fd)           → 最后一个引用关闭 → CMA 回收
```

### 6. 我的项目中使用
| 用途 | 大小 | 代码位置 |
|------|------|---------|
| NPU Arena input | 640×640×3 = 1.2MB | npu_memory_reuse.cc:229 |
| NPU Arena outputs | 3×~300KB = ~900KB | npu_memory_reuse.cc:239 |
| BGR 双缓冲 CMA[0]/[1] | 2×1.2MB = 2.4MB | AIcamera_c_interface.cc:817 |
| LCD 显示 buffer | 320×240×2 = 150KB | AIcamera_c_interface.cc:790 |
| HW JPEG NV12 中间 buffer | 448×448×1.5 = 302KB | hw_jpeg_encoder.cc:106 |
| 总计 | ~6.8MB | (RV1106 256MB 中 CMA 设 64MB) |

### 7. 涉及源码
| 文件 | 关键函数 |
|------|---------|
| `3rdparty/allocator/dma/dma_alloc.h` | `dma_buf_alloc()`, `dma_buf_free()` |
| `3rdparty/allocator/dma/dma_alloc.cpp` | `DmaAllocator` 实现 |
| 内核: `drivers/dma-buf/` | dma-buf 核心框架 |
| 内核: `mm/cma.c` | CMA 分配器 |

### 8. 常见 Bug 及调试方法
| Bug | 原因 | 调试 |
|-----|------|------|
| `dma_buf_alloc` 返回 -1 | CMA 耗尽 | `cat /proc/meminfo \| grep Cma` |
| SIGSEGV at virt_addr | dma_sync 没调，cache 不一致 | 检查 sync 顺序 |
| 硬件读到垃圾数据 | CMA 被 CPU写过但没 sync | 加 `dma_sync_cpu_to_device` |
| fd 泄漏导致 CMA 耗尽 | 忘记 close(fd) | `ls /proc/<pid>/fd \| wc -l` |
| 板端找不到 `/dev/dma_heap/linux,cma` | 内核没开 `CONFIG_DMABUF_HEAPS_CMA` | check kernel config |

### 9. 性能优化点
- CMA 分配有碎片化风险，init 时一次性全部分配，运行时不再 alloc/free
- Cache sync 的范围尽量精确（写多少 sync 多少），不要全量 flush
- RV1106 没有硬件 cache coherent，必须手动 sync（dma_sync_*）

### 10. 面试高频问题
1. CMA vs kmalloc vs vmalloc 的区别？
2. 为什么 DMA 需要物理连续内存？
3. dma-buf fd 传递给另一个进程后，原进程 close(fd) 内存会释放吗？
4. cache coherency 问题：什么叫"CPU 看不到 DMA 写的数据"？
5. RV1106 为什么需要 dma_sync？哪些 SoC 不需要？
6. CMA 区域要不要配 coherent_pool？
7. `/dev/dma_heap/linux,cma` 和 `/dev/dma_heap/system` 的区别？
8. 如何查看当前 CMA 使用量？
9. dma_buf_alloc 的 mmap 为什么用 MAP_SHARED？
10. 如果 CMA 用完了，怎么紧急回收？

### 11. 补充学习
- 内核 `Documentation/admin-guide/kernel-parameters.txt` 中的 cma= 参数
- ARMv7 cache 结构 (PIPT/VIPT)
- DMA-API: `dma_alloc_coherent` vs `dma_map_single`
- Linux IOMMU 和 DMA mapping

---

## K4: NPU Arena 内存复用

### 1. 一句话概括
多个 RKNN 模型注册到同一个 Arena，共享一组 input/output DMA buffer，通过 bind-before-destroy 策略安全切换，节省 29% DMA 内存。

### 2. 为什么需要它
YOLO+Face+KWS 三模型如果各自分配独立 DMA buffer，需要 ~4.8MB。Arena 共享只需 ~3.4MB。在 256MB 的 RV1106 上，省出的 1.4MB 就是生死线。

### 3. Linux 底层原理
```
RKNN 驱动:
  rknn_create_mem(ctx, size) → 内部 dma_buf_alloc → rknn_tensor_mem*
  rknn_set_io_mem(ctx, mem, attrs) → ioctl 绑定 → NPU 知道这个 buffer 的物理地址
  rknn_destroy_mem → dma_buf_free → NPU 解绑 (内部 kernel ioctl)
  
NPU 每次 rknn_run 需要知道 input/output tensor 的物理地址
Arena 复用: 切换 ctx 时调 rknn_set_io_mem 替换物理地址映射
```

### 4. 核心数据结构/关键 API
```c
struct npu_arena_t {
    ArenaState state;                      // EMPTY→REGISTERED→ALLOCATED→DESTROYED
    ModelSlot slots[4];                    // 最多 4 个模型
    size_t max_input_size;                 // 所有模型 input 的最大值
    size_t max_output_sizes[3];            // 所有模型各 output 的最大值
    rknn_tensor_mem *input_mem;            // 共享 input DMA buffer
    rknn_tensor_mem *output_mems[3];       // 共享 output DMA buffer
    rknn_context bound_ctx;                // 当前绑定到哪个模型
    pthread_mutex_t run_mutex;             // 跨线程保护
};

// 生命周期 API
npu_arena_create() → register(YOLO) → register(Face) → allocate → 
  adopt(YOLO) → adopt(Face) → bind(需用时) → destroy
```

### 5. 完整调用流程
```
init:
  arena = npu_arena_create()                                // EMPTY
  npu_arena_register(arena, yolo_ctx, ...)                  // 记录 YOLO tensor 尺寸
  npu_arena_register(arena, face_ctx, ...)                  // 记录 Face tensor 尺寸
  npu_arena_allocate(arena, primary_ctx=yolo)               // 按 max 尺寸分配 DMA
  npu_arena_adopt_yolov5(arena, &yolo_app_ctx)              // bind + swap mem 指针
  npu_arena_adopt_yolov5(arena, &face_app_ctx)              // 同上

每帧:
  lock → arena_bind_yolov5 → rknn_run(YOLO) → unlock
  lock → arena_bind_yolov5 → rknn_run(Face)  → unlock

KWS (IdleState):
  lock → arena_bind_kws → rknn_run(KWS) → unlock

destroy:
  NULL→adopted mem 指针 → rknn_destroy_mem(arena buffers) → free(arena)
```

### 6. 我的项目中使用
- [npu_memory_reuse.cc](D:\echo\yolov5_demo\cpp\npu_memory_reuse.cc) — 完整实现
- [AIcamera_c_interface.cc:728-746](D:\echo\yolov5_demo\cpp\AIcamera_c_interface.cc#L728) — Arena 初始化
- [Idle.cc:43-51](D:\echo\AIChat_demo\Client\Application\UserStates\Idle.cc#L43) — KWS 延迟注册

### 7. 涉及源码
| 文件 | 关键函数 |
|------|---------|
| `npu_memory_reuse.h` | API + ArenaState 枚举 |
| `npu_memory_reuse.cc` | 全部实现，含注释解释 bind-before-destroy 原理 |
| 内核: 无（纯用户态实现，依赖 RKNN runtime） | |

### 8. 常见 Bug 及调试方法
| Bug | 原因 | 调试 |
|-----|------|------|
| rknn_set_io_mem 返回 -1 | output size 不匹配 | 打印 model vs arena max sizes |
| NPU 推理结果全 0 | 忘记 bind 就 run，读到了上一个模型的 buffer | 加 assert(bound_ctx==ctx) |
| 板端 kernel Oops | destroy 时没 unbind，内核 NULL deref | 遵循 bind-before-destroy |
| KWS 注册失败 | late register 时 buffer size 不够 | 检查 register 顺序 |
| 死锁 | lock 后 return 忘了 unlock | 用 RAII LockGuard |

### 9. 性能优化点
- `arena_bind_locked` 中 skip 已绑定的 ctx（no-op），省 200-500μs
- 注册顺序把最大模型放第一个，避免 late-register 超限

### 10. 面试高频问题
1. Arena 为什么能省内存？
2. bind-before-destroy 是什么？为什么需要？
3. late register 的风险？什么情况下会失败？
4. 多线程下 Arena 怎么保护？mutex 粒度多大？
5. ALLOCATED vs REGISTERED 状态的区别？
6. primary_ctx 的作用？
7. 如果三个模型中有两个 input 尺寸不同，Arena 怎么选大小？
8. adopt 和 bind 的区别？
9. 如何验证 Arena 确实工作在板端？
10. 内存节省比例怎么计算的？

### 11. 补充学习
- RKNN API 1.7.x 文档中的 `rknn_set_io_mem`
- Linux kernel `rknpu_drv.c` 中 mem sync ioctl 的处理
- 其他嵌入式 NPU 内存复用方案（TFLite Micro, HIMAX）

---

## K5: MPP 硬件 JPEG 编码

### 1. 一句话概括
调用 RV1106 的 MPP (Media Process Platform) MJPEG 硬件编码器，将 NV12 dma-buf fd 编码为 JPEG bitstream，零 CPU 拷贝。

### 2. 为什么需要它
替代 `cv::imencode` (CPU JPEG, ~5ms/帧)，MPP 硬件编码 < 2ms。配合 RGA fd→fd 的 NV12 输入，端到端零 CPU 像素拷贝。

### 3. Linux 底层原理
```
MPP 框架:
  mpp_create → mpp_init(MPP_CTX_ENC, MJPEG) → 打开 /dev/mpp_service
  mpp_buffer_import(EXT_DMA, fd) → 将 dma-buf 导入 MPP 的 buffer 池
  encode(frame, &packet) → ioctl → 内核调度 VE (Video Encoder) 硬件
                         → JPEG 编码完成 → MppPacket (含 bitstream)
```
MPP 是 Rockchip 对芯片内 VPU/VE 硬件的抽象层，统一接口，底层走 V4L2 mem2mem 或直接 ioctl。

### 4. 核心数据结构/关键 API
```c
// 初始化
MppCtx ctx;  MppApi *mpi;
mpp_create(&ctx, &mpi);
mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);

// 设置参数
MppEncCfg cfg;
mpp_enc_cfg_init(&cfg);
mpp_enc_cfg_set_s32(cfg, "prep:width", 448);
mpp_enc_cfg_set_s32(cfg, "prep:height", 448);
mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
mpi->control(ctx, MPP_ENC_SET_CFG, cfg);

// 编码
MPP_RET ret = mpi->encode(ctx, frame, &packet);
void *data = mpp_packet_get_data(packet);
size_t len = mpp_packet_get_length(packet);
// data 就是 JPEG bitstream，直接 base64_encode → 发送
```

### 5. 完整调用流程
```
hw_jpeg_encoder_init(640, 640, 448, 448, 85)
  → dma_buf_alloc(302KB NV12 中间 buffer)
  → mpp_create + mpp_init(MJPEG)
  → mpp_enc_cfg_init + 设置 prep:width/height/format + rc:quality
  → mpi->control(MPP_ENC_SET_CFG)

hw_jpeg_encode(src_fd, &jpeg_data, &jpeg_size)
  → RGA BGR(fd)→NV12(g_nv12_fd)             // 格式转换+缩放
  → build_frame_from_dma(g_nv12_fd)           // import 到 MPP buffer
  → mpi->encode(frame, &packet)               // sync 编码
    失败 → encode_put_frame + poll + encode_get_packet  // async fallback
  → 检查 SOI 头 (0xFF 0xD8)
  → copy bitstream → *jpeg_data, *jpeg_size
```

### 6. 我的项目中使用
- [hw_jpeg_encoder.h](D:\echo\yolov5_demo\cpp\hw_jpeg_encoder.h) — API
- [hw_jpeg_encoder.cc](D:\echo\yolov5_demo\cpp\hw_jpeg_encoder.cc) — 实现
- [Application.cc:334-343](D:\echo\AIChat_demo\Client\Application\Application.cc#L334) — DescribeCurrentScene 调用 HW 路径

### 7. 涉及源码
| 文件 | 关键函数 |
|------|---------|
| `hw_jpeg_encoder.cc` | `init`, `encode`, `deinit`, `build_frame_from_dma` |
| MPP SDK: `include/rockchip/rk_mpi.h` | `mpp_create`, `mpp_init`, `MppApi` |
| MPP SDK: `include/rockchip/mpp_frame.h` | `mpp_frame_init`, `mpp_frame_get_buffer` |
| 内核: `drivers/video/rockchip/mpp/` | MPP service 驱动 |

### 8. 常见 Bug 及调试方法
| Bug | 原因 | 调试 |
|-----|------|------|
| `mpp_init` 返回非 0 | MJPEG 硬件不支持或驱动未加载 | `lsmod \| grep mpp` |
| SOI header 丢失 (0xFF 0xD8) | MPP 版本首帧不输出头 | 加 `jpeg:header_mode=1` config |
| `mpp_buffer_import` 失败 | EXT_DMA 的 fd 无效 | 确认 CMA 已分配 |
| encode 返回 0 但 packet NULL | 同步 encode 内部 V1 路径 | 自动 fallback async |
| 编译错误 `MPP_RET` 是 enum 不能用 int | C++ 类型检查 | 用 `MPP_OK` 而非 `0` |

### 9. 性能优化点
- 首选 sync `encode()` 路径，少一次 put/get 的 ioctl
- NV12 buffer 一次性分配，每帧复用
- quality=85 是速度和质量的平衡点

### 10. 面试高频问题
1. MPP encode vs CPU cv::imencode 的性能对比？
2. 为什么 MPP 要吃 NV12 而不是 BGR？
3. encode vs encode_put_frame+encode_get_packet 的区别？
4. EXT_DMA 导入是什么？为什么是零拷贝？
5. SOI marker 丢失怎么办？
6. MPP_ENC_SET_CFG 配置了哪些参数？
7. MJPEG vs H.264 编码：为什么选 MJPEG？
8. MPP 编码时 `g_mutex` 锁的作用？

### 11. 补充学习
- MPP 开发文档 `doc/MPP_Development_Manual.pdf`
- JPEG 文件格式 (SOI/APP0/DQT/SOF/SOS/EOI)
- MJPEG vs H.264 编码延迟对比

---

## 调用方式

用户说 "K1" 或 "V4L2" 或 "零拷贝采集" → 输出 K1 卡片
用户说 "全部" → 输出所有卡片
用户说 "K1 K3 K6" → 输出指定卡片
