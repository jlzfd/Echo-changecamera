# ST7789V SPI 显示驱动改造学习复盘

## 1. 改造背景与目标

项目原先使用 Linux `fbtft/fb_st7789v`，屏幕以 `/dev/fb0` 暴露给用户态。传统 fbdev 路径通常是：用户态或 CPU 将像素写入 framebuffer，驱动再把 framebuffer 内容通过 SPI 发送给 ST7789V。

本次改造的目标是缩短 AI Camera 显示链路，让 RGA 直接把转换后的 RGB565 图像写入 SPI 驱动分配的 DMA 内存：

```text
摄像头/推理绘制结果
        │
        ▼
RGA：缩放 + 格式转换
        │  直接写 DMA-BUF FD 对应内存
        ▼
驱动双缓冲 DMA 内存
        │  SPI DMA 直接读取同一块内存
        ▼
ST7789V LCD
```

核心目标如下：

- 去掉“RGA 输出 → CPU memcpy → fbdev framebuffer”的额外复制；
- 用 DMA-BUF FD 在 RGA 与显示驱动之间共享内存；
- 使用双缓冲解耦 RGA 生产与 SPI 发送；
- 生产者过快时只保留最新帧，避免排队导致显示延迟不断累积；
- 保留可观测的统计信息，便于定位丢帧、状态错误及 SPI 异常。

## 2. 本次修改涉及的文件

### 2.1 内核驱动

| 文件 | 作用 |
|---|---|
| `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/fb_st7789v_rga.c` | 新增 ST7789V SPI DMA-BUF 显示驱动 |
| `rv1106-sdk/sysdrv/source/kernel/include/uapi/linux/st7789v_rga.h` | 内核与用户态共用的 ioctl 接口定义 |
| `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/Kconfig` | 新增 `CONFIG_FB_TFT_ST7789V_RGA` 配置项 |
| `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/Makefile` | 将 `fb_st7789v_rga.o` 纳入内核模块构建 |

### 2.2 用户态接入

| 文件 | 作用 |
|---|---|
| `yolov5_demo/cpp/st7789v_rga_uapi.h` | 用户态使用的 UAPI 镜像 |
| `yolov5_demo/cpp/AIcamera_c_interface.cc` | AI Camera 获取显示 buffer、调用 RGA、提交显示帧 |

### 2.3 测试程序

| 文件 | 作用 |
|---|---|
| `AIChat_demo/tests/st7789v_rga_uapi_test.c` | 验证 ioctl、状态切换、双缓冲和异常返回 |
| `AIChat_demo/tests/st7789v_rga_hw_test.c` | 验证 DMA-BUF 映射、测试图案、SPI 显示和统计信息 |
| `AIChat_demo/tests/st7789v_rga_fps_test.c` | 连续提交并统计吞吐、显示帧数和丢帧 |
| `AIChat_demo/tests/st7789v_rga_stream_test.c` | 使用 RGA 连续写入导出的 DMA-BUF，验证完整硬件链路 |

## 3. 驱动注册方式

新驱动不是旧 `fb_st7789v` 的自动替代品，而是一个独立 SPI 驱动：

- 驱动名称：`st7789v-rga`；
- DTS 匹配字符串：`echo,st7789v-rga`；
- 用户接口：misc 设备 `/dev/st7789v-rga`；
- 注册入口：`module_spi_driver(st7789v_rga_driver)`。

`module_spi_driver()` 会展开为模块初始化和退出函数：

```text
加载 fb_st7789v_rga.ko
        │
        ▼
spi_register_driver()
        │
        ▼
SPI core 根据 compatible 匹配设备
        │
        ▼
st7789v_probe()
        ├─ 解析 DTS 属性和 GPIO
        ├─ 配置 SPI mode / bits_per_word / speed
        ├─ 分配两个 coherent DMA buffer
        ├─ 将 buffer 导出为 DMA-BUF
        ├─ 初始化 ST7789V 寄存器
        └─ 注册 /dev/st7789v-rga
```

Kconfig 明确指出该驱动只绑定 `echo,st7789v-rga`，不会自动替换通用 `fb_st7789v`。因此部署时必须确保：

