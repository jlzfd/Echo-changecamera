# Echo Mate DMA-BUF、Cache 与跨硬件同步指南

## 1. 先给结论

DMA-BUF解决“共享哪块内存”，不单独解决“什么时候可以访问”。完整正确性由三层共同保证：

```text
对象共享：DMA-BUF fd、attach、SG table、IOMMU映射
数据可见：DMA API、CPU Cache clean/invalidate
执行顺序：同步调用、Buffer所有权、Fence、完成回调或锁
```

当前项目应用层没有把显式`dma_fence`串进Camera→RGA→NPU链路，主要使用同步调用和状态所有权。

## 2. fd不是物理地址

用户态fd的查找关系：

```text
fd
  → 进程fdtable
  → struct file
  → struct dma_buf
  → exporter管理的底层内存
```

fd具有以下性质：

- 只是当前进程中的整数句柄；
- 不是物理地址或CMA下标；
- 同一个dma-buf传到另一个进程后，fd数值可以不同；
- `close(fd)`只减少引用，所有引用释放后内存才真正回收。

## 3. Exporter与Importer

### Exporter

分配或拥有内存的一方负责导出：

```text
分配物理页/CMA内存
  → 建立SG table
  → dma_buf_export()
  → dma_buf_fd()
  → 返回用户态fd
```

当前项目例子：

- V4L2/VB2通过`VIDIOC_EXPBUF`导出ISP采集Buffer；
- LCD专用驱动导出自身双缓冲；
- 用户态DMA allocator通过CMA heap取得fd。

### Importer

RGA、NPU或其他设备驱动接收fd后：

```c
dmabuf = dma_buf_get(fd);
attach = dma_buf_attach(dmabuf, dev);
sgt = dma_buf_map_attachment(attach, direction);
```

结束时反向释放：

```c
dma_buf_unmap_attachment(attach, sgt, direction);
dma_buf_detach(dmabuf, attach);
dma_buf_put(dmabuf);
```

## 4. 为什么同一Buffer在不同设备上地址不同

```text
同一组物理页
   ├─ RGA IOMMU映射 → RGA IOVA 0x10000000
   ├─ NPU IOMMU映射 → NPU IOVA 0x80000000
   └─ CPU mmap      → 用户虚拟地址 0xb6xxxxxx
```

DMA-BUF共享的是内存对象，不要求每个设备看到相同数值的地址。`sg_table`描述底层页段，DMA/IOMMU层为每个设备建立可访问映射。

底层内存可能来自CMA连续区域，也可能由多个离散页组成；是否必须连续由硬件能力和IOMMU决定。

## 5. 一致性的两个问题

### 5.1 Cache一致性

设备DMA写完后，CPU Cache可能仍有旧副本；CPU写完后，脏Cache也可能尚未到达设备可见内存。

```text
设备写 → CPU读：invalidate/同步给CPU
CPU写 → 设备读：clean/同步给设备
```

内核驱动常使用：

```c
dma_sync_sg_for_cpu();
dma_sync_sg_for_device();
```

用户态mmap访问DMA-BUF时，项目包装了：

```c
DMA_BUF_IOCTL_SYNC + DMA_BUF_SYNC_START
DMA_BUF_IOCTL_SYNC + DMA_BUF_SYNC_END
```

对应：

```cpp
dma_sync_device_to_cpu(fd);
dma_sync_cpu_to_device(fd);
```

这里的函数名是项目封装名；真正语义是声明CPU访问区间。`DMA_BUF_IOCTL_SYNC`主要面向CPU访问，不是通用设备到设备Fence。

### 5.2 时序一致性

即使Cache完全正确，两个设备同时读写仍会出错：

```text
Camera正在写帧
RGA同时读取
  → 可能得到撕裂帧
```

必须通过同步返回、所有权、完成事件或Fence保证访问先后。

## 6. Fence模型

`dma_fence`表示一个异步硬件任务的完成点，`sync_file`可以把Fence包装成用户态fd。DMA-BUF通常借助`dma_resv`关联访问Fence。

```text
Camera写Buffer
  → 产生Fence F1
  → RGA读前等待F1
  → RGA写输出并产生F2
  → NPU读前等待F2
```

Fence回答：“前一个硬件完成了吗？”

Cache同步回答：“完成后写入的数据对当前访问者可见吗？”

两者不能互相替代。

## 7. 当前Camera→RGA同步

项目流程：

