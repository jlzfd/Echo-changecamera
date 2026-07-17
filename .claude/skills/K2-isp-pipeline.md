# ISP 图像处理管线 — 知识卡片

## 1. 一句话概括

ISP (Image Signal Processor) 负责把 sensor 输出的 Bayer RAW 转换为 YUV/RGB，同时执行 3A (AE/AWB/AF)、降噪、去马赛克、Gamma 校正等算法。在 RV1106 上通过 rkisp 驱动实现，参数通过 `/dev/video12` 下发，采集从 `/dev/video11` 取。

---

## 2. 为什么需要它

sensor 输出的原始 Bayer RAW 是**不能直接给人或 NPU 看的**：

```
Sensor 输出的 Bayer RAW (GRBG 排列):
  G R G R G R ...
  B G B G B G ...
  G R G R G R ...
  ↑ 每个像素只有一种颜色，另外两种靠周围像素插值

ISP 处理后:
  NV12 (Y 亮度 + UV 色度分开) → Y 是灰度图，UV 是色彩
  RGB → 人眼习惯的颜色空间
```

没有 ISP 的话，画面是黑白交错的花屏。RGA 也认不出 RAW 格式。

---

## 3. Linux 底层原理

### 3.1 RV1106 ISP 硬件管线

```
Sensor (SC3336) → MIPI DPHY → CSI2 Controller → CIF (Video Input)
                                                      │
                                                      ↓ Bayer RAW
                                              ┌─ RKISP ─────────────────┐
                                              │ ① DPCC   坏点校正       │
                                              │ ② BLC    黑电平校正     │
                                              │ ③ LSC    镜头阴影校正   │
                                              │ ④ DEMOSAIC 去马赛克     │
                                              │ ⑤ GAMMA   Gamma 校正   │
                                              │ ⑥ WDR    宽动态         │
                                              │ ⑦ 3DNR   3D 降噪       │
                                              │ ⑧ CSC    颜色空间转换   │
                                              │ ⑨ OUTPUT  NV12/RGB      │
                                              └─────────────────────────┘
                                                      │
                                                      ↓ NV12
                                              DMA → CMA buffer
                                                      │
                                              /dev/video11 (V4L2 CAPTURE)
```

### 3.2 三个 video 节点

| 节点 | 用途 | 方向 |
|------|------|------|
| `/dev/video11` | 采集输出 | ISP → 用户态（我们拿帧） |
| `/dev/video12` | 参数输入 | 用户态 → ISP（3A 参数/RKIQ 文件） |
| `/dev/video13` | 统计输出 | ISP → 用户态（AE/AWB 直方图） |

### 3.3 参数控制流程

```
RKAIQ 库 (librkaiq.so)
    │
    │ 读取 sc3336_CMK-OT2119-PC1_30IRC-F16.json (IQ tuning 参数)
    │ 根据当前场景计算 3A 值
    │
    ├─ AE (自动曝光): 调整 sensor 曝光时间 + 增益
    │     → V4L2_CID_EXPOSURE, V4L2_CID_ANALOGUE_GAIN
    │     通过 I2C 写 sensor 寄存器
    │
    ├─ AWB (自动白平衡): 调整 R/G/B 增益
    │     → 分析 video13 的统计直方图
    │     → 调整 ISP RGB 增益寄存器
    │
    └─ AF (自动对焦): SC3336 定焦镜头，不使用
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 RKISP V4L2 采集端（我们用的）

```c
// 跟普通 V4L2 一样，只是 pixelformat 是 NV12
struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .fmt.pix_mp = {
        .width       = 640,
        .height      = 480,
        .pixelformat = V4L2_PIX_FMT_NV12,  // ISP 输出 NV12
        .field       = V4L2_FIELD_NONE,
    },
};
ioctl(fd, VIDIOC_S_FMT, &fmt);
```

### 4.2 IQ 文件结构

```json
// sc3336_CMK-OT2119-PC1_30IRC-F16.json 关键字段
{
  "sensor": {
    "width": 2304, "height": 1296,       // sensor 原始分辨率
    "fps": 30,
    "flip": false
  },
  "AWB": {
    "rgain": 1.5, "bgain": 1.8,          // 红/蓝通道增益
    "illumination_index": [3000, 5000, 6500]  // 色温索引
  },
  "LSC": { ... },       // 镜头阴影校正表 (数百个网格点)
  "CCM": { ... },       // 颜色校正矩阵 (3×3)
  "DPCC": { ... },      // 坏点校正阈值
  "Gamma": { ... },     // Gamma 曲线查找表
  "NR": { ... }         // 降噪强度参数
}
```

### 4.3 DTS 中的 ISP 链路

```dts
// rv1106-echo-mate-ipc.dtsi — 我们写的

// Sensor → CSI DPHY
&csi2_dphy0 {
    port { endpoint { remote-endpoint = <&sc3336_out>; data-lanes = <1 2>; } };
};

// CSI2 Controller → CIF
&mipi0_csi2 {
    port@1 { endpoint { remote-endpoint = <&cif_mipi_in>; } };
};

