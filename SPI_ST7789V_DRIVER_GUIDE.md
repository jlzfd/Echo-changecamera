# Echo Mate SPI/ST7789V 驱动修改说明

## 1. 文档范围与当前状态

本文说明 Echo Mate（RV1106）项目中 SPI LCD 链路的项目定制修改，覆盖：

- DTS 中 SPI0、引脚和 ST7789V 节点的配置；
- Linux SPI core、Rockchip SPI 控制器驱动和 ST7789V 从设备驱动的匹配关系；
- 项目新增的 `fb_st7789v_rga.c` 驱动；
- 双缓冲、DMA-BUF、RGA、异步 SPI DMA 和最新帧策略；
- 用户态 UAPI、应用调用、编译切换、验证和故障排查。

当前仓库同时保留两套显示方案：

| 方案 | DTS compatible | 用户接口 | 状态 |
|---|---|---|---|
| 原 FBTFT 方案 | `sitronix,st7789v` | `/dev/fb0` | 默认板级 DTS 仍使用 |
| 项目专用方案 | `echo,st7789v-rga` | `/dev/st7789v-rga` + DMA-BUF | 驱动和测试代码已实现并完成板端原型验证，启用时需切换 DTS/配置 |

因此，本文所说的“新驱动”并没有自动替换旧驱动。最终由 DTB 中 LCD 节点的 `compatible` 决定绑定哪一个驱动。


                        Linux SPI 驱动完整流程
                                 │
                    ┌────────────┴────────────┐
                    │                         │
                 设备侧                     驱动侧
                    │                         │
                   DTS                st7789v_rga.c
                    │                         │
                   dtc              module_spi_driver()
                    │                         │
                   DTB                module_driver()
                    │                         │
              Bootloader                   module_init
                    │                         │
                    │              ┌──────────┴──────────┐
                    │              │                     │
                    │             =y                    =m
                    │              │                     │
                    │         __initcall           init_module
                    │              │                alias
                    │         do_initcalls              │
                    │              │              insmod/modprobe
                    │              │                     │
                    │              └──────────┬──────────┘
                    │                         │
                    │                spi_register_driver
                    │                         │
                    ▼                         ▼
            struct spi_device         struct spi_driver
                    │                         │
                    └────────────┬────────────┘
                                 │
                              SPI BUS
                                 │
                              match()
                                 │
                  compatible ↔ of_match_table
                                 │
                            匹配成功
                                 │
                                 ▼
                 st7789v_rga_probe(spi)
                                 │
                       真正初始化硬件

## 2. 修改目标

旧链路主要是：

```text
应用 RGB 图像
  -> CPU/RGA 生成临时显示图
  -> memcpy 到 /dev/fb0 映射区
  -> FBTFT 刷新线程
  -> SPI 发送到 ST7789V
```

存在的问题是显示缓冲归属不清晰、可能发生整帧 CPU 拷贝，而且应用不能直接把驱动显示缓冲交给 RGA。

新链路的设计目标是：

```text
摄像头/推理结果 RGB888 DMA-BUF
  -> RGA 直接写驱动导出的 RGB565 DMA-BUF
  -> QUEUE_BUFFER
  -> SPI 异步 DMA 读取同一块物理内存
  -> ST7789V
```

核心收益：

1. 去掉应用到 framebuffer 的整帧 `memcpy`；
2. 两块显示缓冲初始化时一次分配，循环复用；
3. RGA 与 SPI 通过 DMA-BUF 共享同一块底层内存；
4. SPI 发送期间，RGA 可以准备另一块缓冲；
5. 积压时丢弃未显示的旧帧，优先显示最新结果，降低视觉延迟；
6. 通过明确的 buffer 状态和所有权避免 RGA、SPI 同时读写同一块内存。

## 3. 代码与配置位置

