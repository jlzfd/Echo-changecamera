# MPP 硬件 JPEG 编码 (Media Process Platform) — 知识卡片

## 1. 一句话概括

MPP (Media Process Platform) 是 Rockchip 多媒体硬件抽象层，调用 VE (Video Encoder) 的 MJPEG 模式，将 NV12 dma-buf fd 编码为 JPEG bitstream，零 CPU 拷贝替代 `cv::imencode`。

---

## 2. 为什么需要它

| | CPU cv::imencode | MPP 硬件 MJPEG |
|---|---|---|
| 640×640 编码 | ~5ms (A7 单核 100%) | **~2ms (硬件独立)** |
| 448×448 编码 | ~3ms | **~1ms** |
| CPU 占用 | 每 5s 占满一次 CPU | **0%** |
| 数据流 | cv::Mat → stb/turbojpeg → vector<uchar> | NV12 fd → MPP → JPEG bitstream |
| 是否零拷贝 | 否（像素需在 cv::Mat 中） | **是（EXT_DMA import fd）** |

---

## 3. Linux 底层原理

### 3.1 硬件架构

```
Userspace (librockchip_mpp.so)
    │
    │ ioctl(/dev/mpp_service)
    ↓
Kernel (drivers/video/rockchip/mpp/)
    │
    ├─ mpp_service → 设备管理 + 权限控制
    ├─ mpp_task   → 任务提交/完成
    ├─ mpp_dev    → VE (Video Encoder) 设备抽象
    └─ ve_regs    → VE 硬件寄存器控制
         │
    ┌────┴────┐
    │ VE 硬件  │ → DCT 变换 → 量化 → 哈夫曼编码 → 输出 JPEG stream
    │ (H.264/H.265/MJPEG) │
    └─────────┘
```

### 3.2 同步 encode vs 异步 put/get

```c
// 同步路径 (简单 — 我们优先用)
MPP_RET ret = mpi->encode(ctx, frame, &packet);
// 内部: put_frame + poll + get_packet，一次调用等完成

// 异步路径 (fallback — 同步失败时用)
mpi->encode_put_frame(ctx, frame);          // 提交帧
mpi->poll(ctx, MPP_PORT_OUTPUT, 50);       // 等 50ms
mpi->encode_get_packet(ctx, &packet);      // 拿结果
```

### 3.3 EXT_DMA 零拷贝原理

```c
// CPU 路径: memcpy 像素到 MPP buffer
// EXT_DMA 路径: MPP 直接读 dma-buf fd 的物理地址

MppBufferInfo info = {
    .type = MPP_BUFFER_TYPE_EXT_DMA,   // 外部 DMA fd
    .fd   = g_nv12_fd,                   // dma-buf fd
    .ptr  = g_nv12_virt,                // CPU virt (backup)
    .size = g_nv12_size,                 // 总大小
};
mpp_buffer_import(&buf, &info);
// 内核: 通过 fd 获取物理地址 → MPP 内部 DMA 映射 → 直接读
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 初始化

```c
// ① 创建 MPP 上下文
MppCtx  g_mpp_ctx = NULL;
MppApi *g_mpp_mpi = NULL;
mpp_create(&g_mpp_ctx, &g_mpp_mpi);
// g_mpp_mpi 是函数指针表: encode, encode_put_frame, poll, encode_get_packet, control

// ② 初始化为 MJPEG 编码器
mpp_init(g_mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingMJPEG);
// MPP_CTX_ENC=编码, MPP_CTX_DEC=解码
// MPP_VIDEO_CodingMJPEG=MJPEG 模式, 不是 H.264/H.265

// ③ 配置编码参数
MppEncCfg g_enc_cfg;
mpp_enc_cfg_init(&g_enc_cfg);
mpp_enc_cfg_set_s32(g_enc_cfg, "prep:width",  448);
mpp_enc_cfg_set_s32(g_enc_cfg, "prep:height", 448);
mpp_enc_cfg_set_s32(g_enc_cfg, "prep:format", MPP_FMT_YUV420SP);  // 只吃 NV12
mpp_enc_cfg_set_s32(g_enc_cfg, "rc:quality",  85);                // JPEG 质量

