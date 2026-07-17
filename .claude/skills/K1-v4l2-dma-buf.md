# V4L2 DMA-BUF 零拷贝采集 — 知识卡片

## 1. 一句话概括

Linux V4L2 框架下，通过 `VIDIOC_EXPBUF` 将 ISP 驱动的 DMA buffer 导出为 dma-buf fd，用户态拿到 fd 直接传给 RGA/NPU，**像素数据始终在 CMA 物理内存中，CPU 从未触碰**。

---

## 2. 为什么需要它

| | 传统 OpenCV 方式 | V4L2 DMA-BUF 零拷贝 |
|---|---|---|
| 数据流 | ISP→内核→`read()`→用户态 buffer→`cv::Mat` | ISP→内核 DMA buffer→fd→RGA→NPU |
| CPU 拷贝次数 | 至少 1 次（kernel→userspace） | **0** |
| 每帧开销 | 640×480×1.5 = 460KB memcpy | 仅传递一个 int fd |
| 内存占用 | 额外 460KB 用户态 buffer | 共享内核 CMA buffer |
| 延迟 | memcpy + cv::resize ~2-3ms | RGA 硬件 ~500μs |

在 256MB RV1106 上，零拷贝不是优化，是生存策略。

---

## 3. Linux 底层原理

### 3.1 整体架构

```
Userspace                    Kernel Space
════════                     ════════════

Application                  V4L2 核心 (v4l2-core)
    │                              │
    │ ioctl(VIDIOC_DQBUF)          │ 从 vb2_queue 的 done_list 取 buffer
    │ ← buf_idx, isp_fd            │
    │                              │
    │ RGA importbuffer_fd(isp_fd)  │ dma-buf 框架
    │    → 内核找物理地址           │   dmabuf->ops->map_dma_buf()
    │    → RGA DMA 读 NV12          │
    │                              │
    │ ioctl(VIDIOC_QBUF)           │ buffer 重新加入 queued_list
    │                              │   → ISP 驱动可以覆盖写这一块
```

### 3.2 vb2_queue buffer 状态机

```
  QUEUED ──→ ACTIVE ──→ DONE ──→ QUEUED
    ↑         ↑          ↑         ↑
  用户态     ISP 正在    ISP 填完  用户态
  QBUF      填充        触发中断    QBUF
```

- `VIDIOC_DQBUF`：从 DONE 取走 → 用户持有
- `VIDIOC_QBUF`：重新加入 QUEUED → ISP 可以用

### 3.3 EXPBUF 的底层实现

```c
// 内核 vb2_v4l2.c 简化
int vb2_ioctl_expbuf(struct file *file, struct v4l2_exportbuffer *p) {
    struct vb2_buffer *vb = q->bufs[p->index];
    
    // vb2_dc_get_dmabuf 把 vb2_dma_contig 的 dma_addr 包装成 dma-buf
    vb->dbuf = vb2_dc_get_dmabuf(vb->planes[0].mem_priv);
    
    // 返回匿名 fd
    p->fd = dma_buf_fd(vb->dbuf, O_RDWR);
}
```

### 3.4 dma-buf 的引用计数

```
EXPBUF bufi=0 → fd=3  ───→ dma_buf refcount=1
RGA importbuffer_fd(3)  ─→ dma_buf refcount=2 (RGA 驱动持有)
close(3)                 ─→ dma_buf refcount=1 (用户态不再访问)
RGA releasebuffer_handle ─→ dma_buf refcount=0 → 但物理内存属于 vb2 管理
                                                      ↑
                            vb2_queue 清空时才真正 free
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 初始化相关

```c
// ① 打开设备
int fd = open("/dev/video11", O_RDWR | O_NONBLOCK);

// ② 查询能力
struct v4l2_capability cap;
ioctl(fd, VIDIOC_QUERYCAP, &cap);
// 确认: cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE
//       cap.capabilities & V4L2_CAP_STREAMING

// ③ 设置格式
struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .fmt.pix_mp.width       = 640,
    .fmt.pix_mp.height      = 480,
    .fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12,    // ISP 直接输出 NV12
    .fmt.pix_mp.num_planes  = 1,
};
ioctl(fd, VIDIOC_S_FMT, &fmt);

