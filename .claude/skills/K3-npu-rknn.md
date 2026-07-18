# NPU RKNN 推理与 Arena 内存复用 — 知识卡片

## 1. 一句话概括

RV1106 内置 0.5T INT8 NPU，通过 RKNN API 加载 `.rknn` 模型执行神经网络推理。Arena 机制让多模型共享 DMA buffer，节省 29% 内存。

---

## 2. 为什么需要它

| | CPU 推理 | NPU 推理 |
|---|---|---|
| YOLOv5 640×640 | ~500ms+ | **~15ms** |
| KWS 关键词检测 | ~200ms | **~50ms** |
| 功耗 | 跑满 100% CPU @900MHz | NPU 独立工作，CPU 空闲 |
| 内存 | 需要额外 model buffer | 通过 Arena 共享 DMA |

没有 NPU，RV1106 单核 A7 根本跑不动实时检测。

---

## 3. Linux 底层原理

### 3.1 内核驱动架构

```
Userspace (librknnrt.so)
    │
    │ ioctl(/dev/rknpu)
    ↓
Kernel (drivers/rknpu/rknpu_drv.c)
    │
    ├─ rknn_create_mem   → dma_buf_alloc → CMA 物理连续内存
    ├─ rknn_set_io_mem   → ioctl 绑定 tensor → 更新 NPU 内部地址表
    ├─ rknn_run          → ioctl 提交推理任务 → NPU 硬件执行
    │                          │
    │                    ┌──────┴──────┐
    │                    │   NPU 硬件   │
    │                    │  MAC 阵列    │
    │                    │  卷积/全连接  │
    │                    │  0.5T @INT8  │
    │                    └──────┬──────┘
    │                           │ 完成中断
    ├─ rknn_destroy_mem  → dma_buf_free → CMA 回收
    │
    └─ 注意: rknn_destroy_mem 在已绑定的 ctx 上调会触发
       内核 NULL deref → Oops。必须先 rknn_set_io_mem 替换绑定，
       再 destroy。这就是 bind-before-destroy 原则。
```

### 3.2 INT8 量化原理

```
FP32 模型 (YOLOv5 原始):
  input:  float32, 640×640×3  = 4.9MB
  weight: float32, ~7M params  = 28MB
  output: float32

INT8 量化后 (RKNN 转换):
  input:  uint8, 640×640×3    = 1.2MB  (省 75%)
  weight: uint8 + scale/zero   = ~8MB   (省 71%)
  output: uint8 + dequant

量化公式:
  q = round(x / scale + zero_point)
  x ≈ (q - zero_point) × scale
```

---

## 4. 核心数据结构 / 关键 API

### 4.1 RKNN 上下文

```c
// 模型上下文 — 一个模型一个
typedef struct {
    rknn_context        rknn_ctx;       // RKNN 句柄
    rknn_input_output_num io_num;       // 输入输出 tensor 数量
    rknn_tensor_attr    input_attrs[1]; // 输入 tensor 属性 (fmt/type/size)
    rknn_tensor_attr    output_attrs[3];// 输出 tensor 属性
    rknn_tensor_mem    *input_mems[1];  // 输入 DMA buffer
    rknn_tensor_mem    *output_mems[3]; // 输出 DMA buffer
    int model_width;                    // 模型输入宽 (640)
    int model_height;                   // 模型输入高 (640)
} rknn_app_context_t;
```

### 4.2 核心 API 调用链

```c
// ① 初始化 — 加载模型文件
rknn_init(&ctx, model_data, size, RKNN_FLAG_COLLECT_PERF_MASK);

// ② 查询输入输出属性 — 知道 tensor 尺寸/格式
rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs, &n_input);
rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs, &n_output);

// ③ 分配 DMA buffer（Arena 模式共享）
rknn_tensor_mem *mem = rknn_create_mem(ctx, size);
// 内部: dma_buf_alloc → 分配 CMA 物理连续内存
// mem->virt_addr: CPU 可读写
// mem->fd:        dma-buf fd, RGA/NPU 硬件可用
// mem->phys_addr: NPU 内部使用的物理地址

// ④ 绑定 tensor — 告诉 NPU "用这块内存"
rknn_set_io_mem(ctx, mem, &attrs);
// ioctl → NPU 寄存器更新 tensor 地址映射

// ⑤ 推理 — 阻塞等 NPU 完成
rknn_run(ctx);
// ioctl → 提交任务 → 等 NPU 中断 → 返回
// 结果在 output_mems[0]->virt_addr 里

// ⑥ 销毁 — 必须先 unbind！
rknn_set_io_mem(ctx, new_mem, &attrs);  // 替换绑定
rknn_destroy_mem(ctx, old_mem);         // 安全销毁
```