1. 目标 SPI 节点使用新 compatible，或采用明确的驱动绑定方式；
2. 同一个 SPI 设备没有被旧 `fb_st7789v` 抢先绑定；
3. 内核打开 `SPI`、`DMA_SHARED_BUFFER` 及 `CONFIG_FB_TFT_ST7789V_RGA`。

## 4. SPI 接口的准确理解

项目使用的是普通单数据线 SPI，并不是“4-lane SPI”。常见的四根显示控制信号是：

- `SCLK`：SPI 时钟；
- `MOSI`：单线串行数据；
- `CS`：片选；
- `D/C`：区分命令和像素数据。

此外通常还有 `RESET` 和背光控制。`D/C` 不是 SPI 数据 lane，所以“CS、MOSI、SCLK、D/C 共四根线”不能称为 SPI 4-lane。

驱动中存在两类 SPI 操作：

- 初始化命令：CPU 组织短命令，通过同步 SPI 发送；
- 整帧像素：CPU 只负责设置传输描述符并提交，像素载荷由 SPI DMA 从 coherent buffer 读取。

所以正确表述是：**像素数据路径避免 CPU memcpy，但 CPU 仍参与设备控制、提交 DMA 和处理完成回调。**

## 5. DMA-BUF 内存模型

### 5.1 内存分配

驱动在 `probe()` 阶段分配两块显示内存：

```c
dma_alloc_coherent(dev, alloc_size, &dma_addr, GFP_KERNEL);
```

逻辑分辨率为 320×240，格式为 RGB565，每帧有效数据大小为：

```text
320 × 240 × 2 = 153600 bytes
```

实际分配大小按页对齐。每块内存同时保存：

- CPU 虚拟地址 `vaddr`；
- 设备 DMA 地址 `dma_addr`；
- 导出的 `struct dma_buf *`；
- 当前 buffer 状态。

这里使用 `GFP_KERNEL` 是合理的，因为分配发生在 `probe()` 等可睡眠的进程上下文中，不在硬中断、软中断或持有自旋锁的原子上下文中。`GFP_ATOMIC` 只应在不能睡眠的场景使用，而且更容易分配失败。

### 5.2 导出 DMA-BUF FD

驱动通过 `dma_buf_export()` 把 coherent 内存包装成 DMA-BUF。用户态调用 `GET_BUFFER` 时，驱动通过 `dma_buf_fd()` 为调用进程安装 FD。

FD 不是物理地址，也不等于 DMA 地址，它只是进程中的内核对象句柄：

```text
用户态 FD
  │
  ▼
struct dma_buf
  │
  ├─ RGA attach/map → RGA 可访问的 DMA 地址
  └─ 驱动自身保留 → SPI 控制器使用的 DMA 地址
```

同一个 DMA-BUF 被不同设备映射后，设备看到的 DMA 地址不保证相同；有 IOMMU 时尤其不能把它简单理解为唯一物理地址。共享的本质是同一组后端存储页，而不是在所有设备上暴露同一个数值地址。

### 5.3 DMA-BUF 操作

驱动实现了以下 `dma_buf_ops`：

- `attach/detach`：管理 RGA 等外部 DMA 设备的附件；
- `map_dma_buf/unmap_dma_buf`：建立和解除设备 DMA 映射；
- `begin_cpu_access/end_cpu_access`：CPU 访问前后的同步入口；
- `mmap`：供诊断程序映射到用户态；
- `vmap/vunmap`：内核虚拟映射接口；
- `release`：DMA-BUF 生命周期结束时释放私有对象。

正常显示链路不需要 `mmap`。AI Camera 中的 `mmap + DMA_BUF_IOCTL_SYNC` 仅用于打开诊断环境变量后的抓图检查，不属于正常 RGA→LCD 数据路径。

## 6. 用户态 UAPI

驱动提供以下 ioctl：