// ④ 请求 buffer
struct v4l2_requestbuffers req = {
    .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
    .memory = V4L2_MEMORY_MMAP,    // 映射方式
    .count  = 4,                   // 3-4 个够用
};
ioctl(fd, VIDIOC_REQBUFS, &req);
```

### 4.2 MMAP 映射

```c
for (int i = 0; i < 4; i++) {
    struct v4l2_buffer buf = { .type = ..., .memory = V4L2_MEMORY_MMAP, .index = i };
    struct v4l2_plane planes[1];
    
    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    // buf.m.planes[0].length = 460800 (640×480×1.5)
    // buf.m.planes[0].m.mem_offset = i * 460800
    
    void *addr = mmap(NULL, buf.m.planes[0].length,
                      PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, buf.m.planes[0].m.mem_offset);
    // addr = 驱动映射的虚拟地址（不是我们用的，只是让 mmap 不报错）
}
```

### 4.3 EXPBUF 导出 fd（核心）

```c
// 预导出：初始化时做一次，运行时直接用
int exported_fd[4];

for (int i = 0; i < 4; i++) {
    struct v4l2_exportbuffer exp = {
        .type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .index = i,
        .plane = 0,
        .flags = O_RDWR,
    };
    ioctl(fd, VIDIOC_EXPBUF, &exp);
    exported_fd[i] = exp.fd;   // 存下来，每帧直接用
}
```

### 4.4 运行时获取/归还

```c
// 获取：阻塞等 ISP 填满一帧
int get_frame(int *fd_out) {
    struct v4l2_buffer buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ioctl(fd, VIDIOC_DQBUF, &buf);        // 阻塞，等 ISP 中断
    *fd_out = exported_fd[buf.index];     // 拿出预导出的 fd
    return buf.index;
}

// 归还：RGA 读完数据后立即归还
void put_frame(int buf_idx) {
    struct v4l2_buffer buf = {
        .type = ..., .memory = ..., .index = buf_idx,
    };
    ioctl(fd, VIDIOC_QBUF, &buf);         // 重新入队
}
```

### 4.5 启停 stream

```c
enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
ioctl(fd, VIDIOC_STREAMON, &type);   // 开始——ISP 开始填充 buffer

// ... 采集循环 ...

ioctl(fd, VIDIOC_STREAMOFF, &type);  // 停止
```

---

## 5. 完整调用流程

```
初始化阶段 (start_ai_camera_v2 → v4l2_capture_init):

① open("/dev/video11")
② VIDIOC_QUERYCAP         → 确认是 rkisp 驱动
③ VIDIOC_S_FMT            → NV12, MPLANE, 640×480
④ VIDIOC_REQBUFS(4)       → 申请 4 个 DMA buffer
⑤ for i in 0..3:
     VIDIOC_QUERYBUF       → 拿到每个 buffer 的大小
     mmap                  → 建立用户态映射
⑥ for i in 0..3:
     VIDIOC_EXPBUF         → 预导出 fd[i]  ← 一次性操作
⑦ for i in 0..3:
     VIDIOC_QBUF           → 全部入队给 ISP 用
⑧ VIDIOC_STREAMON         → 启动采集


每帧循环 (_inference_loop_zero_copy):

⑨ VIDIOC_DQBUF            → 阻塞等 ISP 填完 → buf_idx
   取出预导出的 fd = exported_fd[buf_idx]
   
⑩ RGA: importbuffer_fd(isp_fd) → improcess → releasebuffer_handle
   源=isp_fd (NV12), 目标=arena_fd (RGB)

⑪ VIDIOC_QBUF             → 归还 buf_idx（立即！不等 NPU）

⑫ NPU 推理 → 画框 → LCD ... (全读 arena_fd，跟 isp_fd 无关)


清理阶段 (stop_ai_camera → v4l2_capture_deinit):

