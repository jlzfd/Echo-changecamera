# ST7789V + RGA 零拷贝显示驱动开发方案

状态：P0-D4已实现并完成板端验证；等待D4评审后进入AIcamera应用接入  
适用平台：RV1106 / Rockchip SPI / ST7789V / RGA / Linux FBTFT  
方案版本：v1.0

## 1. 目标和边界

### 1.1 目标

把当前显示链路：

```text
Arena RGB888
  → RGA 转换到独立 g_lcd_fd CMA buffer
  → CPU 将整帧 memcpy 到 /dev/fb0
  → FBTFT 用 CPU 做 RGB565 字节交换并复制到 txbuf
  → SPI 控制器发送
```

改造为：

```text
Arena RGB888 DMA-BUF
  → RGA 直接写显示驱动共享的 RGB565 DMA-BUF
  → 显示驱动等待 RGA 完成
  → SPI DMA 直接读取同一缓冲并发送给 ST7789V
```

“零拷贝”在本方案中的准确含义：

- CPU 不再执行 LCD 整帧 `memcpy`；
- CPU 不再逐像素进行 RGB565 字节交换；
- RGA 输出缓冲与 SPI DMA 输入缓冲是同一份图像存储；
- SPI 将图像传入 ST7789V 内部 GRAM 的硬件传输不可省略，不计作 CPU 拷贝；
- CPU仍负责提交任务、命令控制、状态切换和异常处理。

### 1.2 非目标

- 第一版不改成完整 DRM/KMS 驱动；
- 第一版不追求异步 RGA Fence，先使用同步 RGA 返回作为完成边界；
- 不改变摄像头、NPU Arena 和图传双缓冲架构；
- 不保证第一版同时支持多个显示生产者；
- 不将裸物理地址暴露给用户态。

## 2. 当前代码事实

1. 当前 ST7789V 通过 `sitronix,st7789v` 匹配 FBTFT，并注册 `/dev/fb0`。
2. FBTFT 的 framebuffer 由 `vzalloc()`分配，与项目通过 CMA Heap 分配的 `g_lcd_fd`不是同一块内存。
3. 当前 RGA 将 Arena RGB888 转换为 RGB565，写入 `g_lcd_fd`。
4. `get_buf_data()`通过 CPU `memcpy()`把 `g_lcd_fd`内容复制到 framebuffer。
5. `fbtft_write_vmem16_bus8()`又通过 `cpu_to_be16()`复制到临时 `txbuf`。
6. Rockchip SPI控制器支持8-bit和16-bit word，并支持DMA Engine发送。
7. Rockchip SPI单个 `spi_transfer`最大长度为`0xffff`字节。
8. 320×240 RGB565一帧为153600字节，必须拆成至少3个transfer。
9. SPI Core能够将普通内存或vmalloc内存转换为SG列表并执行DMA映射。

## 3. 总体架构

采用“专用SPI显示驱动 + 双DMA-BUF + 异步SPI”的架构。

```text
                    ┌──────────────────────┐
Arena RGB888 fd ───►│ RGA                  │
                    │ resize + RGB565      │
                    └──────────┬───────────┘
                               │ 写
                  ┌────────────▼────────────┐
                  │ LCD共享双缓冲           │
                  │ buffer[0] / buffer[1]   │
                  │ DMA-BUF fd              │
                  └────────────┬────────────┘
                               │ QUEUE_BUFFER
                    ┌──────────▼───────────┐
                    │ st7789v_rga驱动      │
                    │ 状态机/丢帧/同步     │
                    └──────────┬───────────┘
                               │ spi_async
                    ┌──────────▼───────────┐
                    │ Rockchip SPI DMA     │
                    └──────────┬───────────┘
                               ▼
                         ST7789V GRAM
```

### 3.1 为什么使用专用驱动

不直接修改通用`fbtft-core`，原因如下：

- 通用FBTFT默认假设CPU写Framebuffer；
- deferred I/O依赖CPU脏页，不适合RGA DMA写入；
- 默认`write_vmem16_bus8()`包含CPU字节交换和临时缓冲复制；
- 将DMA-BUF、Fence和双缓冲逻辑塞入通用框架容易影响其他屏幕驱动；
- 专用驱动可复用ST7789V初始化命令，但替换显存和刷新路径。

建议新增：

```text
drivers/staging/fbtft/fb_st7789v_rga.c
include/uapi/linux/st7789v_rga.h
```