| ioctl | 作用 |
|---|---|
| `GET_INFO` | 获取宽、高、stride、格式、buffer 数量和大小 |
| `GET_BUFFER` | 获取指定显示 buffer 的 DMA-BUF FD |
| `ACQUIRE_BUFFER` | 获取一块可由 RGA 写入的 buffer |
| `QUEUE_BUFFER` | RGA 写完后，将该 buffer 提交给显示端 |
| `CANCEL_BUFFER` | RGA 失败或放弃本帧时归还 buffer |
| `GET_STATS` | 获取获取、提交、丢弃、显示和 SPI 错误统计 |

`QUEUE_BUFFER` 中预留了 `in_fence_fd`，但当前实现只接受 `-1`，尚未真正接入显式 DMA fence。

## 7. 双缓冲状态机

每个 buffer 具有四种状态：

```text
FREE
  │ ACQUIRE_BUFFER
  ▼
RGA_OWNED
  ├─ CANCEL_BUFFER ──────────────► FREE
  │
  │ RGA 转换完成 + QUEUE_BUFFER
  ▼
READY
  │ display_work 选中最新帧
  ▼
SPI_OWNED
  │ SPI DMA 完成回调
  ▼
FREE
```

状态字段和统计数据由 `state_lock` 自旋锁保护；面板命令和 SPI 提交由 `io_lock` 互斥锁串行化。

### 7.1 获取 buffer

`ACQUIRE_BUFFER` 优先返回 `FREE` buffer。若没有 FREE，但存在尚未发送的 `READY` buffer，驱动会回收旧 READY 帧给生产者覆盖，并累加 `dropped_ready`。

如果两块内存分别处于 `SPI_OWNED` 和 `RGA_OWNED`，则：

- 非阻塞打开返回 `EAGAIN`；
- 阻塞打开在等待队列上等待。

### 7.2 提交最新帧

当新帧执行 `QUEUE_BUFFER` 时，驱动会清除另一块尚未显示的 READY buffer，只保留最新提交帧。其含义不是“所有帧排队显示”，而是：

```text
生产速度 > SPI 显示速度
        │
        ├─ 旧方案：帧排队 → 延迟越来越大
        └─ 当前方案：丢弃过时 READY 帧 → 始终尽量显示最新帧
```

双缓冲最多允许：

- 一块正在被 SPI DMA 读取；
- 另一块由 RGA 写入或等待显示。

SPI 正在读取的 `SPI_OWNED` buffer 绝不会被 RGA 回收，从而避免读写竞争。

## 8. RGA 到 LCD 的完整零拷贝链路

AI Camera 的用户态流程如下：

```text
lcd_driver_init()
  ├─ open("/dev/st7789v-rga", O_RDWR | O_NONBLOCK)
  ├─ GET_INFO，校验 320×240 / RGB565 / 双缓冲
  ├─ GET_BUFFER[0]
  └─ GET_BUFFER[1]

每一帧 lcd_driver_present(source)
  ├─ ACQUIRE_BUFFER
  │    └─ 没有可用 buffer：EAGAIN，本帧直接丢弃，不阻塞相机线程
  ├─ RGA convert_image(source, display_dma_buf)
  │    ├─ 缩放到 320×240
  │    └─ 转换为 BGR565
  ├─ QUEUE_BUFFER
  ├─ display_work 选择 READY 帧
  ├─ SPI DMA 读取同一块内存
  └─ 完成回调将 buffer 置为 FREE
```

数据面不存在中间 CPU memcpy：

```text
源 DMA buffer ──RGA──► 显示 DMA-BUF ──SPI DMA──► LCD GRAM
```

这里的“零拷贝”有明确边界：

- 是：RGA 输出到 SPI 输入之间没有 CPU 像素复制；
- 不是：硬件完全不需要 CPU 控制；
- 不是：LCD 像 DRM 显示控制器一样持续扫描共享 framebuffer；
- SPI LCD 没有外部 scanout framebuffer，仍需逐帧把像素通过 SPI 写入面板 GRAM。

## 9. 为什么使用 BGR565

接口元数据仍表示 16 位 565 像素，但当前 RGA、内存字节序、SPI 16-bit 传输和 ST7789V MADCTL 设置组合后，直接请求 `RGB565` 曾出现红蓝互换、画面偏青。