```text
VIDIOC_DQBUF
  → 应用获得V4L2 Buffer所有权
  → 把导出的ISP fd交给RGA
  → 同步convert_image()/improcess()
  → RGA返回后不再访问输入Buffer
  → VIDIOC_QBUF归还Camera
```

`DQBUF/QBUF`构成所有权边界：

- DQBUF前，Buffer归摄像头驱动；
- DQBUF后到QBUF前，归应用/RGA链路；
- QBUF后，应用不能继续访问，Camera可再次写入。

如果以后改为异步RGA并在提交后立即QBUF，Camera可能在RGA读取期间覆盖该Buffer，这时必须等待Fence或完成事件。

## 8. 当前RGA→NPU同步

项目使用独立staging Buffer和单槽任务状态：

```text
检查worker.busy=false
  → 设置busy=true
  → RGA同步写g_npu_stage_rgb
  → 设置job_pending并唤醒NPU线程
  → NPU线程读取staging
  → RGA同步写Arena输入
  → rknn_run同步推理
  → busy=false
```

因此NPU处理期间，采集线程不会覆盖staging Buffer。NPU忙时，新推理帧被跳过，避免形成无界队列和高延迟。

YOLO、Face、KWS共享Arena时，还通过`npu_arena_lock()`把以下操作串行化：

```text
写Arena输入 → 绑定模型IO → rknn_run → 消费结果
```

这属于应用层所有权同步，不是Fence。

## 9. 当前RGA→LCD→SPI同步

```text
ACQUIRE取得FREE LCD Buffer
  → 状态变ACQUIRED
  → RGA同步写RGB565
  → QUEUE，in_fence_fd=-1
  → 驱动状态变QUEUED/IN_FLIGHT
  → spi_async提交
  → SPI完成回调
  → Buffer恢复FREE
```

双缓冲和驱动状态机防止SPI还在读取时RGA重新覆盖。没有空闲Buffer时应用得到`EAGAIN`并丢弃显示帧，保持低延迟。

接口虽然预留`in_fence_fd`，当前应用传`-1`，因为RGA调用返回时已经完成。显式Fence是未来异步RGA接入点。

## 10. 当前同步机制汇总

| 链路 | 时序保证 | 数据可见性 |
|---|---|---|
| ISP → 应用 | V4L2 DQBUF/QBUF | V4L2/VB2 DMA映射规则 |
| Camera fd → RGA | 同步`improcess()` | Importer DMA映射 |
| RGA → CPU画框 | RGA同步返回 | `DMA_BUF_IOCTL_SYNC START` |
| CPU画框 → RGA/LCD | CPU操作完成 | `DMA_BUF_IOCTL_SYNC END` |
| RGA staging → NPU | `busy/job_pending`、同步RGA | 运行时/DMA映射，项目另有sync包装 |
| 多模型共享Arena | `npu_arena_lock()` | RKNN内存映射 |
| RGA → LCD | 同步RGA后QUEUE | DMA-BUF映射 |
| LCD → SPI完成 | Buffer状态机、完成回调 | 驱动DMA API |

## 11. 什么时候应该引入显式Fence

出现以下设计时应考虑Fence：

- RGA接口改成提交后立即返回；
- Camera Buffer需要在RGA完成前提前QBUF；
- 同一个输出Buffer跨多个异步设备传递；
- 希望Camera、RGA、NPU、Display形成深度流水；
- 驱动和应用之间需要传递硬件完成依赖，而不是阻塞线程等待。

不应为了“用了高级机制”而引入Fence。当前单路、latest-only、低并发链路中，同步调用与严格所有权更简单，也更容易验证。

## 12. 内存对齐与stride

DMA场景里的“对齐”不是一个概念，而是至少包含四层：

| 层次 | 含义 | 不满足时的后果 |
|---|---|---|
| 地址对齐 | Buffer起始DMA地址满足硬件要求 | DMA拒绝、性能下降或总线异常 |
| 分配粒度 | 页/CMA/IOMMU映射通常按页管理 | 映射范围扩大，尾部存在padding |
| 行对齐 | 每行实际跨度`stride`可能大于有效宽度 | 图像倾斜、错色、越界 |
| Tensor对齐 | RKNN真实内存按`size_with_stride`分配 | NPU输出截断或写越界 |

### 12.1 width、stride和size不是一回事

以RGB888为例：

```text
有效行字节 = width × 3
实际行字节 = width_stride × 3
Buffer大小至少 = 实际行字节 × height_stride
```

