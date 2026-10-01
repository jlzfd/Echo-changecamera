# Echo Mate Linux 驱动开发基础与项目映射

## 1. 一条统一主线

无论是 MIPI、SPI、音频还是 NPU，都可以先套用同一套 Linux 设备模型：

```text
原理图和芯片手册
  → DTS描述硬件连接与资源
  → dtc编译为DTB
  → Bootloader把DTB交给内核
  → 内核创建device
  → 驱动注册driver
  → bus执行match
  → probe获取资源并注册子系统接口
  → 应用open/read/write/ioctl/poll/mmap
  → 驱动配置硬件、DMA和中断
```

DTS不包含执行代码；它描述“板上有哪些设备、地址和连接关系”。驱动才定义“如何操作设备”。

## 2. DTS、device、driver和bus

Linux驱动模型的四个核心对象是：

| 对象 | 作用 | 项目例子 |
|---|---|---|
| device | 一个硬件实例 | `spi_device`、`platform_device`、`i2c_client` |
| driver | 操作一类硬件的方法 | ST7789V驱动、RV1106 Codec驱动 |
| bus | 负责枚举和匹配 | platform、SPI、I2C总线 |
| class/subsystem | 向上提供统一接口 | V4L2、ALSA、字符设备、DMA-BUF |

典型DTS节点：

```dts
device@ff000000 {
    compatible = "vendor,soc-device";
    reg = <0xff000000 0x1000>;
    interrupts = <...>;
    clocks = <...>;
    dmas = <...>;
    status = "okay";
};
```

典型匹配表和驱动：

```c
static const struct of_device_id demo_of_match[] = {
    { .compatible = "vendor,soc-device" },
    { }
};
MODULE_DEVICE_TABLE(of, demo_of_match);

static struct platform_driver demo_driver = {
    .probe = demo_probe,
    .remove = demo_remove,
    .driver = {
        .name = "demo",
        .of_match_table = demo_of_match,
    },
};
module_platform_driver(demo_driver);
```

`compatible`匹配成功只是允许调用`probe()`，并不等于硬件已经工作。时钟、复位、电源、引脚和远端组件仍可能失败。

## 3. 三种项目中常见的总线

### 3.1 platform总线

SoC内部不可动态枚举的控制器通常使用platform总线，例如I2S、CIF、ISP和Codec控制器。

```text
DT节点
  → of_platform_populate()
  → platform_device
  → platform_driver_register()
  → platform_match()
  → probe(platform_device)
```

### 3.2 I2C总线

摄像头Sensor通常挂在I2C上。I2C只用于寄存器控制，图像数据不走I2C。

```text
I2C控制器probe
  → 注册i2c_adapter
  → 根据子节点创建i2c_client
  → compatible匹配i2c_driver
  → sensor probe
  → 注册V4L2 subdev
```

### 3.3 SPI总线

ST7789V是SPI控制器的从设备：

```text
Rockchip SPI控制器platform probe
  → 注册spi_controller
  → SPI核心创建spi_device
  → 匹配ST7789V spi_driver
  → st7789v probe
```

这就是“两级匹配”：先让控制器工作，再匹配控制器下面的从设备。

## 4. probe中通常做什么

建议按以下顺序理解和编写：

```text
校验DTS和设备能力
  → 获取私有结构并保存drvdata
  → 获取reg/irq/clock/reset/regulator/gpio
  → 设置DMA mask、IOMMU或DMA channel
  → 初始化锁、队列、completion/workqueue
  → 申请中断
  → 注册到上层子系统
  → 配置runtime PM
```

常用接口：

```c
devm_kzalloc();
devm_platform_ioremap_resource();
platform_get_irq();
devm_request_irq();
devm_clk_get();
devm_reset_control_get();
devm_gpiod_get();
dma_set_mask_and_coherent();
platform_set_drvdata();
```

`devm_*`资源会在设备解绑时自动释放，但硬件停机顺序仍需在`remove()`和错误路径中处理，例如停止DMA、关闭中断和断电。

## 5. 驱动如何暴露给应用

不同子系统选择不同接口：

| 硬件 | 内核接口 | 应用看到的接口 |
|---|---|---|
| Camera/ISP | V4L2 + Media Controller | `/dev/video*`、`media-ctl` |
| Audio | ALSA/ASoC | `/dev/snd/pcmC*D*`、alsa-lib |
| ST7789V专用驱动 | misc/字符设备 | `/dev/st7789v-rga` |
| NPU | Rockchip NPU驱动 | RKNN Runtime API |