| 层次 | 文件 | 用途 |
|---|---|---|
| 板级 DTS | `rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/rv1106g-echo-mate.dts` | 启用 SPI0，关闭同 CS 的 spidev，设置 60 MHz |
| 板级外设 DTSI | `rv1106-sdk/sysdrv/source/kernel/arch/arm/boot/dts/rv1106-echo-mate-ipc.dtsi` | SPI pinctrl、LCD compatible、DC/RESET GPIO |
| 新内核驱动 | `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/fb_st7789v_rga.c` | DMA-BUF 双缓冲、队列、异步 SPI 刷屏 |
| 内核 UAPI | `rv1106-sdk/sysdrv/source/kernel/include/uapi/linux/st7789v_rga.h` | ioctl 数据结构与命令 |
| Kconfig | `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/Kconfig` | `CONFIG_FB_TFT_ST7789V_RGA` |
| Makefile | `rv1106-sdk/sysdrv/source/kernel/drivers/staging/fbtft/Makefile` | 生成 `fb_st7789v_rga.o`/模块 |
| 应用 UAPI 副本 | `yolov5_demo/cpp/st7789v_rga_uapi.h` | 用户态接口定义 |
| 应用接入 | `yolov5_demo/cpp/AIcamera_c_interface.cc` | 打开设备、导入双缓冲、RGA 转换、提交帧 |
| 测试 | `AIChat_demo/tests/st7789v_rga_*.c` | UAPI、RGA、连续帧和帧率验证 |

## 4. DTS 配置和两级驱动匹配

### 4.1 SPI0 控制器

RV1106 SoC DTS 定义 SPI0 控制器资源，包括寄存器、中断、时钟和 DMA 通道。板级 DTS/DTSI 再启用控制器并配置引脚：

```dts
&spi0 {
    status = "okay";
    pinctrl-0 = <&spi0m0_clk &spi0m0_miso
                 &spi0m0_mosi &spi0m0_cs0>;
    #address-cells = <1>;
    #size-cells = <0>;
};
```

控制器节点由 Rockchip SPI platform driver 匹配。内核配置为：

```text
CONFIG_SPI_ROCKCHIP=y
```

其 `probe()` 注册 `spi_controller`。之后 SPI core 才能枚举控制器下面的 `fbtft@0` 从设备。

### 4.2 SPI 从设备节点

项目 LCD 使用 SPI0 的 CS0，即 `reg = <0>`，最高频率为 60 MHz：

```dts
&spi0 {
    lcd_st7789v: fbtft@0 {
        compatible = "echo,st7789v-rga";
        reg = <0>;
        spi-max-frequency = <60000000>;
        rotation = <270>;
        display-fps = <30>;
        dc-gpios = <&gpio1 RK_PD0 GPIO_ACTIVE_HIGH>;
        reset-gpios = <&gpio1 RK_PC4 GPIO_ACTIVE_LOW>;
    };
};
```

注意：仓库默认 DTSI 当前仍写为：

```dts
compatible = "sitronix,st7789v";
```

这会绑定旧 `fb_st7789v` 并生成 `/dev/fb0`。启用新驱动时必须改成 `echo,st7789v-rga`，否则新驱动的 `probe()` 不会执行。

同一个总线和片选不能同时放两个启用的从设备。`rv1106g-echo-mate.dts` 已将 `spidev@0` 设为 `disabled`，避免它与 LCD 同时占用 `spi0.0`。

### 4.3 完整匹配流程

```text
U-Boot 加载 rv1106g-echo-mate.dtb
  -> 内核展开设备树
  -> Rockchip SPI platform driver 匹配 SPI0 控制器
  -> rockchip_spi_probe() 注册 spi_controller
  -> SPI core 枚举 CS0，创建 spi_device（spi0.0）
  -> 比较 spi_device 的 compatible
  -> "echo,st7789v-rga" 命中 st7789v_rga_of_match[]
  -> 调用 st7789v_rga_probe(struct spi_device *spi)
  -> 初始化屏幕、缓冲区和 /dev/st7789v-rga
```

新驱动匹配表：

```c
static const struct of_device_id st7789v_rga_of_match[] = {
    { .compatible = "echo,st7789v-rga" },
    {}
};
```