mpi->control(g_mpp_ctx, MPP_ENC_SET_CFG, g_enc_cfg);
```

### 4.2 构建输入帧 (EXT_DMA 零拷贝)

```c
static MPP_RET build_frame_from_dma(int fd, void* virt, size_t size,
                                     int w, int h, MppFrame* out) {
    // ① 导入 dma-buf 为 MPP buffer
    MppBufferInfo info = {
        .type = MPP_BUFFER_TYPE_EXT_DMA,
        .fd   = fd,           // CMA fd → MPP 直接 DMA 读
        .ptr  = virt,         // CPU virt (备用)
        .size = size,
    };
    MppBuffer buf;
    mpp_buffer_import(&buf, &info);    // 零拷贝导入

    // ② 构建 MppFrame — 描述这张 NV12 图
    mpp_frame_init(&frame);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_width(frame, w);
    mpp_frame_set_height(frame, h);
    mpp_frame_set_hor_stride(frame, w);   // 无 padding
    mpp_frame_set_ver_stride(frame, h);
    
    *out = frame;
    return MPP_OK;
}
```

### 4.3 提取 JPEG bitstream

```c
void  *pkt_data = mpp_packet_get_data(packet);    // JPEG 数据
size_t pkt_len  = mpp_packet_get_length(packet);  // 字节数

// 验证 JPEG SOI 头
uint8_t *raw = (uint8_t*)pkt_data;
bool valid = (pkt_len >= 2 && raw[0] == 0xFF && raw[1] == 0xD8);

// 拷贝到输出 buffer (必须要 copy — packet 在 deinit 后会释放)
g_jpeg_buf.assign(raw, raw + pkt_len);
*jpeg_data = g_jpeg_buf.data();
*jpeg_size = g_jpeg_buf.size();
```

---

## 5. 完整调用流程

### 5.1 初始化 (start_ai_camera_v2 时调用一次)

```
hw_jpeg_encoder_init(640, 640, 448, 448, 85)

① dma_buf_alloc(302KB NV12 CMA buffer)
   → g_nv12_fd, g_nv12_virt  ← 中间 buffer，每帧复用

② mpp_create() → mpp_init(MJPEG)
   → 打开 /dev/mpp_service → 初始化 VE 硬件为 MJPEG 模式

③ mpp_enc_cfg_init() → 配 prep:448×448 NV12 + rc:quality=85
   → mpi->control(MPP_ENC_SET_CFG)
```

### 5.2 每帧编码 (DescribeCurrentScene 时调用)

```
hw_jpeg_encode(src_fd, &jpeg_data, &jpeg_size)

① RGA: BGR(src_fd, 640×640) → NV12(g_nv12_fd, 448×448)
   一步完成 CSC + resize, fd→fd

② dma_sync_device_to_cpu(g_nv12_fd)
   等 RGA 写完, CPU/MPP 可见

③ build_frame_from_dma(g_nv12_fd) → MppFrame
   EXT_DMA 导入，零拷贝

④ mpi->encode(ctx, frame, &packet)
   ├ 成功 → packet 里有 JPEG
   └ 失败 → fallback: encode_put_frame + poll + encode_get_packet

⑤ 检查 SOI 头 (0xFF 0xD8)
   ├ 有效 → 直接用
   └ 缺失 → warn 但继续 (某些 MPP 版本第一帧无头)

⑥ 拷贝到 g_jpeg_buf → 交给上游 base64_encode
```

---

## 6. 我的项目中使用

| 文件 | 行号 | 作用 |
|------|------|------|
| `hw_jpeg_encoder.h` | 全部 | C API: init / encode / deinit |
| `hw_jpeg_encoder.cc` | 全部 | 完整实现，含 DEBUG 宏 |
| `hw_jpeg_encoder.cc:94` | init | CMA alloc + MPP 初始化 |
| `hw_jpeg_encoder.cc:180` | encode | RGA→MPP 编码主函数 |
| `hw_jpeg_encoder.cc:48-83` | build_frame_from_dma | EXT_DMA 零拷贝导入 |
| `AIcamera_c_interface.cc:849` | init 调用 | `hw_jpeg_encoder_init(640,640,448,448,85)` |
| `Application.cc:334-343` | encode 调用 | HW 优先 + CPU fallback |

---

## 7. 涉及源码

| 层 | 路径 | 关键内容 |
|----|------|---------|
| 用户态 MPP | `rv1106-sdk/media/mpp/release_mpp_*/include/rockchip/` | `rk_mpi.h`, `mpp_frame.h`, `mpp_packet.h` |
| 用户态 MPP 库 | `librockchip_mpp.so` | MPP API 实现 |
| 内核 MPP | `drivers/video/rockchip/mpp/` | `mpp_service.c`, `mpp_dev_ve.c` |
| JPEG 标准 | ISO/IEC 10918-1 | SOI/SOF/SOS/DQT/DHT/EIO 标记 |

---

## 8. 常见 Bug 及调试方法

### 8.1 mpp_init(MJPEG) 失败

```
症状: mpp_init 返回非 MPP_OK
原因: 
  ① /dev/mpp_service 不存在 → VE 驱动未加载
  ② RV1106 VE 不支持 MJPEG → 换 H.264 试试