⑬ VIDIOC_STREAMOFF
⑭ for i in 0..3: munmap → close(fd[i])
⑮ close("/dev/video11")
```

---

## 6. 我的项目中使用

| 功能 | 文件 | 行号 |
|------|------|------|
| V4L2 封装 | [v4l2_capture.h](D:\echo\yolov5_demo\cpp\v4l2_capture.h) | 全部 |
| V4L2 实现 | [v4l2_capture.cc](D:\echo\yolov5_demo\cpp\v4l2_capture.cc) | `init`, `get_frame`, `put_frame`, `deinit` |
| 主循环调用 | [AIcamera_c_interface.cc:297](D:\echo\yolov5_demo\cpp\AIcamera_c_interface.cc#L297) | `v4l2_capture_get_frame` |
| 立即归还 | [AIcamera_c_interface.cc:309](D:\echo\yolov5_demo\cpp\AIcamera_c_interface.cc#L309) | `v4l2_capture_put_frame` |
| 设备号 | DTS 中 `/dev/video11` | rkisp 注册的 video 节点 |

---

## 7. 涉及源码

| 层 | 路径 | 关键函数 |
|----|------|---------|
| 用户态封装 | `v4l2_capture.cc` | `v4l2_capture_init`, `v4l2_capture_get_frame`, `v4l2_capture_put_frame` |
| V4L2 核心 | `drivers/media/v4l2-core/v4l2-ioctl.c` | `video_usercopy`, 所有 ioctl 入口 |
| vb2 队列 | `drivers/media/common/videobuf2/videobuf2-v4l2.c` | `vb2_ioctl_dqbuf`, `vb2_ioctl_qbuf`, `vb2_ioctl_expbuf` |
| vb2 DMA | `drivers/media/common/videobuf2/videobuf2-dma-contig.c` | `vb2_dc_get_dmabuf` — EXPBUF 的核心 |
| ISP 驱动 | `drivers/media/platform/rockchip/isp/` | rkisp 中断处理，buffer 填充完成回调 |
| CIF 驱动 | `drivers/media/platform/rockchip/cif/` | RKCIF MIPI 接收 |
| Sensor 驱动 | `drivers/media/i2c/sc3336.c` | I2C 控制 sensor streaming |
| dma-buf | `drivers/dma-buf/dma-buf.c` | `dma_buf_fd`, `dma_buf_put` |
| 设备树 | `rv1106-echo-mate-ipc.dtsi` | sensor→dphy→csi2→cif→isp 链路 |

---

## 8. 常见 Bug 及调试方法

### 8.1 EXPBUF 失败 (-ENOTTY / -EINVAL)

```
症状: ioctl(VIDIOC_EXPBUF) 返回 -1, errno=25 (ENOTTY)
原因: 内核没开 CONFIG_DMA_BUF 或者 vb2 用的不是 DMABUF memory model
检查: zcat /proc/config.gz | grep -E "DMA_BUF|VIDEOBUF2_DMA"
修复: 确认 vb2 用的是 V4L2_MEMORY_MMAP + vb2_dma_contig 后端
```

### 8.2 DQBUF 超时

```
症状: VIDIOC_DQBUF 永远阻塞
原因: ISP 没输出 → sensor 不在 streaming 状态 → MIPI 链路断了
调试:
  1. v4l2-ctl -d /dev/video11 --stream-mmap --stream-count=1 能否成功
  2. media-ctl -p 看 pipeline 链路是否完整
  3. cat /sys/kernel/debug/clk/clk_summary | grep mipi 确认时钟
```

### 8.3 buffer 耗尽

```
症状: get_frame 连续返回 -1
原因: QBUF 没调用 → buffer 都在用户态 → ISP 没 buffer 可填
调试:
  1. 确认每次 get 后都有 put
  2. start_ai_camera_v2 初始化时 QBUF 计数是否等于 REQBUFS count
```

### 8.4 用户态读 fd 数据是乱码

```
症状: ISP 输出看似正常但 RGA 转换后颜色偏差
原因: NV12 stride 不对齐 (width 不是 16 对齐时 Y/UV 偏移量计算错误)
修复: 
  fmt.fmt.pix_mp.plane_fmt[0].bytesperline = ALIGN(width, 16);
```

### 8.5 板端 debug 工具

```bash
# 查看 video 设备
v4l2-ctl --list-devices
# rkisp-statistics -> /dev/video11
# rkisp-params      -> /dev/video12

# 查看当前格式
v4l2-ctl -d /dev/video11 --get-fmt-video