// CIF → ISP
&rkcif_mipi_lvds {
    port { cif_mipi_in: endpoint { remote-endpoint = <&mipi_csi2_output>; }; };
};

// ISP 最终输出
&rkisp_vir0 {
    port@0 { isp_in: endpoint { remote-endpoint = <&mipi_lvds_sditf>; }; };
};
```

---

## 5. 完整调用流程

### 5.1 启动流程

```
① Kernel probe:
   sc3336.ko → I2C 匹配 → 注册 v4l2_subdev
   rkisp.ko  → 注册 /dev/video11/12/13

② RKAIQ 初始化:
   rkaiq_init()
   → 加载 sc3336_xxx.json IQ 文件
   → 初始化 3A 算法状态
   → 启动 AE/AWB 控制线程
   → 写 video12 下发初始 ISP 参数

③ sensor s_stream(on):
   I2C 写 STREAMING 寄存器
   → MIPI 开始传 Bayer RAW
   → ISP 接收 → 处理 → 输出 NV12

④ V4L2 STREAMON:
   → ISP 开始填充 vb2 buffer
   → 中断通知 buffer 就绪
```

### 5.2 每帧中断处理

```
ISP frame done 中断:
  ① 读取 video13 统计信息 (AE/AWB 直方图)
  ② RKAIQ 更新 3A 参数
  ③ 写 video12 下发新的 ISP 参数 (下一帧生效)
  ④ buffer 标记 DONE → vb2_queue 通知 DQBUF 可读
```

### 5.3 用户态看到的

```
我们的代码 (AIcamera_c_interface.cc):

v4l2_capture_get_frame()
  → VIDIOC_DQBUF (等 ISP 中断)
  → isp_fd = 预导出的 fd (指向 ISP 刚写好的 NV12 buffer)

RGA: isp_fd(NV12) → arena_fd(RGB)

void // ISP 下一帧在这期间仍然在跑，只是写到另一个 buffer 里
```

---

## 6. 我的项目中使用

| 功能 | 文件 | 说明 |
|------|------|------|
| DTS ISP 链路 | [rv1106-echo-mate-ipc.dtsi](D:\echo\rv1106-sdk\sysdrv\source\kernel\arch\arm\boot\dts\rv1106-echo-mate-ipc.dtsi) | sensor→dphy→csi2→cif→isp 完整链路 |
| IQ 文件 | `sc3336_CMK-OT2119-PC1_30IRC-F16.json` | 白平衡/降噪/Gamma 参数 |
| ISP 驱动 | `drivers/media/platform/rockchip/isp/rkisp.c` | 内核驱动 |
| V4L2 采集 | [v4l2_capture.cc](D:\echo\yolov5_demo\cpp\v4l2_capture.cc) | 我们的封装 |

---

## 7. 涉及源码

| 层 | 路径 | 关键内容 |
|----|------|---------|
| 内核 ISP 驱动 | `drivers/media/platform/rockchip/isp/rkisp.c` | 中断处理、buffer 管理 |
| 内核 ISP 寄存器 | `drivers/media/platform/rockchip/isp/rkisp-regs.h` | ISP 硬件寄存器定义 |
| CIF 接收 | `drivers/media/platform/rockchip/cif/` | MIPI 数据接收 |
| RKAIQ 库 | `media/isp/release_camera_engine_rkaiq_*/` | `librkaiq.so`, 3A 算法 |
| IQ 文件 | `media/isp/out/isp_iqfiles/sc3336_*.json` | sensor 专属调优参数 |
| DTS 配置 | `rv1106-echo-mate-ipc.dtsi:112-301` | ISP 链路全部 DTS |

---

## 8. 常见 Bug 及调试方法

### 8.1 ISP 没输出 (DQBUF 永久阻塞)

```
症状: 第一步就卡住，VIDIOC_DQBUF 不返回
排查顺序:
  ① dmesg | grep -i "isp\|cif\|csi\|mipi\|sc3336"
     看 probe 是否成功，error 日志
  ② media-ctl -p                           # 看 pipeline 链路是否完整
  ③ cat /sys/kernel/debug/clk/clk_summary | grep -E "mipi|isp|cif"
     确认时钟是否 enable
  ④ v4l2-ctl -d /dev/video11 --stream-mmap --stream-count=1
     裸测能否出帧
  ⑤ i2cdetect -y 4                         # I2C4 总线上能否看到 0x30
     读 sensor CHIP_ID 寄存验证 sensor 活着
```

### 8.2 画面偏色/过曝/偏暗

```
症状: 出图了但颜色不对
原因: IQ 文件不匹配或未加载
检查:
  ① ls /oem/usr/share/iqfiles/
     确认 sc3336_xxx.json 存在
  ② rkaiq_tool --get_awb_gain            # 查看当前白平衡值
  ③ 更换匹配的 IQ 文件 (同型号 sensor 下模组不同 IQ 也不同)
```

### 8.3 MIPI 数据错乱 (花屏/线条)

```
症状: 画面有规则的条纹或花乱
原因:
  - MIPI lane 数不对 (配了 2-lane 但实际接了 1-lane)
  - data-lanes 映射不对 (物理 lane1→逻辑 lane0)
  - MIPI clock 太高 (信号质量差)