驱动通过 `module_spi_driver(st7789v_rga_driver)` 向 SPI core 注册。这里的 LCD 驱动是 SPI 从设备驱动；真正操作 SPI 控制器寄存器和 DMA engine 的仍是 `spi-rockchip` 主控制器驱动。

## 5. probe 初始化流程

`st7789v_rga_probe()` 的主要工作如下：

```text
1. devm_kzalloc() 创建驱动私有对象
2. 读取 width/height/rotation/display-fps/bgr
3. 获取 DC 和 RESET GPIO
4. 初始化 mutex、spinlock、waitqueue、completion、delayed_work
5. 设置 SPI_MODE_0、8 bit 命令模式和最大频率
6. spi_setup()
7. 分配 2 个 coherent DMA buffer
8. 将每个 buffer 导出为 dma_buf
9. 复位并发送 ST7789V 初始化命令
10. 创建 selftest sysfs 属性
11. 注册 misc 字符设备 /dev/st7789v-rga
```

面板逻辑分辨率为 240×320；旋转 270° 后，应用通过 `GET_INFO` 看到 320×240。格式固定为 RGB565：

```text
有效帧大小 = 240 × 320 × 2 = 153600 bytes
分配大小   = PAGE_ALIGN(153600) = 155648 bytes
双缓冲总分配约 304 KiB（不含元数据）
```

## 6. DMA-BUF 双缓冲实现

### 6.1 内存如何分配

每个显示缓冲通过：

```c
vaddr = dma_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
```

分配后同时得到：

- `vaddr`：CPU/内核虚拟地址；
- `dma_addr`：SPI 控制器可使用的 DMA 地址；
- 固定的 coherent DMA 后端内存。

随后使用 `dma_buf_export()` 封装成 `struct dma_buf`。应用调用 `GET_BUFFER` 时，驱动通过 `dma_buf_fd()` 为同一对象安装一个用户态 fd。

```text
驱动 coherent buffer
  ├─ vaddr：面板自测或内核访问
  ├─ dma_addr：SPI DMA 发送
  └─ dma_buf
       └─ fd：应用传给 RGA
```

fd 不是物理地址，也不是 CMA 下标；它是当前进程文件描述符表中指向 `dma_buf` 的句柄。

### 6.2 RGA 如何访问

RGA 导入 fd 后，DMA-BUF framework 回调新驱动的：

```text
attach       -> st7789v_dmabuf_attach()
map_dma_buf  -> st7789v_dmabuf_map()
unmap        -> st7789v_dmabuf_unmap()
detach       -> st7789v_dmabuf_detach()
```

`attach()` 用 `dma_get_sgtable()` 建立 SG 描述；`map_dma_buf()` 再用 `dma_map_sgtable()` 映射为 RGA 可访问的 DMA 地址。即使 SPI 与 RGA 看到的 DMA 地址不同，它们仍指向同一份底层存储。

### 6.3 CPU cache 同步

DMA-BUF ops 还实现了：

```text
begin_cpu_access -> dma_sync_sgtable_for_cpu()
end_cpu_access   -> dma_sync_sgtable_for_device()
```

正常显示路径由 RGA 写、SPI DMA 读，没有 CPU 像素访问。诊断时应用 `mmap()` 并读取像素，才需要使用 `DMA_BUF_IOCTL_SYNC` 对 CPU 访问进行 START/END 包围。

## 7. 缓冲区状态机和所有权

每个 slot 有四种状态：

```text
FREE
  | ACQUIRE_BUFFER
  v
RGA_OWNED
  | RGA 同步转换完成 + QUEUE_BUFFER
  v
READY
  | display_work 选中
  v
SPI_OWNED
  | SPI DMA completion
  v
FREE
```

各状态的所有者：

| 状态 | 所有者 | 允许的操作 |
|---|---|---|
| `FREE` | 驱动池 | 应用可 acquire |
| `RGA_OWNED` | 当前打开文件 | RGA 可写；可 queue 或 cancel |
| `READY` | 驱动显示队列 | 等待 SPI，不应再写 |
| `SPI_OWNED` | SPI 控制器 | DMA 正在读，不可覆盖 |