用户态最终把 RGA 目标格式设置为 `IMAGE_FORMAT_BGR565`，使实际传到面板后的颜色恢复正确。

该问题说明格式不能只看名称，还必须同时确认：

1. RGA 产生的是 RGB565 还是 BGR565；
2. 一个 16 位像素在内存中的大小端顺序；
3. SPI 控制器按 8 bit 还是 16 bit 发送；
4. ST7789V `MADCTL` 的 BGR 位；
5. 面板模组本身的颜色排列。

## 10. SPI 整帧发送与撕裂问题

一帧为 153600 字节，而 Rockchip SPI 单个 transfer 的可靠长度受控制器约束。驱动将一帧拆为三个不超过 61440 字节的 transfer。

关键点是：这三个 transfer 被放入**同一个 `spi_message`** 中：

```text
设置一次显示窗口
发送一次 RAMWR
提交一个 spi_message
  ├─ transfer 0
  ├─ transfer 1
  └─ transfer 2
完成回调
```

它不是把一幅图当成三幅图分别刷新。拆分只用于满足 SPI 控制器传输长度限制，并保持 RGB565 的 2 字节像素对齐。

连续三色条测试早期出现过明显闪烁和类似折叠的撕裂。改造后的主要约束包括：

- 完整帧只设置一次窗口并使用一个异步 message；
- 用 `display-fps` 做软件节拍，默认限制为 30 FPS；
- 不让多个整帧 SPI DMA 同时飞行；
- SPI 完成后才释放 `SPI_OWNED` buffer；
- 生产者过快时覆盖旧 READY 帧，而不是积压发送。

修正后，连续动态三色测试不再出现之前的闪烁和明显撕裂。

但当前驱动没有使用 ST7789V 的 TE（Tearing Effect）引脚和 TE 中断，也没有真正的垂直同步。`display-fps=30` 是软件限速，不等于与面板扫描同步。因此，在某些高速运动画面或不同面板时序下仍可能出现撕裂。

进一步增强方案是：

1. 确认硬件是否将面板 TE 引脚接到 SoC GPIO；
2. 初始化时配置 `TEON` 和需要的扫描线；
3. 将 TE GPIO 配置为中断；
4. READY 帧等待 TE 边沿后再提交 SPI DMA；
5. 增加 TE 超时回退，避免接线异常导致永久不显示。

## 11. DMA 一致性与同步

### 11.1 CPU 与 DMA 的一致性

显示内存由 `dma_alloc_coherent()` 分配，对驱动 CPU 与所属设备提供 coherent 语义。DMA-BUF 的 CPU 访问接口还会对外部附件调用相应的 `dma_sync_sg_for_cpu/device()`。

只要 CPU 映射并修改 DMA-BUF，就必须遵循：

```text
DMA_BUF_SYNC_START
  → CPU 读/写
DMA_BUF_SYNC_END
```

正常显示路径中 CPU 不修改像素，因此不需要为了显示而 `mmap`。

### 11.2 RGA 与 SPI 的先后关系

当前用户态调用 `convert_image()`，只有 RGA 操作完成后才执行 `QUEUE_BUFFER`。因此当前顺序契约为：

```text
RGA 完成写入
    ↓
QUEUE_BUFFER
    ↓
SPI 开始读取
```

它依赖 RGA API 的同步完成语义，而不是显式 fence。若未来把 RGA 改为异步接口，必须接入 `in_fence_fd` 或在用户态等待 RGA fence，否则 SPI 可能在 RGA 尚未写完时读取半帧数据。

### 11.3 多线程和卸载安全

- `state_lock`：保护 buffer 状态、活动索引和统计数据；
- `io_lock`：串行化 ST7789V 命令和帧提交；
- `spi_idle` completion：模块移除前等待正在运行的异步 SPI 完成；
- wait queue：buffer 可用和设备关闭事件通知；
- 单打开限制：同一时刻只允许一个用户进程打开设备，防止多个生产者破坏状态机；
- `remove()`：先设置 stopping、唤醒等待者、注销 misc 设备，再取消工作并等待 SPI 完成后释放内存。