修复:
  ① 降低 MIPI 频率: assigned-clock-rates 从 400MHz → 300MHz
  ② 确认 sensor datasheet 的 lane 配置
  ③ 加 MIPI 信号等长走线测试 (硬件问题)
```

### 8.4 sensor 驱动没 probe

```
症状: dmesg 里看不到 "sc3336" 相关日志
原因:
  - DTS compatible 不匹配驱动的 of_match_table
  - I2C 总线没 enable
  - pwdn-gpio 状态不对 (sensor 一直掉电)
调试:
  # 检查 I2C
  i2cdetect -y -r 4          # 扫描 I2C4
  # 地址 0x30 有设备 → I2C 通
  # 地址 0x30 无设备 → 查 GPIO/供电
  
  # 手动拉 gpio
  echo 0 > /sys/class/gpio/gpio\<N\>/value  # 拉低 pwdn
  echo 1 > /sys/class/gpio/gpio\<N\>/value  # 拉高 pwdn, sensor 启动
```

### 8.5 调试工具速查

```bash
# Media Controller 链路查看
media-ctl -d /dev/media0 -p

# 查看 ISP 版本
cat /sys/class/video4linux/video11/name

# 查看当前帧率
v4l2-ctl -d /dev/video11 --get-parm

# 导出 IQ 文件当前值
rkaiq_tool --dump_iq
```

---

## 9. 性能优化点

| 优化点 | 说明 | 当前状态 |
|--------|------|---------|
| NV12 直出 | ISP 输出 NV12 而非 RGB，交给 RGA 转 | ✅ 已做 |
| IQ 文件精简 | 去掉不用的模块 (AF/WDR) 减少每帧参数更新量 | 中性 |
| 3A 频率降低 | AE/AWB 不需要每帧更新，2-3 帧一次即可 | 默认 |
| ISP 缩放 | 如果最终只需 640x640，让 ISP 直接出对应尺寸 | ❌ 当前 ISP 出 640×480，RGA 再缩放 |

---

## 10. 面试高频问题

1. **ISP 在相机 pipeline 中的位置？** sensor → ISP → V4L2 video11。ISP 输入 Bayer RAW，输出 NV12/RGB。

2. **3A 是什么？分别在哪做？**
   - AE (自动曝光): 调 sensor 曝光时间+增益，通过 I2C 写 sensor 寄存器
   - AWB (自动白平衡): 调 ISP 内部 R/G/B 增益，通过 video12 下发
   - AF (自动对焦): 调镜头马达 (SC3336 定焦，不用)

3. **为什么 3 个 video 节点？** video11=采集输出(dqbuf 拿帧)，video12=参数输入(写 ISP 寄存器)，video13=统计输出(读直方图让 RKAIQ 算 3A)

4. **Bayer RAW → RGB 的去马赛克原理？** 每个像素只有一种颜色，通过周围像素加权插值补全另外两种。双线性是最简单的，ISP 用更高级的 edge-aware 算法。

5. **IQ 文件是干什么的？** 每款 sensor+镜头模组都有不同的色彩特性，IQ 文件存储校准后的 LSC(镜头阴影)、CCM(色彩矩阵)、Gamma(色调曲线)、降噪强度等参数。换模组不换 IQ → 偏色。

6. **ISP 和 sensor 的时钟关系？** sensor 用 MCLK(我们 27MHz，DTS `clocks = <&cru MCLK_REF_MIPI0>`)，内部 PLL 产生 pixel clock。ISP 用 PCLK_ISP，由 CRU 提供。

7. **MIPI lane 数量怎么选？** 由 sensor 支持 + 带宽需求决定。SC3336 2-lane 10-bit 约 500Mbps，1080p@30fps 需要 ~500Mbps，2-lane 刚好。

8. **如何在不改代码的情况下切换 sensor？** 换 DTS 的 compatible + IQ 文件 + sensor 驱动 .ko。前提是用同一套 V4L2 接口。

9. **RKISP 和 RKCIF 的关系？** CIF 是 MIPI 数据接收 + 打包，ISP 是图像处理。CIF 进来 Bayer RAW → ISP 处理 → 输出 NV12。

10. **ISP 丢帧的原因？** buffer 不够 (QUEUED 少于 2 个)、DDR 带宽不足、ISP 时钟太低、MIPI 信号质量差导致 CRC 错误。

---

## 11. 补充学习

- `Documentation/media/v4l-drivers/rkisp1.rst` — 内核文档
- Media Controller API — `Documentation/media/media-controller.rst`
- MIPI CSI-2 协议: Lane Management, Data Type, ECC/CRC
- Bayer Pattern 原理: RGGB/GRBG/GBRG/BGGR 四种排列
- 3A 算法基础知识:
  - AE: 亮度直方图 → 目标亮度 → P/I/D 控制曝光时间级数
  - AWB: 灰度世界假设 vs 完美反射假设
  - Gamma: 为什么需要非线性映射 (人眼感知 vs 线性光强)