驱动使用 `state_lock` 保护状态、统计和 active slot；使用 `io_lock` 串行化面板命令与像素窗口设置。设备只允许一个应用打开，第二次 `open()` 返回 `-EBUSY`，从根源上减少多进程抢占。

## 8. 最新帧策略

这是项目修改中最重要的低延迟机制之一。

当应用申请缓冲时：

1. 优先取得 `FREE`；
2. 若没有 `FREE`，允许回收尚未送入 SPI 的 `READY`；
3. 正在 `SPI_OWNED` 的缓冲绝不覆盖；
4. 非阻塞模式无可用缓冲时返回 `-EAGAIN`。

当新帧 `QUEUE_BUFFER` 时，驱动会释放其他仍处于 `READY` 的旧帧，只保留最新待显示帧：

```text
SPI 正在显示 B0(frame 10)
B1 READY(frame 11)
应用又获得并提交最新可回收槽(frame 12)
  -> frame 11 计入 dropped_ready
  -> SPI 完成后优先发送 frame 12
```

该策略优化的是“画面新鲜度”，不是保证每一帧都显示。对于实时陪伴/安防画面，显示最新检测结果通常比排队播放旧帧更重要。

## 9. SPI 异步传输

应用执行 `QUEUE_BUFFER` 后不会等待整帧 SPI 传输完成。驱动只更新状态并调度 `display_work`。

`display_work` 的流程：

```text
READY slot
  -> 标记 SPI_OWNED
  -> 设置 ST7789V 显示窗口
  -> 组成 spi_message
  -> spi_async()
  -> 立即返回工作队列
  -> Rockchip SPI 控制器启动 DMA
  -> DMA/中断完成
  -> st7789v_spi_complete()
  -> slot 变回 FREE，唤醒 poll/阻塞 acquire
```

RGB565 一帧为 153600 字节。驱动按 61440 字节切成 3 个 `spi_transfer`：

```text
61440 + 61440 + 30720 = 153600 bytes
```

每个 transfer 使用 16 bits-per-word，保证不会在单个 RGB565 像素中间拆分。`spi_message.is_dma_mapped = true`，并直接设置 coherent buffer 对应的 `tx_dma`，避免 SPI core 再为这块内存建立临时映射。

理论裸线传输时间：

```text
153600 × 8 / 60 MHz ≈ 20.48 ms
```

加上命令、片选、DMA 调度等开销，60 MHz 下 30 FPS 是更稳妥的目标。驱动通过 `display-fps` 和 `next_submit_jiffies` 节流，避免无意义地堆积 SPI 请求。

## 10. 用户态接口

设备节点：

```text
/dev/st7789v-rga
```

主要 ioctl：

| ioctl | 作用 |
|---|---|
| `GET_INFO` | 获取宽高、stride、RGB565 格式、缓冲数和大小 |
| `GET_BUFFER` | 按 index 导出 DMA-BUF fd |
| `ACQUIRE_BUFFER` | 获取可写 slot；非阻塞无缓冲时返回 `EAGAIN` |
| `QUEUE_BUFFER` | RGA 完成后提交显示，并携带 frame_id |
| `CANCEL_BUFFER` | 转换失败时归还 slot |
| `GET_STATS` | 获取 acquire、drop、display、SPI error 等统计 |

`poll()` 语义：

| 事件 | 含义 |
|---|---|
| `POLLOUT` | 有可 acquire 的 FREE/READY buffer |
| `POLLIN` | 自上次读取统计后有 SPI 完成事件 |
| `POLLHUP` | 驱动正在卸载/设备停止 |

UAPI 中预留了 `in_fence_fd`，但当前实现只接受 `-1`。也就是说当前应用必须保证 RGA 操作已经完成再 `QUEUE_BUFFER`。项目使用同步 `convert_image()`，所以不需要 fence；如果以后切换为异步 RGA，必须在驱动中真正导入并等待 acquire fence，不能直接传一个未实现的 fence fd。