也可放到`drivers/video/fbdev/`，但第一版放在现有FBTFT附近更便于复用和构建。

## 4. 驱动注册方案

### 4.1 设备树

将屏幕节点的兼容字符串改为项目专用值，避免原`fb_st7789v`抢占：

```dts
&spi0 {
    status = "okay";

    display@0 {
        compatible = "echo,st7789v-rga";
        reg = <0>;
        spi-max-frequency = <60000000>;

        width = <240>;
        height = <320>;
        rotation = <270>;
        buffer-count = <2>;

        dc-gpios = <&gpio1 RK_PD0 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&gpio1 RK_PC4 GPIO_ACTIVE_LOW>;
        status = "okay";
    };
};
```

### 4.2 SPI驱动注册

```text
module_spi_driver(st7789v_rga_driver)
  → spi_register_driver()
  → SPI Core匹配 compatible
  → st7789v_rga_probe()
```

`probe()`负责：

1. 解析DTS；
2. 获取DC、RESET、背光资源；
3. 配置SPI mode和频率；
4. 初始化锁、等待队列和工作队列；
5. 分配两块显示缓冲；
6. 建立DMA-BUF共享能力；
7. 初始化ST7789V；
8. 注册`/dev/st7789v-rga`；
9. 可选注册兼容`/dev/fb0`；
10. 开启背光。

普通SPI驱动使用`module_spi_driver()`即可，不需要额外使用`subsys_initcall()`。

## 5. 用户态接口

驱动新增字符设备：

```text
/dev/st7789v-rga
```

### 5.1 UAPI

| 接口 | 作用 |
|---|---|
| `GET_INFO` | 获取宽高、stride、格式、缓冲数量 |
| `GET_BUFFER` | 获取指定显示缓冲对应的DMA-BUF fd |
| `ACQUIRE_BUFFER` | 获取一块可由RGA写入的空闲缓冲 |
| `QUEUE_BUFFER` | RGA完成后提交显示缓冲 |
| `CANCEL_BUFFER` | RGA失败时归还缓冲 |
| `GET_STATS` | 获取显示、丢帧、超时和错误计数 |

`QUEUE_BUFFER`预留Fence字段：

```c
struct st7789_queue {
    __u32 index;
    __s32 in_fence_fd; /* 第一版固定为-1 */
    __u64 frame_id;
    __u32 flags;
};
```

### 5.2 poll与epoll

驱动实现文件操作`.poll`：

- 有FREE缓冲或可替换的READY缓冲：返回`EPOLLOUT | EPOLLWRNORM`；
- 有完成事件：返回`EPOLLIN | EPOLLRDNORM`；
- 发生不可恢复错误：返回`EPOLLERR`；
- 驱动移除或停止：返回`EPOLLHUP`。

用户态既可以使用`poll()`，也可以把设备fd加入`epoll()`；epoll底层同样调用驱动`.poll`。项目后续若统一监听WebSocket、音频、timerfd和LCD，建议用户态使用epoll。

## 6. 缓冲区状态机

每块缓冲具有以下状态：

```text
FREE
  └─ ACQUIRE ─► RGA_OWNED
                    ├─ CANCEL ─► FREE
                    └─ QUEUE ──► READY
                                    ├─ 新ACQUIRE替换 ─► RGA_OWNED
                                    └─ SPI调度 ──────► SPI_OWNED
                                                       └─ 完成 ─► FREE
```

约束：

- `RGA_OWNED`只能由当前用户提交或取消；
- `SPI_OWNED`绝不能被RGA覆盖；
- `ACQUIRE`优先选择FREE；没有FREE时才原子抢占READY，以便生产者写入更新帧；
- 状态切换在`state_lock`保护下进行；
- SPI完成回调仅修改状态、统计并唤醒等待者；
- 睡眠操作不得在spinlock或SPI完成回调中执行。

## 7. 显示队列和延迟策略

采用“正在发送1帧 + 等待发送最多1帧”的策略：

```text
SPI正在发送A
READY中已有B
RGA又完成C
  → 丢弃B
  → 保留C
```

规则：

- 不排队历史画面；
- 永远优先显示最新完成帧；
- 被替换的READY缓冲直接转为`RGA_OWNED`，供RGA写入更新帧；
- 每次替换记录`dropped_ready`；
- 只有两块缓冲均为`SPI_OWNED`/`RGA_OWNED`、不存在FREE或READY时，非阻塞ACQUIRE才返回`-EAGAIN`，LCD分支跳过该帧而不阻塞NPU主链路。

