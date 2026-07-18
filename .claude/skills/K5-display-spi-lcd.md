# SPI LCD 显示 (ST7789V + RGA) — 知识卡片

## 1. 一句话概括

ST7789V SPI 屏通过内核 fbtft 驱动注册为 `/dev/fb0`，RGA 硬件将 Arena RGB 640×640 一次转换为 RGB565 320×240 写入 CMA buffer，SPI DMA 自动推到 LCD 面板，CPU 零拷贝。

---

## 2. 为什么需要它

RV1106 无 MIPI DSI/并口 RGB，只能用 SPI 屏。传统 CPU 路径每帧 ~2MB memcpy：

```
cv::cvtColor RGB2BGR → cv::resize 640→320 → cv::cvtColor BGR2BGR565 → memcpy
```

RGA 替代后：1 次硬件操作完成 RGB→RGB565+缩放，数据直接写到 CMA buffer。

---

## 3. Linux 底层原理

```
Userspace                    Kernel
════════                     ══════

convert_image()              fbtft 框架 (drivers/staging/fbtft/fbtft-core.c)
  → ioctl(RGA_BLIT_SYNC)      → 注册 /dev/fb0
  → RGA 硬件写 CMA             → 周期性 DMA 推屏
                              fb_st7789v.c → 初始化序列 (MADCTL/COLMOD)
                              SPI 控制器 (spi-rockchip.c) → SPI 总线 → 面板

PWM 背光: pwm-bl 驱动 → /sys/class/backlight/backlight/brightness
```

SPI 4 线模式：CS(PC0) + DC(PD0: 命令/数据) + SCL + SDA @ 60MHz

---

## 4. 核心代码

### 初始化

```c
// LCD CMA buffer — 一次性分配
yolo_pic_buf_size = 320 * 240 * 2;  // 150KB RGB565
dma_buf_alloc(CMA_HEAP, yolo_pic_buf_size, &g_lcd_fd, (void**)&yolo_pic_buf);

// RGA 目标描述符 — 每帧复用
g_rga_lcd.format     = IMAGE_FORMAT_RGB565;
g_rga_lcd.width      = 320;  g_rga_lcd.height = 240;
g_rga_lcd.virt_addr  = yolo_pic_buf;   // CPU 可读
g_rga_lcd.fd         = g_lcd_fd;       // RGA 硬件可写
```

### 每帧

```c
// ⑨ LCD: 一次 RGA 调用完成 RGB→RGB565 + resize
dma_sync_cpu_to_device(arena_in->fd);
convert_image(&g_rga_dst, &g_rga_lcd, NULL, NULL, 0);
dma_sync_device_to_cpu(g_lcd_fd);
// → fbtft 驱动通过 SPI DMA 推屏
```

### DTS

```dts
// 背光 PWM
backlight { pwms = <&pwm9 0 300000 0>; default-brightness-level = <50>; };

// SPI LCD
fbtft@0 {
    compatible = "sitronix,st7789v";
    spi-max-frequency = <60000000>;
    fps = <60>; rotate = <270>; buswidth = <8>;
    dc-gpios = <&gpio1 RK_PD0 GPIO_ACTIVE_HIGH>;
    reset-gpios = <&gpio1 RK_PC4 GPIO_ACTIVE_LOW>;
};
```

---

## 5. 数据流

```
Arena RGB (640×640, CMA fd=6)
  ↓ RGA: RGB→RGB565 + resize 640→320 (fd→fd)
LCD CMA buffer (320×240 RGB565, fd=7)
  ↓ fbtft SPI DMA
ST7789V 面板
```

---

## 6. 涉及源码

| 层 | 路径 | 关键函数 |
|----|------|---------|
| 用户态 RGA | `rga_convert.c` | `convert_image` |
| 初始化 | `AIcamera_c_interface.cc:788-811` | LCD buffer alloc + RGA desc |
| 每帧 | `AIcamera_c_interface.cc:402-406` | RGA→LCD 转换 |
| DTS | `rv1106-echo-mate-ipc.dtsi:61-73` | PWM 背光 |
| DTS | `rv1106-echo-mate-ipc.dtsi:305-329` | SPI + ST7789V |
| fbtft 驱动 | `drivers/staging/fbtft/fbtft-core.c` | 通用 SPI LCD 框架 |
| ST7789V 驱动 | `drivers/staging/fbtft/fb_st7789v.c` | 初始化序列 |
| SPI 控制器 | `drivers/spi/spi-rockchip.c` | SPI DMA |
| PWM 背光 | `drivers/video/backlight/pwm_bl.c` | `/sys/class/backlight` |

---

## 7. 常见 Bug

| Bug | 原因 | 调试 |
|-----|------|------|
| 白屏 | fbtft 未 probe / SPI 未通 | `dmesg \| grep fbtft` |
| 颜色异常 | RGB/BGR 字节序反 / MADCTL 错 | 试改 DTS rotate/bgr |
| 撕裂 | 写 LCD buffer 时 fbtft 在读 | 降 fps 或双 buffer |
| 无背光 | PWM 未使能 | `cat /sys/class/backlight/backlight/brightness` |

---

## 8. 面试高频问题

1. **为什么不并行口/MIPI？** RV1106 无 DSI 控制器，SPI 是唯一选择。
2. **RGA 单次操作做了什么？** RGB→RGB565 颜色转换 + 640→320 缩放 + fd→fd 写入。
3. **fbtft 工作原理？** 注册 /dev/fb0 → 周期 DMA 推显存 → SPI 发送到 LCD。
4. **PWM 背光怎么控制？** 内核 pwm-backlight 驱动，sysfs 写 brightness。
5. **怎么解决画面撕裂？** SPI 屏无 vsync，降 fps、双 buffer 减轻。
6. **rotate 怎么实现？** ST7789V MADCTL 寄存器控制扫描方向，DTS 传入。

---

## 9. 补充学习

- ST7789V datasheet: MADCTL、GRAM 寻址、初始化序列
- `drivers/staging/fbtft/` — 最简单的 Linux 驱动入门
- SPI 协议 Mode 0/3、CPOL/CPHA