## 11. 当前应用的完整调用流程

`AIcamera_c_interface.cc` 中的流程如下：

### 11.1 初始化

```text
open("/dev/st7789v-rga", O_RDWR | O_NONBLOCK)
  -> GET_INFO，校验 320×240/RGB565/2 buffers
  -> 对 index 0、1 调用 GET_BUFFER
  -> 获得两个 DMA-BUF fd
  -> 填入 image_buffer_t，供 librga importbuffer_fd()
```

### 11.2 每帧显示

```text
摄像头帧 -> RGA/推理/CPU 叠加框，形成 RGB888 g_display_rgb
  -> ACQUIRE_BUFFER 得到 index
  -> convert_image(RGB888 source, RGB565 dma-buf[index])
  -> 同步 RGA 调用返回，说明目的 buffer 写入完成
  -> QUEUE_BUFFER(index, in_fence_fd=-1, ++frame_id)
  -> 应用继续处理下一帧
  -> 驱动后台异步 SPI 刷屏
```

如果 RGA 转换失败，应用调用 `CANCEL_BUFFER`；如果 acquire 返回 `EAGAIN`，当前帧可以直接跳过显示，不阻塞摄像头/NPU 主链路。

## 12. 与旧 FBTFT 驱动的差异

| 维度 | 旧 `fb_st7789v` | 新 `fb_st7789v_rga` |
|---|---|---|
| 接口 | Linux framebuffer `/dev/fb0` | 项目 misc UAPI `/dev/st7789v-rga` |
| 缓冲 | framebuffer 内存 | 两块 coherent DMA-BUF |
| RGA 直写 | 应用通常需要中间缓冲/拷贝 | 支持 fd 导入后直接写 |
| 提交 | fb 写入/脏页刷新 | acquire/queue/cancel 状态机 |
| SPI | FBTFT 刷屏路径 | `spi_async()` + completion callback |
| 丢帧 | 不突出实时最新帧 | READY 旧帧可回收，只保留最新帧 |
| 多进程 | framebuffer 通用访问 | 独占 open |
| 回退 | 默认方案 | 将 compatible 改回旧值即可 |

新驱动没有修改 `drivers/spi/spi-rockchip.c`。Rockchip 控制器驱动继续负责寄存器、FIFO、DMA channel、中断和实际时钟；项目修改集中在 ST7789V 从设备层和应用显示协议层。

## 13. 编译与启用

Kconfig 依赖：

```text
CONFIG_FB_TFT
CONFIG_SPI
CONFIG_DMA_SHARED_BUFFER
CONFIG_FB_TFT_ST7789V_RGA
```

Makefile 已包含：

```make
obj-$(CONFIG_FB_TFT_ST7789V_RGA) += fb_st7789v_rga.o
```

当前源码树 `.config` 可确认 `CONFIG_SPI_ROCKCHIP=y`、DMA-BUF/CMA heap 和旧 `CONFIG_FB_TFT_ST7789V=y`，但没有看到新选项处于启用状态。因此正式固化时应完成两项：

1. 在板级 defconfig 中启用 `CONFIG_FB_TFT_ST7789V_RGA=y` 或 `m`；
2. 将 LCD 节点 compatible 改为 `echo,st7789v-rga`，重新编译并部署 DTB。

若编译为模块，需要保证模块进入 rootfs 并在绑定前加载；若内建为 `y`，内核启动时会直接匹配。

回退方式：

```text
compatible 改回 "sitronix,st7789v"
  + 保留 CONFIG_FB_TFT_ST7789V
  -> 恢复 /dev/fb0 旧链路
```

## 14. 验证方法

### 14.1 启动与匹配

```sh
dmesg | grep -E 'st7789|spi0|rockchip-spi'
ls -l /dev/st7789v-rga
readlink /sys/bus/spi/devices/spi0.0/driver
```

期望日志包含两块 DMA-BUF、分辨率、旋转和帧率信息。

### 14.2 面板与 SPI DMA 自测

```sh
echo 1 > /sys/bus/spi/devices/spi0.0/selftest
```