## 8. 同步与DMA一致性

一致性拆成三个问题：完成顺序、所有权、Cache可见性。三者必须同时满足。

### 8.1 CPU绘制Arena → RGA读取

当前CPU会在Arena上绘制检测框，因此保留：

```text
CPU绘制Arena
  → dma_sync_cpu_to_device(arena_fd)
  → RGA读取
```

若以后改为硬件绘框，再重新评估此同步。

### 8.2 RGA写LCD Buffer → SPI读取

第一版继续使用同步`convert_image()`：

```text
ACQUIRE
  → convert_image()
  → 确认函数返回代表RGA硬件完成
  → QUEUE_BUFFER
  → SPI启动
```

必须通过RGA实现或运行测试确认`convert_image()`不是“仅提交即返回”。若不能确认，则Fence成为第一版必选项，而不是后续优化。

异步版本：

```text
RGA提交并返回out_fence_fd
  → QUEUE_BUFFER携带Fence
  → 驱动在工作队列等待或注册dma_fence回调
  → Fence完成后才转READY并启动SPI
```

### 8.3 SPI读取完成 → RGA重新写

`spi_async()`返回只代表提交成功。只有SPI message完成回调触发后，缓冲才能：

```text
SPI_OWNED → FREE
```

完成前禁止RGA复用。

### 8.4 Cache同步原则

最终LCD缓冲没有CPU像素访问，因此移除LCD分支的：

```text
dma_sync_device_to_cpu(g_lcd_fd)
```

驱动通过标准DMA映射规则保证SPI可见性：

- RGA和SPI分别拥有自己的DMA映射；
- 不跨设备复用裸`dma_addr_t`；
- 首选让SPI Core对`tx_buf`构造`tx_sg`并执行DMA map/unmap；
- 如果采用长期映射，则在所有权切换时使用与映射方式匹配的`dma_sync_sg_for_device()`；
- 标准映射和预映射只能选一种，禁止重复map；
- `wmb()/rmb()`不能代替RGA完成Fence或DMA Cache同步。

### 8.5 DMA-BUF生命周期

- 驱动持有底层缓冲主引用；
- 导出fd关闭只减少用户态引用，不能在SPI使用期间释放底层内存；
- RGA attachment必须按设备建立和解除；
- 用户进程退出时回收其`RGA_OWNED`和`READY`缓冲；
- `SPI_OWNED`必须等完成或终止DMA后才能释放；
- Remove和Suspend先停止新提交，再排空/终止SPI，最后释放缓冲。

## 9. DMA内存方案

最终实现前做一个受控技术验证，在以下两条路径中选择一条：

### 方案A：驱动分配并导出DMA-BUF（目标方案）

优点：

- 显示驱动拥有生命周期和所有权；
- 双缓冲状态最清晰；
- 用户态不能错误释放底层显存；
- 便于Suspend、Remove和异常恢复。

要求：

- 实现DMA-BUF exporter操作；
- 正确为RGA attachment建立SG映射；
- SPI Core能够从该缓冲建立有效`tx_sg`。

### 方案B：用户态CMA Heap分配，驱动导入（备选）

优点：

- 延续当前`dma_buf_alloc()`；
- 用户态/RGA改动较少。

风险：

- 驱动要处理fd导入、attach、map、vmap和异常关闭；
- Buffer生命周期更容易被用户态误用；
- SPI Core是否能从导入Buffer的内核映射建立DMA SG需要验证。

决策原则：优先验证方案A；只有Exporter或SPI映射在当前内核不可行时才退到方案B。禁止退回裸物理地址接口。

## 10. RGB565字节序

这是严格零CPU像素处理的关键风险。

当前FBTFT执行：

```text
txbuf16[i] = cpu_to_be16(vmem16[i])
```

用于把ARM小端RGB565转换成ST7789V需要的高字节先发顺序。

验证顺序：

1. 使用Rockchip SPI 16-bit word模式直接发送RGA RGB565；
2. 用红、绿、蓝、黑、白测试块验证线路字节顺序；
3. 若错误，检查Rockchip SPI硬件Endian配置；
4. 再检查RGA是否支持目标字节序/交换输出；
5. 如果硬件两端均无法处理，则严格零CPU方案不成立，需要单独评审硬件字节交换或保留CPU转换。