字符设备调用关系：

```text
open/read/write/ioctl/poll/mmap
  → VFS
  → file_operations
  → 驱动私有状态
  → 寄存器/DMA/硬件队列
```

复杂设备应优先接入已有子系统，而不是自行发明一套ioctl，因为子系统已经解决设备枚举、格式协商、缓冲队列和用户态兼容性。

## 6. 中断、下半部和睡眠规则

硬件完成后一般触发IRQ：

```text
硬件置中断状态
  → CPU进入IRQ handler
  → 读取并清除状态
  → 更新最小必要状态
  → 唤醒等待者或调度work
  → 返回
```

硬中断上下文不能睡眠，因此不能直接调用可能阻塞的SPI、I2C或内存分配路径。耗时工作通常放到：

- threaded IRQ；
- workqueue；
- tasklet/softirq（适用范围更窄）；
- 子系统自己的完成线程。

## 7. 锁怎么选

| 原语 | 能否睡眠 | 适用场景 |
|---|---|---|
| `spinlock_t` | 否 | IRQ与进程上下文共享的短状态 |
| `mutex` | 是 | 进程上下文中的较长临界区 |
| `completion` | 等待方可睡眠 | 等待一次硬件任务完成 |
| wait queue | 等待方可睡眠 | `poll/read`等待状态变化 |
| atomic | 否 | 简单计数或标志，不能替代复合状态锁 |

SPI显示驱动用自旋锁保护`FREE/ACQUIRED/QUEUED/IN_FLIGHT`等短状态，是因为完成回调可能处于不能睡眠的上下文。SPI传输本身不能放在自旋锁临界区内。

## 8. DMA基本模型

DMA让设备在不由CPU逐字节搬运的情况下访问内存：

```text
CPU准备描述符
  → 配置DMA地址、长度、方向
  → 启动硬件
  → DMA搬运
  → 中断通知完成
```

地址可能是：

- 物理地址；
- 经过IOMMU映射的IOVA；
- SG表描述的多个段。

不要把用户态fd理解为DMA地址。fd只用于让内核找到共享内存对象，驱动仍需attach和map得到本设备能访问的DMA地址。

## 9. 电源管理和生命周期

典型生命周期：

```text
probe：注册能力，不一定长期开电
open/stream start：runtime resume、开时钟、解除复位
运行：DMA和中断工作
stream stop/release：停DMA、关路径
runtime suspend：关闭空闲模块时钟/电源
remove：注销接口并回收资源
```

摄像头的`s_stream(1)`、ALSA的`trigger(START)`和显示的QUEUE都属于“开始实际数据流”的阶段，不应与`probe()`混为一谈。

## 10. 编译和装载

```text
Kconfig决定功能是否可选
  → defconfig/.config决定y或m
  → Makefile决定编译对象
  → y：链接进内核
  → m：生成.ko
```

项目音频配置可在`echo_rv1106_linux_defconfig`中看到：

```text
CONFIG_SND=y
CONFIG_SND_SOC=y
CONFIG_SND_SOC_ROCKCHIP_I2S_TDM=y
CONFIG_SND_SOC_RV1106=y
CONFIG_SND_SIMPLE_CARD=y
```

## 11. 通用调试顺序

不要一开始就改代码。按层检查：

```text
1. 当前镜像是否包含预期DTB和驱动
2. /proc/device-tree中节点是否存在且status=okay
3. device是否创建
4. driver是否注册并绑定
5. probe是否成功，是否deferred
6. 时钟、GPIO、中断、DMA是否正常
7. 子系统设备节点是否生成
8. 最小工具能否工作
9. 最后再检查业务应用
```

常用命令：

```sh
dmesg | grep -Ei 'probe|defer|error|fail|dma|irq'
find /sys/bus/platform/devices -maxdepth 1
find /sys/bus/spi/devices -maxdepth 1
cat /proc/interrupts
ls -l /dev/video* /dev/snd /dev/st7789v-rga
```

## 12. 项目映射总结

```text
Camera：多驱动通过OF graph和V4L2 async notifier组装
SPI：控制器与面板两级匹配，专用字符设备管理双缓冲
Audio：CPU DAI、Codec、Machine三组件由ASoC绑定
NPU：RKNN Runtime封装模型IO和驱动提交
共享内存：DMA-BUF负责引用，DMA API负责映射和Cache，所有权/Fence负责时序
```