## 12. `poll` 与 `epoll` 的关系

内核驱动实现的是 `file_operations.poll`：

- 有 buffer 可获取时返回可写事件；
- SPI 完成序号变化时返回可读事件；
- 设备停止或移除时返回 HUP。

这并不代表只能使用用户态 `poll()`。`select()`、`poll()` 和 `epoll()` 最终都会调用驱动的 `.poll` 回调。驱动不需要再实现一个单独的“epoll 接口”。

当前 AI Camera 使用 `O_NONBLOCK + ACQUIRE ioctl`，遇到 `EAGAIN` 就丢帧，这对“显示最新画面、不能阻塞摄像头主链路”的目标更合适。若以后一个线程同时管理显示、网络和音频等多个 FD，可在用户态用 epoll 统一等待。

## 13. 与旧 fbdev 方案的对比

| 项目 | 旧 `fb_st7789v` / `/dev/fb0` | 新 `st7789v-rga` |
|---|---|---|
| 用户接口 | fbdev framebuffer | misc + ioctl + DMA-BUF FD |
| 像素写入 | CPU mmap/write 或 memcpy | RGA 直接写导出 buffer |
| 显示内存 | fbdev 管理 | 驱动分配两块 coherent DMA 内存 |
| 排队策略 | fbdev dirty/update 语义 | 双缓冲、最新帧优先 |
| SPI 发送 | fbtft 刷新函数 | 一个异步 SPI message、三个 transfer |
| 生产者阻塞 | 可能因刷新路径阻塞 | 非阻塞获取，忙时丢帧 |
| 帧统计 | 较弱 | ioctl 可查询详细统计 |
| 通用兼容性 | 标准 fbdev 软件可使用 | 需要专用用户态接口 |

新方案的代价是失去了通用 `/dev/fb0` 兼容性，应用必须显式遵守 acquire/queue/cancel 协议。

## 14. 测试和问题定位复盘

### 阶段一：驱动自测

先让驱动生成红、绿、蓝测试画面，验证：

- SPI 引脚和设备初始化正确；
- 16 位 RGB565 能发送；
- SPI DMA 能读取 coherent buffer；
- 屏幕基本方向和显示窗口正确。

### 阶段二：连续 RGA 测试

使用 RGA 连续向两个 DMA-BUF 写入三色动态图案，验证：

- DMA-BUF attach/map 可被 RGA 正常使用；
- acquire/queue 状态机可以长期循环；
- SPI 异步完成后 buffer 能正确回收；
- 最新帧覆盖策略不会死锁。

此阶段发现过快速刷新时的严重闪烁和撕裂，随后通过整帧单 message、发送串行化和软件帧率节拍进行修正。

### 阶段三：接入 AI Camera

接入真实图像后曾先后出现以下现象：

| 现象 | 最终认识 |
|---|---|
| 画面几乎全黑、只有灯是亮点 | 优先检查 ISP/AIQ、曝光和相机输出，不应直接归因于 LCD |
| 能看见轮廓但整体偏青 | 最后一级 RGB/BGR565 与字节序组合不匹配 |
| 显示几帧后卡住 | 后续日志指向 MPP JPEG 配置、FD 生命周期或 stride 异常，不是 SPI 显示状态机本身 |
| 帧率约 10 FPS | 需要拆分 NPU、RGA、JPEG、SPI 各阶段耗时，不能只看最终显示 FPS |
| 优化后约 25 FPS | 说明双缓冲显示驱动不是当时主要推理瓶颈 |

本次联调最重要的经验是按链路逐级隔离：

```text
LCD 驱动自产图
  → RGA 生成测试图
  → Camera 原始画面
  → ISP/AIQ 后画面
  → NPU 绘制结果
  → JPEG/图传并行链路
```

不要在完整应用中同时猜测摄像头、ISP、RGA、NPU、SPI 和 MPP 六个模块。

## 15. 统计字段的诊断价值

`GET_STATS` 提供：