验收样本：

```text
红：0xF800，应在线路发送 F8 00
绿：0x07E0，应在线路发送 07 E0
蓝：0x001F，应在线路发送 00 1F
```

## 11. SPI分段发送

一帧大小：

```text
320 × 240 × 2 = 153600 bytes
```

单个Rockchip `spi_transfer`上限：

```text
0xffff = 65535 bytes
```

因此像素数据至少拆成3个transfer，建议按偶数字节和完整像素对齐：

```text
transfer[0] = 61440 bytes
transfer[1] = 61440 bytes
transfer[2] = 30720 bytes
```

三个transfer放在同一个`spi_message`内：

- DC在像素消息开始前拉高；
- 不设置中途`cs_change`；
- Buffer保持`SPI_OWNED`直到整个message完成；
- 只在message最终完成回调中释放Buffer。

窗口和`RAMWR`命令可以在像素消息前独立同步发送，延续现有ST7789V可接受的命令/数据CS行为。

## 12. `/dev/fb0`兼容策略

第一版建议保留fbdev兼容，但项目运行时只允许一个Owner：

```text
OWNER_NONE
OWNER_FBDEV
OWNER_DMABUF
```

DMA-BUF模式启用后：

- 项目通过`/dev/st7789v-rga`提交；
- 普通fbdev写入返回`-EBUSY`或不触发刷新；
- 避免fbdev和RGA同时生产画面。

如果保留fb0显著增加第一版复杂度，可先在开发内核中不注册fb0，等零拷贝主链路稳定后再恢复兼容层。此项需要评审决定。

## 13. 异常和恢复

| 异常 | 处理方案 |
|---|---|
| RGA转换失败 | 用户调用`CANCEL_BUFFER`，缓冲回FREE |
| QUEUE状态错误 | 返回`-EINVAL`，记录状态和frame_id |
| 无FREE缓冲 | 非阻塞返回`-EAGAIN`，跳过LCD帧 |
| SPI提交失败 | 当前Buffer转ERROR后回FREE，增加错误计数 |
| SPI超时 | 终止DMA、复位SPI/屏幕、重新初始化 |
| 用户进程崩溃 | release中回收RGA_OWNED/READY，SPI_OWNED等待完成 |
| Suspend | 禁止新提交、停止或排空SPI、关背光、屏幕休眠 |
| Resume | 复位屏幕、重新init、恢复最后一帧或黑屏 |
| Remove | 设置stopping、唤醒等待者、取消work、终止SPI后释放DMA |

建议SPI整帧看门狗初值为200ms，后续按实测发送耗时调整。

## 14. 应用改造

当前LCD CMA申请和CPU读取路径删除：

```text
dma_buf_alloc(... g_lcd_fd ...)
yolo_pic_buf
dma_sync_device_to_cpu(g_lcd_fd)
get_buf_data()
mmap(/dev/fb0)
memcpy(framebuffer, ...)
```

新路径：

```text
open(/dev/st7789v-rga)
  → GET_INFO
  → GET_BUFFER × 2
  → epoll注册LCD fd

每帧：
  ACQUIRE_BUFFER(nonblock)
  → 有Buffer：RGA写该fd
  → RGA成功：QUEUE_BUFFER
  → RGA失败：CANCEL_BUFFER
  → 无Buffer：跳过本帧LCD，不阻塞NPU
```

## 15. 开发任务和评审门

| ID | 工作项 | 依赖 | 风险 | 验证 |
|---|---|---|---|---|
| P0 | RGB565字节序、SPI 16-bit、DMA和3段传输原型 | 无 | 已验证 | 板端红绿蓝三色条正确；16-bit预映射SPI三段传输成功 |
| D1 | 新SPI驱动骨架、DTS、Kconfig、Makefile | P0 | 已验证 | 模块装载、临时driver_override绑定和旧fb0回滚均成功；默认DTS仍未切换 |
| D2 | 双缓冲内存和DMA-BUF共享 | D1 | SPI侧已验证，RGA导入待D3联测 | 驱动分配并导出2×155648字节DMA-BUF；5次分配/释放循环、最终模块装卸和SPI发送通过 |
| D3 | UAPI、poll、状态机、统计 | D2 | 已验证 | `/dev/st7789v-rga`、DMA-BUF fd/mmap、独占打开、poll、非法顺序、EAGAIN、进程退出均通过板端测试 |
| D4 | 异步SPI、分段、完成回调和丢旧帧 | D3 | 已验证 | workqueue提交、三段预映射SPI、完成回调、POLLIN、RGA fd导入和卸载回滚通过 |
| A1 | AIcamera LCD分支接入新接口 | D4 | 已完成首次联测 | 无LCD memcpy、无fb0依赖；离线YOLO+Face+V4L2+RGA+LCD运行通过 |
| T1 | Cache、并发、错误和Suspend测试 | A1 | 高 | 长稳、弱负载、强制退出、休眠恢复 |
| C1 | 是否恢复fb0兼容层 | T1 | 中 | 旧程序兼容与Owner互斥 |