---

## 5. Arena 内存复用完整流程

### 5.1 为什么需要 Arena

```
独立分配模式:
  YOLO:  input(3.7MB) + outputs(~500KB) = 4.2MB
  Face:  input(1.6MB) + outputs(~300KB) = 1.9MB
  KWS:   input(24KB)  + outputs(~1KB)   = 0.025MB
  总计: ~6.1MB

Arena 共享模式:
  取各模型 input 最大值:  3.7MB
  取各模型各 output 最大值合计: ~0.5MB
  总计: ~4.2MB → 省 1.9MB (31%)
```

### 5.2 状态机

```
ARENA_EMPTY ──→ ARENA_REGISTERED ──→ ARENA_ALLOCATED ──→ ARENA_DESTROYED
                   ↑ 注册各模型           ↑ 按 max 分配       ↑ NULL 所有指针
                   │ 只记尺寸             │ 一次性分配         │ rknn_destroy_mem
                   │ 不分配内存           │ DMA 共享            │ free(arena)
```

### 5.3 bind-before-destroy 策略

```c
// npu_memory_reuse.cc:321-344
// 先绑到 Arena 的共享 buffer，才释放模型自己的旧 buffer
arena_bind_locked(arena, ctx, &arena_input, arena_outputs); // ① bind 新
                                                              // ② swap 指针
rknn_destroy_mem(ctx, old_in);                               // ③ destroy 旧

// 如果反过来（先 destroy 再 bind）:
// destroy 在绑定了模型旧 buffer 的 ctx 上操作
// → 内核 ioctl 检查 ctx->mem == 被 destroy 的 mem
// → NULL deref → kernel Oops → 板端重启
```

### 5.4 线程安全

```c
// camera 线程 (YOLO/Face) vs IdleState 线程 (KWS)
npu_arena_lock(arena);
  npu_arena_bind_yolov5(arena, &ctx);  // rknn_set_io_mem 修改 NPU 地址映射
  inference_yolov5_model(&ctx, &res);  // rknn_run 读取 NPU 结果
npu_arena_unlock(arena);
```

---

## 6. 我的项目中使用

### 6.1 YOLO 物体检测

```cpp
// AIcamera_c_interface.cc:314-320
npu_arena_lock(arena);
npu_arena_bind_yolov5(arena, &rknn_app_ctx);  // 绑定 Arena → YOLO ctx
inference_yolov5_model(&rknn_app_ctx, &od_results);  // NPU 推理
npu_arena_unlock(arena);

if (is_person_detected(od_results)) {
    app->SubmitActiveVisionEvent("person_detected", 0.7f, 5000);
}
```

### 6.2 SCRFD 人脸检测

```cpp
// AIcamera_c_interface.cc:334-351 — 每 N 帧跑一次
if (sched_should_run_face(&sched)) {
    npu_arena_lock(arena);
    npu_arena_bind_yolov5(arena, &face_detect_ctx);  // 复用同一个 Arena
    inference_face_detect_model(&face_detect_ctx, &fd_results);
    npu_arena_unlock(arena);
    
    if (fd_results.count > 0) {
        app->SubmitActiveVisionEvent("face_detected", max_conf, 8000);
    }
}
```

### 6.3 KWS 唤醒词检测

```cpp
// Idle.cc:67-76 — IdleState 线程
if (g_kws_uses_arena && arena) npu_arena_lock(arena);
kws_feed_frame(&g_kws_ctx, data.data(), &kw_res);
if (g_kws_uses_arena && arena) npu_arena_unlock(arena);

if (kw_res.detected) {
    app->eventQueue_.Enqueue(AppEvent::wake_detected);
    break;
}
```

---

## 7. 涉及源码

| 层 | 路径 | 关键内容 |
|----|------|---------|
| 用户态 RKNN | `3rdparty/rknpu2/include/rknn_api.h` | `rknn_init`, `rknn_run`, `rknn_create_mem` |
| 用户态封装 | `npu_memory_reuse.h/cc` | Arena 生命周期 + bind-before-destroy |
| YOLO 推理 | `rknpu2/yolov5_rv1106_1103.cc` | `inference_yolov5_model` |
| Face 推理 | `face_detect.cc` | `inference_face_detect_model` |
| KWS 推理 | `kws_detector.cc` | `kws_feed_frame` |
| 内核驱动 | `drivers/rknpu/rknpu_drv.c` | NPU ioctl 入口 + 中断处理 |

---

## 8. 常见 Bug 及调试方法

### 8.1 rknn_init 失败

```
症状: init 返回 -1, "RKNN_ERR_MODEL_INVALID"
原因: .rknn 模型版本不匹配 (用 RKNN Toolkit 1.7 转的模型不能给 2.0 的 runtime 用)
修复: 统一 RKNN API 版本，用匹配的 toolkit 重新转换
```