如果硬件要求每行16字节对齐，可表示为：

```c
stride_bytes = ALIGN(width * bytes_per_pixel, 16);
```

CPU逐行访问时必须使用真实stride：

```c
row = base + y * stride_bytes;
pixel = row + x * bytes_per_pixel;
```

不能默认使用`width * bytes_per_pixel`跨行，否则只要出现padding，第二行开始就会错位。

### 12.2 常见图像格式大小

在无额外padding时：

```text
NV12    = width × height × 3 / 2
RGB888  = width × height × 3
RGB565  = width × height × 2
```

但驱动返回的`bytesperline`、`sizeimage`、LCD UAPI中的`stride`以及RKNN的`size_with_stride`优先级更高。真实分配不能只按理论有效像素计算。

项目V4L2初始化会记录驱动协商后的stride和size；RGA结构则显式保存`width_stride/height_stride`。LCD侧由驱动返回字节stride，RGB565转换时应用使用`info.stride / 2`得到像素stride。

### 12.3 RKNN必须使用size_with_stride

项目NPU Arena不是只使用tensor逻辑尺寸，而是比较和分配：

```cpp
input_attrs[i].size_with_stride
output_attrs[i].size_with_stride
```

原因是NPU内部可能对W、H或C进行硬件对齐。逻辑元素数量计算出的size可能小于真实写入范围。正确原则是：

```text
分配大小 >= size_with_stride
绑定属性与实际模型tensor一致
模型切换前重新校验每个输入输出上限
```

项目通过Arena记录所有模型的最大输入和各输出最大值，再预分配共享内存。这既减少反复申请释放，也避免较大模型写穿较小Buffer。

### 12.4 对齐不等于物理连续

页对齐、Cache line对齐和物理连续是不同属性：

- CMA通常可提供物理连续内存；
- SG Buffer可以由多个物理段组成；
- IOMMU可以把离散物理页映射成连续IOVA；
- 起始地址对齐并不能证明整个Buffer物理连续；
- fd更不能反推出连续物理地址。

## 13. DMA场景下的线程安全

线程安全要同时保护三类对象：

```text
CPU状态：index、busy、job_pending、引用计数
Buffer内容：谁正在读、谁正在写
硬件上下文：RGA/NPU绑定、提交队列、模型Arena
```

### 13.1 锁只保护CPU临界区

例如：

```cpp
pthread_mutex_lock(&worker.job_mutex);
if (!worker.busy) {
    worker.busy = true;
    submit = true;
}
pthread_mutex_unlock(&worker.job_mutex);
```

它保证两个CPU线程不会同时认领staging Buffer，但它本身不能证明RGA或NPU已经完成DMA。硬件完成仍由同步API返回、IRQ完成事件或Fence保证。

### 13.2 当前项目的锁职责

| 锁/状态 | 保护对象 | 不能替代什么 |
|---|---|---|
| `job_mutex + busy/job_pending` | staging任务的生产消费状态 | RGA/NPU硬件完成 |
| `result_mutex` | 检测结果结构的跨线程复制 | DMA Cache同步 |
| `npu_arena_lock()` | Arena写入、IO绑定和推理的原子序列 | 多Buffer流水调度 |
| `pic_buf_mutex` | 图片Buffer的应用层访问 | V4L2 QBUF/DQBUF所有权 |
| LCD Buffer状态机 | FREE到IN_FLIGHT生命周期 | CPU Cache维护 |
| `running_mutex` | 相机启动停止和全局资源生命周期 | 单帧DMA完成 |

### 13.3 正确的所有权状态机

推荐每个共享Buffer都有明确状态：

```text
FREE
  → PRODUCER_WRITING
  → READY
  → CONSUMER_READING
  → FREE
```

如果是异步硬件，还需要把完成条件放入状态转换：

```text
RGA_SUBMITTED
  → RGA fence/complete
  → NPU_READY
```

禁止仅因为“已经调用提交函数”就把Buffer标成FREE。提交成功只说明任务进入队列，不一定说明硬件完成。

### 13.4 互斥锁、原子变量和内存屏障

- 互斥锁适合保护多个相关字段和复合状态转换；
- 原子变量适合单个停止标志或计数器；
- 条件变量必须和谓词一起在循环中检查，防止虚假唤醒；
- CPU内存屏障只约束CPU访存顺序，不能代替DMA API的设备同步；
- 驱动中访问MMIO和DMA描述符时应使用内核提供的`readl/writel`、DMA API和相应barrier，不能只靠C/C++ `volatile`。