当前已进入A1评审门。DMA-BUF exporter、字符设备UAPI、poll、双缓冲所有权状态机、异步SPI、真实RGA fd导入和AIcamera离线显示均已完成首次验证。

### 15.2 D2板端验证记录

- 最终模块SHA-256：`2fd2e74204d94bc0d626401180ae8b193befc4e9f467976bd0bbd896dfb8bc28`；
- `checkpatch --strict`：0 errors、0 warnings、0 checks；
- ARM 5.10.110交叉编译成功；
- `spi0.0`绑定新驱动后成功分配2块DMA-BUF，每块155648字节；
- 自测试直接从`buffer[0]`的coherent DMA内存通过预映射`tx_dma`发送，RGB三色条正确；
- 连续5次probe/remove分配释放成功；
- 最终解绑、`rmmod`和恢复原`fb_st7789v`成功，`/dev/fb0`恢复；
- 板端未发现新增`WARNING`或`BUG`；
- 未完成项：RGA通过fd导入、RGA写后SPI读的跨设备cache一致性实测，随D4异步SPI路径联合测试。

### 15.3 D3板端验证记录

- 新增UAPI头文件`include/uapi/linux/st7789v_rga.h`；
- 新增独占字符设备`/dev/st7789v-rga`；
- `GET_INFO`返回320×240、stride 640、RGB565、2缓冲、153600字节；
- `GET_BUFFER`成功返回两块DMA-BUF fd，两块均可`mmap()`；
- 第二次打开正确返回`-EBUSY`；
- 初始`poll()`正确返回`POLLOUT`；
- 两块缓冲均处于`RGA_OWNED`时，非阻塞第三次`ACQUIRE`正确返回`-EAGAIN`；
- `CANCEL`、`QUEUE`和READY替换状态转换通过；
- 重复`QUEUE`正确返回`-EINVAL`并增加`invalid_state`；
- 文件关闭自动回收该进程持有的RGA_OWNED/READY缓冲；
- 驱动解绑、模块卸载和原`fb_st7789v`/`dev/fb0`恢复成功；
- 板端未发现新增`WARNING`或`BUG`；
- D3测试程序：`AIChat_demo/tests/st7789v_rga_uapi_test.c`；
- D3遗留的librga真实导入、READY到SPI_OWNED调度和SPI完成事件已在D4验证完成。

### 15.4 D4板端验证记录

- READY由workqueue转为SPI_OWNED，面板窗口命令在可睡眠上下文执行；
- RGB565像素使用持久化`spi_message`和3个预映射`tx_dma` transfer异步提交；
- SPI完成回调只更新状态/统计、唤醒等待者并调度下一帧；
- 两次异步提交均在1秒内产生`POLLIN`完成事件，完成后缓冲重新可ACQUIRE；
- 双缓冲latest-wins已验证：A发送期间，C的ACQUIRE原子抢占等待中的B，板端索引为`second=1、third=1`；
- latest-wins联测统计：`dropped_ready=1`、`displayed=2`、`spi_errors=0`，确认A和最终保留的C均已发送完成；
- 显示帧率实测：15/20/25/30/35/38/39 FPS均无READY丢帧或EAGAIN；40 FPS运行10秒提交400帧，最终显示397帧、替换3帧；
- 不限速生产运行15秒时提交约298万帧，截止时显示600帧，持续SPI显示吞吐约40.0 FPS；其余帧由latest-wins合并，`EAGAIN=0`、`spi_errors=0`；
- 帧率测试程序：`AIChat_demo/tests/st7789v_rga_fps_test.c`；
- 真实RGA三色连续测试：30 FPS运行60秒，RGA生成1800帧、LCD排空后显示1800帧，`dropped_ready=0`、`EAGAIN=0`、`spi_errors=0`；
- 上述测试中RGA三段RGB565填充平均耗时0.824 ms、最大1.457 ms，板端未发现RGA、SPI、DMA-API、WARNING或BUG日志；
- RGA连续测试程序：`AIChat_demo/tests/st7789v_rga_stream_test.c`；
- librga 1.9.1成功通过`importbuffer_fd()`导入显示驱动DMA-BUF；
- RGA直接向同一DMA-BUF写入红绿蓝三段RGB565，随后异步SPI显示成功；
- 联测统计：`displayed=3`、`spi_errors=0`；
- RGA和SPI中断均有增长，未发现DMA-API、WARNING或BUG日志；
- 驱动解绑会停止新提交、取消待执行work并等待在途SPI完成，再释放DMA-BUF；
- 解绑、`rmmod`和原`fb_st7789v`/`dev/fb0`恢复成功；
- RGA联测程序：`AIChat_demo/tests/st7789v_rga_hw_test.c`。