调试:
  ls /dev/mpp_service
  dmesg | grep -i "mpp\|ve"
```

### 8.2 编译错误: "invalid conversion from 'int' to 'MPP_RET'"

```
原因: MPP_RET 是 C++ 强类型 enum，不能用 `return 0`
修复: 用 `return MPP_OK` 而不是 `return 0`
  同理: `(MppPollType)50` 而不是 `50`
```

### 8.3 首帧 SOI 头缺失

```
症状: 第一帧 encode 出来的数据不以 0xFF 0xD8 开头
原因: 某些 MPP 版本 MJPEG 首帧不输出 JPEG 头
当前处理: warn + 继续（大部分 VLM 能容错）
最佳修复: 加 jpeg:header_mode=1 config → mpp_enc_cfg_set_s32
  或者: 手动插入 \xFF\xD8 前缀
```

### 8.4 encode 返回 OK 但 packet 为空

```
症状: mpi->encode 返回 MPP_OK, packet 是 NULL
原因: 同步 encode 内部路径不支持 → 自动 fallback 异步路径
代码已处理: hw_jpeg_encoder.cc:227-269 的 fallback 逻辑
```

### 8.5 CMA 分配失败

```
症状: dma_buf_alloc 返回 -1, size=302KB
原因: CMA 耗尽 (256MB 总内存, CMA 只配了 ~64MB)
排查: cat /proc/meminfo | grep Cma
```

### 8.6 MPP buffer import 失败

```
症状: mpp_buffer_import 返回错误
原因: fd 指向的 buffer 不是 dma-buf / EXT_DMA type 不支持
调试: 确认 fd 是 dma_buf_alloc 分配的，确认 size 正确
```

---

## 9. 性能优化点

| 优化 | 说明 | 效果 |
|------|------|------|
| EXT_DMA 导入 | mpp_buffer_import fd → 零拷贝 | 省 302KB memcpy |
| NV12 buffer 复用 | init 时分配一次，每帧覆盖写 | 省 alloc/free 开销 |
| resize 到 448 | JPEG 448×448 约 50KB，够 VLM 看 | 比 640 省 40% 带宽 |
| quality=85 | JPEG 质量和大小平衡点 | ~50KB/帧 |
| 同步 encode 优先 | 少一次 put/get ioctl | 省 ~100μs |
| g_jpeg_buf 复用 | vector 不反复分配 | 避免 malloc |

---

## 10. 面试高频问题

1. **MPP 是什么？和 OpenCV imencode 的区别？** Rockchip 多媒体硬件抽象层，调用 VE 硬件编码，CPU 零占用。OpenCV 是 CPU 软件编码。

2. **为什么 MPP 只吃 NV12？** MJPEG 编码前需要 YUV 色彩空间。NV12 是 YUV420 的一种，VE 硬件直接支持。BGR→NV12 由 RGA 完成。

3. **EXT_DMA 导入为什么是零拷贝？** mpp_buffer_import(type=EXT_DMA, fd=xxx) 让 MPP 直接读 dma-buf 的物理地址，不经过 CPU memcpy。

4. **同步 encode vs 异步 put/get 分别适用什么场景？** 同步适合单帧编码 (简单)，异步适合批量编码 (高吞吐)。我们优先同步，失败 fallback 异步。

5. **SOI marker 缺失怎么处理？** 加 config jpeg:header_mode=1，或手动插入 \xFF\xD8 前缀。

6. **为什么还要 CPU fallback？** 防 MPP 初始化失败或硬件不支持。RV1106 上 MPP 初始化失败 → 回退 cv::imencode。

7. **JPEG 质量 85 怎么选的？** 85 是质量和文件大小的体验平衡点。太高文件大传输慢，太低 VLM 看不清。

8. **NV12 中间 buffer 多大？** 448×448×3/2 = 301,056 bytes ≈ 302KB。

---

## 11. 补充学习

- MPP 开发手册: `MPP_Development_Manual.pdf`
- JPEG 文件格式: SOI(0xFFD8) / APP0 / DQT(量化表) / SOF(帧头) / DHT(哈夫曼表) / SOS(扫描头) / ECS(压缩数据) / EOI(0xFFD9)
- MJPEG 编码流程: DCT → 量化 → Zig-Zag → 哈夫曼编码
- VE 硬件 vs 软件 JPEG: 软件用 libjpeg-turbo (SIMD 加速), 硬件用 VE DCT 加速器