### 8.2 rknn_run 返回 -1 但无日志

```
症状: run 失败，无错误信息
调试:
  1. dmesg | grep rknpu → 看内核有什么 error
  2. 确认 input tensor 格式: fmt=NHWC, type=UINT8
  3. 确认 input size 匹配: model_width×height×3
  4. /sys/kernel/debug/rknpu/status 查看 NPU 状态
```

### 8.3 Arena bind 后推理结果全是 0

```
症状: 输出 tensor 全零
原因: 忘记 bind 就直接 run, NPU 读到了上一个模型的 tensor 内存
      或 bind 了错误的 tensor attrs (output size 不匹配)
调试:
  printf("bound_ctx=%p current_ctx=%p\n", arena->bound_ctx, ctx);
  printf("input_size=%d arena_input_size=%zu\n", model_size, arena->max_input_size);
```

### 8.4 板端 kernel Oops (最重要!)

```
症状: 板端突然重启, dmesg 有 "Unable to handle kernel NULL pointer"
原因: rknn_destroy_mem 在已绑定的 ctx 上操作
      → 内核检查 ctx->mem == 被 destroy 的 mem → NULL deref
修复: 遵循 bind-before-destroy:
  ① rknn_set_io_mem(ctx, new_arena_mem)  // 先替换
  ② rknn_destroy_mem(ctx, old_mem)       // 再销毁
```

### 8.5 NPU 和 CPU cache 不一致

```
症状: NPU 输出写到了 CMA, CPU 读出来是旧数据
原因: NPU 不经过 CPU cache, CMA 内存需要显式 sync
修复:
  dma_sync_device_to_cpu(arena_in->fd);  // CPU 读之前
  // rknn_run 内部其实会做这个, 但显式调一次保证安全
```

---

## 9. 性能优化点

| 优化 | 说明 | 效果 |
|------|------|------|
| INT8 量化 | 模型精度 INT8，比 FP16 快 2-3x | 15ms vs ~40ms |
| Arena 共享 | 多模型共享 DMA，减少 alloc/free | 省 29% 内存 |
| bind no-op | 已绑定的 ctx 跳过 set_io_mem | 省 200-500μs |
| Face 跳帧 | 每 3 帧跑一次 Face | 省 12ms/帧 |
| rknn_set_io_mem 用 fd | 传入 dma-buf fd 而非 virt_addr | NPU 直接 DMA，更快 |
| 预创建 mem | 一次 alloc 多次复用 | 消除 alloc 延迟 |

---

## 10. 面试高频问题

1. **RV1106 NPU 算力多少？支持哪些精度？** 0.5T @INT8，也支持 FP16 但 INT8 更快。

2. **INT8 量化的原理？为什么能几乎不掉精度？** `q = x/scale + zp`，scale 逐层校准保证分布一致。

3. **rknn_create_mem vs rknn_create_mem_from_fd 区别？**
   - `rknn_create_mem`: 内部 dma_buf_alloc，返回 mem
   - `rknn_create_mem_from_fd`: 直接用已有 fd，零拷贝接入 CMA 管线

4. **bind-before-destroy 是什么？为什么必须？** 先 rknn_set_io_mem 替换绑定，再 rknn_destroy_mem 销毁旧 buffer。否则内核 NULL deref Oops。

5. **YOLO 640×640 推理 15ms，瓶颈在哪？** NPU MAC 阵列的卷积计算时间，带宽不是瓶颈（RV1106 DDR 带宽对 INT8 够用）。

6. **NPU 错误率怎么检测？** 对比 NPU 输出和 CPU reference 的 cosine similarity。板端跑 benchmark。

7. **三模型怎么不冲突？** Arena 只有一份 buffer，通过 mutex 串行访问，bind + run 是原子操作。

8. **KWS 为什么单独在线程？** 音频帧率 25fps (40ms/帧)，camera 线程 30fps——两个独立时钟，必须两个线程。

9. **NPU 能否和 RGA 并行？** RV1106 的 NPU 和 RGA 共享 DDR 带宽，串行就行。高端芯片 (RK3588) 有独立带宽可以并行。

---

## 11. 补充学习

- Rockchip RKNN Toolkit 文档: 模型转换 + 量化 + 精度验证
- `rknn-toolkit2/examples/tflite/yolov5/` — YOLOv5 转换例子
- 嵌入式 NPU 架构对比: Rockchip NPU vs Google EdgeTPU vs Intel Movidius
- INT8 量化深入: Per-tensor vs Per-channel, Symmetric vs Asymmetric
- `Documentation/devicetree/bindings/npu/rockchip,rk3568-rknpu.yaml`