# 抓一帧测试
v4l2-ctl -d /dev/video11 --stream-mmap --stream-count=1 --stream-to=frame.raw

# 查看 ISP 中断计数
cat /proc/interrupts | grep rkisp
```

---

## 9. 性能优化点

### 9.1 已做的优化

| 优化点 | 实现 |
|--------|------|
| EXPBUF 预导出 | 初始化时一次性导出 4 个 fd，不每帧调 ioctl |
| put_frame 最早归还 | RGA 完成后第 3 步就还，不等到循环末尾 |
| MPLANE 模式 | 单 plane NV12，减少 plane 数量开销 |
| buffer count=4 | 够用不浪费 CMA |

### 9.2 可做的优化

| 优化点 | 预期收益 | 难度 |
|--------|---------|------|
| MMAP 映射只用 EXPBUF，不读 virt_addr | 每帧省一次 cache flush | 低 |
| vb2 改用 DMABUF memory model（不 MMAP） | 少一次 mmap 映射 | 中 |
| 增加 buffer 超时重启机制 | 稳定性提升 | 低 |

---

## 10. 面试高频问题

1. **V4L2 视频采集的完整流程？open→querycap→s_fmt→reqbufs→querybuf→mmap→qbuf→streamon→dqbuf→qbuf→streamoff**

2. **MMAP vs USERPTR vs DMABUF 三种 memory model 区别？**

   | model | 拷贝次数 | fd 可用？ | 适用场景 |
   |-------|---------|----------|---------|
   | MMAP | 0 (驱动直接写这块物理内存) | 需 EXPBUF | 本地采集 |
   | USERPTR | 0 (用户提供 buffer 物理地址) | 需 EXPBUF | 用户自定义 allocator |
   | DMABUF | 0 (直接传 fd) | 天然支持 | 跨设备零拷贝 |

3. **EXPBUF 导出的 fd 是什么？** — dma-buf 框架的文件描述符。背后是 vb2_dma_contig 分配的连续物理内存，RGA/NPU 通过 fd 获取物理地址直接 DMA。

4. **为什么 RGA 完成就能立即归还 buffer？** — RGA 是同步阻塞调用，返回时硬件 DMA 已读完 isp_fd 的数据，arena_fd 上有独立副本。归还 isp_fd 不会影响 arena 的数据。

5. **MPLANE vs SINGLE_PLANE？** — MPLANE 用于多 plane 格式（如 NV12 的 Y 和 UV 分开存放），每个 plane 独立 mmap/expbuf。

6. **vb2_queue 的 buffer 生命周期？** — QUEUED(驱动可用) → ACTIVE(ISP 正在写) → DONE(写完了) → QUEUED(QBUF 重新入队)

7. **为什么 ISP 输出 NV12 而不是 RGB？** — ISP 原始输出是 Bayer RAW，去马赛克后 YUV 是自然产物。转 RGB 需要额外 CSC，交给 RGA 做。

8. **DQBUF 阻塞 vs 非阻塞？** — open 时 `O_NONBLOCK` 影响行为。阻塞时 DQBUF 等中断，非阻塞时立即返回 -EAGAIN。

9. **如何确认真正实现了零拷贝？** — 用 perf 看 `memcpy` 在帧循环中的占比，或者用 `strace` 看 `read/write` 调用是否出现在采集路径中。

10. **如果 ISP 驱动崩溃了怎么恢复？** — `VIDIOC_STREAMOFF → STREAMON` 重新初始化 pipeline。更严重的情况需要 close→open 重新打开设备。

---

## 11. 补充学习

- `Documentation/userspace-api/media/v4l/` — 官方 V4L2 User API 文档
- `Documentation/driver-api/media/v4l2-subdev.rst` — subdev (sensor) 框架
- `v4l2-ctl` + `media-ctl` 用户的调试神器
- 内核 `vb2_queue` 源码 `videobuf2-core.c` — 理解 buffer 状态机的最好入口
- Linux Media Controller Framework — `media-ctl -p` 看到的 pipeline 是如何构建的
- RKISP 驱动 (`drivers/media/platform/rockchip/isp/rkisp.c`) — ISP 3A 参数如何通过 video12 节点下发