## 14. 一致性的完整闭环

一次正确的Buffer交接应同时回答四个问题：

```text
1. 所有权：当前谁可以访问？
2. 完成性：前一个CPU/硬件任务结束了吗？
3. 可见性：新数据对下一个访问者可见吗？
4. 布局：双方对format、stride、offset、size理解一致吗？
```

以RGA写、CPU画框、LCD读取为例：

```text
RGA同步返回                     // 完成性
  → DMA_BUF_SYNC_START          // 对CPU可见
  → CPU按真实stride画框          // 布局正确
  → DMA_BUF_SYNC_END            // 对设备可见
  → ACQUIRE/QUEUE状态切换        // 所有权
  → SPI完成回调后恢复FREE        // 生命周期
```

少任何一环都可能表现为偶现问题：

- 缺所有权：并发覆盖、撕裂；
- 缺完成等待：读取半帧；
- 缺Cache同步：读到旧数据；
- stride/size错误：花屏、越界；
- 生命周期错误：use-after-free或fd泄漏。

## 15. DMA内存生命周期和错误路径

建议把生命周期成对检查：

```text
alloc       ↔ free
dma_buf_get ↔ dma_buf_put
attach      ↔ detach
map         ↔ unmap
mmap        ↔ munmap
DQBUF       ↔ QBUF
ACQUIRE     ↔ QUEUE或CANCEL
lock        ↔ unlock
```

当前项目尤其要保证：

- RGA失败时仍归还V4L2 Buffer；
- ACQUIRE后转换失败必须CANCEL LCD Buffer；
- NPU停止前先停止新任务并join工作线程；
- 硬件仍在使用时不能close最后一个fd或释放Arena；
- 部分初始化失败时按逆序释放已成功资源。

## 16. 项目中的DMA数据链总图

```text
Sensor/ISP
  → VB2采集Buffer
  → VIDIOC_EXPBUF得到dma-buf fd
  → DQBUF取得所有权
  → RGA导入fd并读取NV12
  → RGA写CMA RGB Buffer
       ├→ CPU Cache同步后画框
       ├→ staging Buffer → NPU Arena → RKNN
       └→ RGA RGB565 → LCD双缓冲 → SPI DMA
  → 各消费者完成
  → QBUF归还Camera Buffer
```


## 17. 常见错误

### 把fd当物理地址

错误。fd只能在内核中查找到`dma_buf`，硬件使用的是驱动映射出的DMA地址或IOVA。

### 只刷Cache，不等待硬件

错误。Cache同步不能证明DMA任务已经结束。

### 只等待Fence，不处理CPU Cache

错误。Fence只说明任务完成，不一定完成非一致性平台上的CPU Cache维护。

### 异步提交后立即复用Buffer

这是典型竞态。必须等完成回调/Fence，或切换到另一个FREE Buffer。

### close(fd)后认为硬件一定停止

错误。驱动可能仍持有attachment或任务引用。必须先完成/取消任务，再释放生命周期引用。

## 18. 调试方法

优先记录每个Buffer的：

```text
index/fd
当前owner和state
提交时间
硬件完成时间
DQBUF/QBUF序号
frame_id
```

异常判断：

| 现象 | 可能原因 |
|---|---|
| 偶发花屏/撕裂 | Buffer过早复用、异步任务未完成 |
| CPU偶尔读到旧图 | 缺少CPU access Cache同步 |
| NPU结果错帧 | staging/Arena被并发覆盖或模型绑定竞态 |
| LCD延迟持续增大 | 排队旧帧而非latest-only |
| fd持续增加 | `dma_buf_get/put`或导出fd生命周期泄漏 |

## 19. 项目源码索引

```text
V4L2所有权：
  yolov5_demo/cpp/v4l2_capture.cc

Camera/RGA/NPU/LCD主链：
  yolov5_demo/cpp/AIcamera_c_interface.cc

RGA同步调用：
  yolov5_demo/cpp/utils/image_utils.c

DMA-BUF用户态同步封装：
  yolov5_demo/cpp/3rdparty/allocator/dma/dma_alloc.cpp

NPU Arena：
  yolov5_demo/cpp/npu_memory_reuse.cc
  yolov5_demo/cpp/npu_memory_reuse.h

SPI显示说明：
  AIChat_demo/docs/SPI_ST7789V_DRIVER_GUIDE.md
```