屏幕应显示红、绿、蓝色条。该测试绕过应用和 RGA，可用于区分“面板/SPI 问题”与“应用转换问题”。

### 14.3 UAPI 和状态机测试

对应测试程序：

```text
AIChat_demo/tests/st7789v_rga_uapi_test.c
```

覆盖：独占打开、DMA-BUF 导出与 mmap、双 acquire、`EAGAIN`、cancel、重复 queue 拒绝、poll、旧 READY 帧丢弃和进程退出回收。

### 14.4 RGA 和持续帧测试

```text
st7789v_rga_hw_test.c      单帧 RGA fd 导入和 SPI 显示
st7789v_rga_stream_test.c  双缓冲连续提交
st7789v_rga_fps_test.c     帧率与统计验证
```

重点观察 `GET_STATS`：

- `displayed` 持续增长；
- `spi_errors` 应为 0；
- 高负载时 `dropped_ready` 增长是最新帧策略生效，不一定是错误；
- `invalid_state` 增长通常表示应用重复 queue、错误 index 或所有权顺序错误。

## 15. 常见问题排查

### 15.1 没有 `/dev/st7789v-rga`

按顺序检查：

1. DTB 中 compatible 是否仍为 `sitronix,st7789v`；
2. `CONFIG_FB_TFT_ST7789V_RGA` 是否启用；
3. 模块是否已加载；
4. `spi0.0` 是否已被旧 FBTFT 或 spidev 占用；
5. DC/RESET GPIO 获取是否失败。

### 15.2 probe 成功但屏幕不显示

先执行 sysfs selftest：

- selftest 失败：检查 SPI 模式、频率、CS/DC/RESET、供电和排线；
- selftest 成功：检查应用 `GET_BUFFER -> ACQUIRE -> RGA -> QUEUE` 顺序及 RGB565 格式；
- 有 `spi_errors`：检查 Rockchip SPI DMA 日志、传输长度和时钟稳定性。

### 15.3 色彩或方向异常

- 红蓝互换：检查 `bgr`；
- 方向错误：检查 `rotation`，新驱动读取的是 `rotation`，不是旧 FBTFT 节点里的 `rotate`；
- 图像错位：检查 `GET_INFO` 返回的 width、height、stride，并确保 RGA 输出是 RGB565。

### 15.4 延迟或掉帧

- `dropped_ready` 增长：生产帧率高于 SPI 消费速率，驱动在主动保留最新帧；
- `acquire_eagain` 增长：两块 buffer 一块正在 SPI，另一块正在 RGA，应用应跳过本次显示或使用 `poll(POLLOUT)`；
- 不应为了“零掉帧”无限排队，否则会把实时画面变成历史回放；
- 60 MHz 下整帧裸传输约 20.48 ms，实际帧率还受面板和调度开销限制。

## 16. 已知限制和后续改进

1. 默认 DTS/defconfig 尚未正式切到新驱动，当前属于可切换、已验证的专用方案；
2. `in_fence_fd` 字段仅预留，当前不支持异步 RGA fence；
3. 缓冲数固定为 2，分辨率和 RGB565 也是面向当前面板的设计；
4. 驱动位于 staging/fbtft 目录，但接口是项目专用 misc device，并非标准 DRM/KMS；
5. `display_work` 选择首个 READY slot，低延迟主要依靠 queue 时清理其他 READY；
6. 若未来需要通用显示栈、page flip、标准 fence 和 compositor，应评估迁移到 DRM tiny driver，而不是继续扩展私有 UAPI。

## 17. 一句话总结

项目的 SPI 修改不是重写 Rockchip SPI 控制器，而是在 ST7789V 从设备层新增了一个面向实时视觉的显示驱动：启动时预分配并导出两块 coherent DMA-BUF，RGA 直接写 RGB565，应用通过明确的 acquire/queue 状态机提交，驱动用异步 SPI DMA 刷屏并在积压时丢旧保新，从而减少 CPU 拷贝并控制端到端显示延迟。