双缓冲约束说明：当A处于SPI_OWNED、B处于READY时，新的ACQUIRE会原子抢占B并把旧帧计入`dropped_ready`，因此等待发送的始终是最新帧。若A处于SPI_OWNED且B已经是RGA_OWNED，则没有可安全覆盖的缓冲，非阻塞ACQUIRE仍返回`-EAGAIN`。后续只有在该窗口频繁出现并影响显示帧率时，才升级为三缓冲；当前先保留双缓冲以降低CMA占用和排队延迟。

### 15.1 P0/D1当前原型使用方法

默认板级DTS仍使用`sitronix,st7789v`，因此现有`/dev/fb0`不受影响。板端验证原型时，临时把`lcd_st7789v`节点改为：

```dts
compatible = "echo,st7789v-rga";
```

重新编译并启动测试内核后，通过对应SPI设备的sysfs节点触发测试，例如：

```sh
echo 1 > /sys/bus/spi/devices/spi0.0/selftest
```

预期画面从左到右为红、绿、蓝三块。需要同时采集：

- `dmesg`中的probe和selftest结果；
- SPI/DMA路径日志，确认进入`rockchip_spi_prepare_dma()`；
- 如有条件，用逻辑分析仪确认RGB565在线路上的高低字节顺序；
- 观察三段transfer之间是否保持正确的像素连续性。

## 16. 验收标准

### 功能

- 方向、RGB/BGR、分辨率正确；
- 无持续花屏、撕裂和混帧；
- 用户进程退出后可以重新启动；
- SPI错误后能恢复或明确进入错误状态；
- Suspend/Resume后屏幕可恢复。

### 零拷贝

- LCD路径无整帧CPU `memcpy`；
- 不调用FBTFT `cpu_to_be16()`像素循环；
- RGA目标Buffer和SPI DMA源Buffer是同一图像存储；
- LCD Buffer不再执行无意义的device-to-CPU同步；
- SPI大包实测进入Rockchip DMA路径。

### 同步

- RGA完成前SPI不读取；
- SPI完成前RGA不覆盖；
- 无Buffer永久卡在RGA_OWNED/SPI_OWNED；
- 显示慢时丢旧帧，不积累延迟。

### 性能和观测

- 输出RGA耗时、SPI发送耗时、显示FPS；
- 输出无Buffer跳帧和READY替换计数；
- 输出SPI错误、超时和恢复计数；
- 对比改造前后的CPU占用和内存带宽。

## 17. 回滚方案

- 保留原`fb_st7789v.c`不修改；
- DTS只需把`compatible`恢复为`sitronix,st7789v`即可回到旧驱动；
- AIcamera通过编译开关保留旧LCD路径，待新链路稳定后再删除；
- 新驱动使用独立Kconfig，关闭配置即可不参与构建。

## 18. 评审需要确认的决策

1. 第一版是否暂时取消`/dev/fb0`，只验证专用零拷贝接口；建议：是。
2. 第一版是否接受同步RGA、异步SPI；建议：是。
3. RGA忙或SPI无空闲Buffer时是否直接跳过LCD帧；建议：是。
4. 是否采用“只保留最新READY帧”；建议：是。
5. 是否先做P0硬件原型，通过后再写完整驱动；建议：是，且作为硬门槛。
6. 如果RGA/SPI均无法解决RGB565字节序，是否接受保留一次硬件外的字节交换；建议：重新评审，不直接接受CPU整帧转换。