- `acquired`：成功交给生产者的次数；
- `queued`：生产者提交的次数；
- `cancelled`：生产者主动取消次数；
- `released_on_close`：进程异常关闭时驱动回收的次数；
- `dropped_ready`：未显示就被更新帧覆盖的次数；
- `acquire_eagain`：双缓冲都忙时非阻塞获取失败次数；
- `invalid_state`：非法重复提交或错误状态操作次数；
- `displayed`：SPI 成功发送完成的帧数；
- `spi_errors`：SPI 提交或完成异常次数。

可根据统计快速判断瓶颈：

```text
dropped_ready 高：生产者比显示快，延迟低但存在主动丢帧
acquire_eagain 高：RGA/应用持有 buffer 太久，或 SPI 发送耗时过长
queued 高而 displayed 增长慢：检查软件限帧、SPI 带宽和 SPI 错误
invalid_state 增长：用户态 acquire/queue/cancel 协议有 bug
spi_errors 增长：检查控制器、DMA、时钟、消息长度和硬件信号
```

## 16. 当前方案的边界和遗留风险

### 16.1 尚无硬件垂直同步

没有 TE 同步，软件 30 FPS 只能降低冲突概率，不能从原理上消除所有撕裂。

### 16.2 尚无显式 DMA fence

当前要求 RGA 同步完成后再 queue。未来一旦异步化，必须实现 fence 等待或把 fence 传给驱动。

### 16.3 单消费者、单生产者假设

驱动限制单进程打开，简化了状态机，但不支持多个应用共享屏幕。若未来需要合成，应在用户态增加唯一显示服务，不建议直接放开多进程 ioctl。

### 16.4 双缓冲不是无损队列

该设计目标是低延迟而不是保存每一帧。生产者过快时主动丢弃旧帧是正确行为。若要录像或逐帧处理，应使用独立队列，不能复用显示策略。

### 16.5 SPI 带宽上限

60 MHz 理论传输一帧 RGB565 的时间约为：

```text
153600 × 8 / 60,000,000 ≈ 20.48 ms
```

这还未包含命令、间隙、控制器开销和面板限制。理论上约 48.8 FPS，工程上配置 30 FPS 更稳妥。提高帧率前应同时测量 SPI 实际完成时间、CPU 占用、错误率和屏幕稳定性。

## 17. 推荐的后续优化顺序

1. **补齐可重复基准测试**：记录 RGA 时间、queue 时间、SPI DMA 时间、displayed FPS 和丢帧率。
2. **接入 TE 同步**：前提是硬件确实引出了 TE GPIO。
3. **加入显式 fence**：支持异步 RGA，并由驱动在安全时刻消费 READY 帧。
4. **统一 UAPI 头文件来源**：避免内核与用户态手工镜像长期发生字段漂移。
5. **补充异常恢复**：对连续 SPI 错误增加限频日志、统计告警和可控面板重初始化。
6. **完善 DTS binding 文档**：明确 `dc-gpios`、`reset-gpios`、旋转、BGR、SPI 频率和 `display-fps` 属性。
7. **持续做链路隔离测试**：显示异常先运行驱动自产图和 RGA 测试，不直接在完整 AI 应用中猜原因。

## 18. 最终总结

本次改造的本质不是“让 `/dev/fb0` 支持 mmap”，而是绕过传统 fbdev 的 CPU framebuffer 更新路径，建立一条专门面向 RGA 的 DMA-BUF 显示通道：

```text
RGA 生产者
   │ acquire / queue / cancel
   ▼
驱动双缓冲状态机
   │ latest-frame-wins
   ▼
SPI DMA
   │
   ▼
ST7789V GRAM
```

它实现了 RGA 输出到 SPI 输入之间的零 CPU 拷贝，通过状态机保证 RGA 不会覆盖 SPI 正在读取的内存，并通过“最新帧优先”避免弱显示端拖慢摄像头和推理主链路。

需要长期牢记三点：

1. DMA-BUF FD 是共享对象句柄，不是物理地址；
2. 零拷贝不等于 CPU 完全不参与，也不等于没有同步问题；
3. 双缓冲解决所有权和低延迟问题，TE/fence 才分别解决扫描同步和异步设备同步问题。
