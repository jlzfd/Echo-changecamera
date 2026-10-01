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

## 12. 常见错误

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

## 13. 调试方法

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

## 14. 项目源码索引

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



