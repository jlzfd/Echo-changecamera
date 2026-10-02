# RV1106 NPU 架构与项目实现说明

## 1. 文档范围

本文说明 Echo Mate 项目在 RV1106 上使用 NPU 的方式，重点覆盖：

- RKNN 模型的初始化、推理和释放；
- YOLOv5、人脸检测、关键词唤醒（KWS）三个模型如何共享 NPU；
- NPU DMA Arena 的内存复用机制；
- V4L2、RGA、DMA-BUF 与 NPU 之间的低拷贝图像链路；
- NPU 推理结果如何进入 AIChat Client 的状态机和云端视觉流程；
- 生命周期、安全约束、调试方法与当前局限。

本文以仓库当前代码为准。`yolov5_demo` 虽然沿用“demo”名称，但它实际上已经承担 `AIChat_demo/Client` 的边缘视觉与 NPU 运行时职责。

## 2. 硬件与软件栈

项目的 NPU 软件栈可以分为五层：

```text
AIChat Client 业务与状态机
        │
AIcamera C 接口 / KWS 检测器
        │
YOLOv5 / SCRFD Face / KWS 模型封装
        │
NPU DMA Arena + RKNN Runtime
        │
RV1106 RKNPU 驱动、DMA-BUF、CMA
```

相关组件：

| 组件 | 作用 | 主要位置 |
|---|---|---|
| RKNN Runtime | 创建模型上下文、绑定张量内存、执行推理 | `yolov5_demo/cpp/3rdparty/rknpu2` |
| YOLOv5 | 目标检测，当前主要用于识别人 | `rknpu2/yolov5_rv1106_1103.cc`、`postprocess.cc` |
| SCRFD Face | 人脸框与关键点检测 | `face_detect.cc` |
| KWS | 用 NPU 替代 Snowboy 完成唤醒词检测 | `kws_detector.cc`、`mfcc_extract.cc` |
| NPU Arena | 多模型共享输入、输出 DMA 张量 | `npu_memory_reuse.h/.cc` |
| V4L2 | 从摄像头取得 DMA-BUF 帧 | `v4l2_capture.h/.cc` |
| RGA | NV12/RGB/BGR/RGB565 转换与缩放 | `utils/image_utils.*` |
| AIcamera | 摄像头线程、多模型调度和业务事件上报 | `AIcamera_c_interface.h/.cc` |

## 3. 项目中的 NPU 模型

### 3.1 YOLOv5 目标检测

YOLOv5 的主要职责是对摄像头画面做目标检测。当前业务层通过 `is_person_detected()` 检查结果中是否包含 person；检测到人后调用：

```cpp
app->SubmitActiveVisionEvent("person_detected", 0.7f, 5000);
```

这会生成主动视觉事件，并通过客户端状态机进入视觉理解流程。检测框也会画到供 LCD 显示和后续帧缓存使用的 RGB 图像上。

主要接口：

```cpp
int init_yolov5_model(const char* model_path, rknn_app_context_t* app_ctx);
int inference_yolov5_model(rknn_app_context_t* app_ctx,
                           object_detect_result_list* results);
int release_yolov5_model(rknn_app_context_t* app_ctx);
```

初始化时执行 `rknn_init()`、查询输入输出张量属性、创建并绑定张量内存。推理时执行同步的 `rknn_run()`，随后读取输出 DMA 内存并完成量化解码、置信度过滤和 NMS。

### 3.2 SCRFD 人脸检测

人脸模型与 YOLO 使用相同的 `rknn_app_context_t` 结构，因此可以复用相同的 Arena 接口。当前实现解析三路量化输出：

- bounding box；
- confidence；
- landmark。

当检测到人脸时，摄像头线程计算最高置信度，并提交 `face_detected` 主动视觉事件。默认事件冷却时间为 8 秒。

人脸模型不是每帧都运行，而是由 `face_interval` 控制调度频率，以减少单核 NPU 的占用。

### 3.3 深入示例：一帧图像经过 YOLOv5 和 NPU 的完整流程

下面按真实调用顺序，跟踪摄像头的一帧图像如何最终变成 `person_detected` 事件。需要先区分两个概念：

- **NPU 内部流程**：从 `rknn_run()` 开始，由 RKNPU 驱动和已经编译进 `.rknn` 的计算图执行卷积、激活、特征融合和检测头计算；
- **NPU 外部流程**：图像采集、缩放和色彩转换、DMA 内存管理、输出解码、NMS、画框及业务事件，均由 V4L2、RGA 或 CPU 完成。

因此，完整链路并不是“所有工作都在 NPU 内完成”，而是：

```text
摄像头/ISP         RGA                  NPU                    CPU
   │                │                    │                      │
NV12 DMA-BUF ──→ resize + CSC ──→ YOLOv5 量化计算图 ──→ 解码 + NMS
                    │                    │                      │
                  RGB888          3 路 int8 输出          person 事件/画框
```

#### 阶段 A：离线模型准备

仓库中运行的是 `yolov5.rknn`，而不是直接运行 ONNX。通常在开发机上先使用 RKNN Toolkit 将 `yolov5s_relu.onnx` 转换成针对 RV1106 的 RKNN 模型。转换阶段会完成：

1. 读取 ONNX 计算图；
2. 对算子进行融合和图优化；
3. 根据校准数据把权重及中间张量量化为 int8；
4. 为 RV1106 支持的算子和张量布局生成设备执行图；
5. 固化输入输出节点、量化参数和模型权重，导出 `.rknn`。

这部分发生在设备运行前。当前仓库只保存了 ONNX/RKNN 文件和设备端 Runtime 代码，没有包含转换脚本及校准数据，因此具体的量化配置不能仅从本仓库确认。

#### 阶段 B：模型加载与 Runtime 建图

设备启动摄像头时，`start_ai_camera_v2()` 调用 `init_yolov5_model()`：

```cpp
rknn_init(&ctx, model_path, 0, 0, nullptr);
```

`rknn_init()` 将 `.rknn` 模型交给 RKNN Runtime，创建一个 `rknn_context`。该 context 是后续所有查询、内存绑定和执行操作的句柄。随后代码查询：

```cpp
RKNN_QUERY_IN_OUT_NUM
RKNN_QUERY_NATIVE_INPUT_ATTR
RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR
```

查询结果包括：

- 输入、输出数量；
- 张量维度和 native layout；
- `size` 与包含对齐填充的 `size_with_stride`；
- 数据类型；
- 非对称量化的 zero point `zp` 和 scale；
- 模型输入宽、高、通道数。

这里必须使用 runtime 返回的 `size_with_stride` 分配 DMA 内存，不能简单按 `width × height × channels` 猜测，因为 NPU native tensor 可能有行、通道或硬件对齐填充。

项目把输入声明为 `UINT8 + NHWC`：

```cpp
input_attrs[0].type = RKNN_TENSOR_UINT8;
input_attrs[0].fmt  = RKNN_TENSOR_NHWC;
```

代码注释说明，这样可以把输入归一化和量化融合到 NPU 侧；CPU/RGA 只需提供 RGB uint8 图像，不必先生成 int8 输入张量。

#### 阶段 C：张量内存进入共享 Arena

模型初始化函数最初会为 YOLO 创建一套私有输入/输出 `rknn_tensor_mem`。在所有常驻模型初始化完成后，摄像头模块再注册 YOLO 和 Face 的张量需求：

```text
register YOLO tensor sizes
register Face tensor sizes
allocate max(input), max(output[0..2])
adopt YOLO
adopt Face
```

`adopt YOLO` 做了三件关键事情：

1. 用 `rknn_set_io_mem()` 把共享 Arena 内存绑定到 YOLO context；
2. 将 `rknn_app_context_t::input_mems/output_mems` 指向共享内存；
3. 在旧内存已经解绑后销毁 YOLO 原来的私有 DMA 内存。

到这里，`app_ctx->input_mems[0]` 就不再是 YOLO 独占缓冲，而是 YOLO、Face、KWS 分时复用的 Arena 输入缓冲。

#### 阶段 D：摄像头帧直接进入 NPU 输入 DMA

每帧开始时，V4L2 从摄像头/ISP 获得一个 NV12 DMA-BUF fd：

```cpp
buf_idx = v4l2_capture_get_frame(&g_v4l2_cap, &isp_fd);
```

RGA 随后以 fd 为源、Arena 输入内存的 fd 为目标执行一次硬件转换：

```text
源：ISP fd，NV12，640×480
操作：resize + YUV→RGB 色彩空间转换
目标：Arena input fd，RGB888，YOLO 模型输入尺寸
```

目标描述符的地址和 fd 直接取自：

```cpp
rknn_tensor_mem* arena_in = npu_arena_input_mem(arena);
g_rga_dst.virt_addr = arena_in->virt_addr;
g_rga_dst.fd        = arena_in->fd;
```

这意味着 RGA 写完时，图像已经位于 NPU 即将读取的输入 DMA 内存中，不再执行一次 `memcpy`。代码随后调用 DMA sync，建立设备之间的内存可见性。

#### 阶段 E：锁定单核 NPU 并绑定 YOLO context

摄像头线程执行：

```cpp
npu_arena_lock(arena);
npu_arena_bind_yolov5(arena, &rknn_app_ctx);
inference_yolov5_model(&rknn_app_ctx, &od_results);
npu_arena_unlock(arena);
```

锁覆盖了 bind、run 和后处理。这样 KWS 所在线程不能在 YOLO 读取输出时把共享内存重新绑定到另一个 context。

`bind` 会把共享输入和三块共享输出分别通过 `rknn_set_io_mem()` 绑定到 YOLO context。若 Arena 的 `bound_ctx` 已经是 YOLO，则为 no-op。

#### 阶段 F：`rknn_run()` 内部发生什么

```cpp
rknn_run(app_ctx->rknn_ctx, nullptr);
```

这是进入 NPU 执行的边界。结合 YOLOv5 结构和该项目的三路输出，可以把 NPU 内部工作理解为：

```text
RGB uint8 输入
  ↓ NPU 输入转换/量化
量化特征提取 Backbone
  ↓
多尺度特征融合 Neck（FPN/PAN 类路径）
  ↓
三个检测 Head
  ├─ 小步长网格：偏向小目标
  ├─ 中步长网格：偏向中目标
  └─ 大步长网格：偏向大目标
  ↓
三路 native NHWC int8 输出写入 output_mems[0..2]
```

卷积、激活、特征图运算和检测头预测由 NPU 执行。每个网格位置、每个 anchor 输出 `5 + 80 = 85` 个预测量：

```text
tx, ty, tw, th, objectness, class_0 ... class_79
```

三个 head 分别使用三组 anchors：

```text
(10,13),  (16,30),  (33,23)
(30,61),  (62,45),  (59,119)
(116,90), (156,198), (373,326)
```

需要注意：Runtime 和驱动内部如何进一步拆分 tile、调度 MAC 阵列、使用片上 SRAM，以及具体融合了哪些算子，并没有由当前源码公开。除非结合 RKNN Toolkit 的模型分析报告或 RKNPU profiling 数据，否则不应把这些硬件细节写成已验证事实。

`rknn_run()` 是同步阻塞调用：返回时三路输出已经写入共享 `output_mems`。当前代码没有调用 `rknn_outputs_get()`，因为它使用预绑定的 zero-copy tensor memory，直接读取 `virt_addr`。

#### 阶段 G：CPU 解码量化输出

NPU 不直接返回最终的 `object_detect_result_list`。`post_process()` 在 CPU 上遍历三路输出，并从各输出张量属性计算：

```cpp
grid_h = output_attrs[i].dims[2];
grid_w = output_attrs[i].dims[1];
stride = model_input_height / grid_h;
```

在 RV1106 分支中，输出按 native NHWC 的网格连续布局访问。每个候选首先用量化域阈值做快速过滤：

```text
threshold_i8 = threshold / scale + zero_point
```

只有 objectness 达标的候选才进一步解析。int8 数值通过以下公式恢复到浮点：

```text
x_float = (x_int8 - zero_point) × scale
```

边界框解码公式与当前代码一致：

```text
center_x = (tx × 2 - 0.5 + grid_x) × stride
center_y = (ty × 2 - 0.5 + grid_y) × stride
width    = (tw × 2)² × anchor_width
height   = (th × 2)² × anchor_height

left = center_x - width / 2
top  = center_y - height / 2
```

对 80 个类别寻找最高类别分数，最终候选分数为：

```text
score = objectness × max_class_probability
```

本项目阈值为：

- 候选框置信度 `BOX_THRESH = 0.25`；
- NMS IoU 阈值 `NMS_THRESH = 0.45`；
- 最多输出 128 个对象。

#### 阶段 H：排序、分类别 NMS 和结果生成

所有尺度的候选合并后，CPU 按分数从高到低排序，然后针对每一个类别分别执行 NMS：

1. 保留当前分数最高的框；
2. 计算同类别其他框与它的 IoU；
3. 将 IoU 大于 0.45 的重叠框标记为无效；
4. 继续处理剩余框；
5. 将结果写入 `object_detect_result_list`。

例如一帧中三个 head 都对同一个人产生候选框，NMS 通常只保留其中置信度最高、定位最合理的一个。

#### 阶段 I：从检测结果进入产品业务

摄像头线程收到 `od_results` 后执行三类消费：

1. `is_person_detected()` 查找 person 类别；
2. 命中后调用 `SubmitActiveVisionEvent("person_detected", ...)`；
3. 用 OpenCV 将框画在 Arena RGB 图像上，并将画面转换给 LCD 和最新帧缓存。

主动视觉事件经过冷却和状态检查后进入应用事件队列：

```text
YOLO person
  → SubmitActiveVisionEvent
  → AppEvent::vision_detected
  → idle → thinking
  → 从 VisionFrameBuffer 取得最近帧
  → JPEG 编码
  → WebSocket 发送给 Server VisionService
  → 云端生成自然语言视觉描述
```

所以 YOLO 在这个项目中不是最终回答生成器，而是一个低延迟、低带宽的**边缘触发器**：NPU 决定“当前值得看”，云端视觉模型负责理解“具体看到了什么”。

#### 一帧数据的所有权总结

| 阶段 | 执行单元 | 输入 | 输出 |
|---|---|---|---|
| 摄像头采集 | ISP/V4L2 | 传感器数据 | NV12 DMA-BUF fd |
| 预处理 | RGA | NV12 fd | Arena RGB888 input fd |
| 模型计算 | NPU/RKNN | RGB uint8 tensor | 三路 native int8 tensor |
| 解码与 NMS | CPU | 三路 int8 输出 | 检测框、类别、置信度 |
| 画框/运动检测 | CPU + OpenCV | Arena RGB 图像 | 标注图像、运动事件 |
| LCD/帧缓存转换 | RGA | RGB fd | RGB565/BGR DMA buffer |
| 业务触发 | AIChat Client | person 检测结果 | 状态机视觉事件 |

### 3.4 KWS 关键词唤醒

KWS 在 `IdleState` 的工作线程中运行，用 NPU 模型替代了原有 Snowboy 路径。

当前数据流程为：

```text
16 kHz 单声道 PCM
  → 每帧 640 samples（40 ms）
  → 环形缓冲累计 1 秒（16000 samples）
  → 每约 120 ms 触发一次推理
  → 提取 40 × 98 MFCC
  → 归一化并量化为 int8
  → RKNN 推理
  → 对 background / wake word 两类执行 softmax
  → wake word 概率 > 0.8 时触发 wake_detected
```

KWS 的模型输入注释为 `NHWC [1, 40, 98, 1]`。唤醒成功后，`IdleState` 向事件队列投递 `AppEvent::wake_detected`，状态机离开空闲状态。

## 4. RKNN 模型通用生命周期

三个模型遵循相近的生命周期：

```text
rknn_init(model)
    ↓
rknn_query(IN_OUT_NUM)
    ↓
rknn_query(INPUT/OUTPUT_ATTR)
    ↓
rknn_create_mem()
    ↓
rknn_set_io_mem()
    ↓
循环：准备输入 → rknn_run() → 解析输出
    ↓
rknn_destroy_mem()
    ↓
rknn_destroy(ctx)
```

RV1106 的零拷贝输入要求代码将输入张量设置为：

```cpp
attr.type = RKNN_TENSOR_UINT8;
attr.fmt  = RKNN_TENSOR_NHWC;
```

模型输出采用 native output tensor 属性，从 `rknn_tensor_mem::virt_addr` 直接读取。量化模型根据张量属性中的 zero point 和 scale 反量化：

```text
float_value = (quantized_value - zero_point) × scale
```

## 5. 单核 NPU 与并发模型

### 5.1 为什么必须串行化

项目代码明确按“RV1106 只有一个 NPU 核，`rknn_run()` 同步阻塞”设计。模型来自两个业务线程：

- 摄像头线程：运行 YOLO 和人脸检测；
- `IdleState` 线程：运行 KWS。

因此，多线程不代表 NPU 可以并行执行。当前正确的执行单元是：

```text
lock Arena
  → 将共享张量绑定到目标模型 ctx
  → 写入或准备目标模型输入
  → rknn_run()（阻塞）
  → 读取输出
unlock Arena
```

Arena 内部使用 `pthread_mutex_t run_mutex`，用于把“模型切换 + 推理”整体串行化。不能只锁 `rknn_run()`，因为另一个线程可能在输入准备后、运行前重新绑定共享张量。

### 5.2 摄像头线程内的调度

每轮摄像头循环大致执行：

```text
V4L2 取得一帧
  → RGA 转换到 Arena 输入缓冲
  → YOLO（每帧，可配置关闭）
  → Face（按 face_interval 调度）
  → CPU 运动检测
  → 更新 Client 最新帧缓存
  → RGA 转换到 LCD RGB565 缓冲
  → 归还 V4L2 buffer
```

YOLO 和 Face 顺序使用同一块 Arena 输入图像。Face 调度器记录 YOLO/Face 帧数并周期性输出 FPS 信息。

## 6. NPU DMA Arena 内存复用

### 6.1 要解决的问题

若每个模型各自保留输入和输出 DMA 内存，YOLO、Face、KWS 的张量会同时占用 CMA/NPU DMA 内存。但单核 NPU 在同一时刻只执行一个模型，因此这些内存不需要同时被使用。

Arena 为每个输入/输出位置寻找所有注册模型中的最大尺寸，只分配一组共享缓冲：

```text
共享输入大小 = max(各模型 input[0].size_with_stride)
共享 output[i] 大小 = max(各模型 output[i].size_with_stride)
```

代码注释给出的目标收益约为节省 30% NPU DMA 内存；实际数值应以启动日志中的 `Arena: ... bytes, ... saved` 为准。

当前限制：

- 最多注册 4 个模型；
- 每个模型最多 1 个输入；
- 最多支持 3 个输出；
- 后注册模型的张量不能超过已经分配的 Arena；
- 不验证所有张量语义，只依赖各位置缓冲容量足够。

### 6.2 Arena 状态机

```text
ARENA_EMPTY
    │ register(model)
    ▼
ARENA_REGISTERED
    │ allocate(primary_ctx)
    ▼
ARENA_ALLOCATED
    │ adopt / bind / inference
    ▼
ARENA_DESTROYED
```

各阶段含义：

| 状态 | 含义 |
|---|---|
| `ARENA_EMPTY` | 对象已创建，尚未注册模型 |
| `ARENA_REGISTERED` | 已收集一个或多个模型的张量需求 |
| `ARENA_ALLOCATED` | 已按最大需求分配共享 DMA 缓冲，可绑定和运行 |
| `ARENA_DESTROYED` | 缓冲和互斥锁已经释放 |

标准调用顺序：

```cpp
npu_arena_t* arena = npu_arena_create();
npu_set_global_arena(arena);

npu_arena_register(arena, yolo_ctx, ...);
npu_arena_register(arena, face_ctx, ...);
npu_arena_allocate(arena, yolo_ctx);

npu_arena_adopt_yolov5(arena, &yolo_app_ctx);
npu_arena_adopt_yolov5(arena, &face_app_ctx);
```

KWS 通常在进入 Idle 状态时延迟加载。如果全局 Arena 已分配，KWS 会尝试晚注册并采用共享内存；如果 KWS 张量超过已有 Arena 容量，注册会失败，此时应保留模型自己的 DMA 内存作为回退路径。

### 6.3 register、allocate、adopt、bind 的区别

- `register`：只复制张量属性并统计最大尺寸，不分配内存。
- `allocate`：通过某个有效的 primary context 调用 `rknn_create_mem()`，一次性创建共享缓冲。
- `adopt`：先把 Arena 缓冲绑定到模型，再把模型上下文中的内存指针替换为 Arena 指针，最后销毁模型原有的独占缓冲。
- `bind`：模型切换前重新调用 `rknn_set_io_mem()`；如果当前已绑定到该 context，则直接返回。

`bound_ctx` 用于避免连续运行同一模型时重复绑定。

### 6.4 关键安全约束：先绑定新内存，再销毁旧内存

代码记录了 RV1106 内核侧的一个重要约束：对仍被 NPU I/O 绑定的内存直接执行 `rknn_destroy_mem()`，可能触发 `rknpu_mem_sync_ioctl` 空指针并导致 Kernel Oops。

因此 adopt 必须严格按照以下顺序：

```text
rknn_set_io_mem(ctx, arena_mem)   // 替换旧绑定
    ↓
更新 app_ctx 的内存指针
    ↓
rknn_destroy_mem(ctx, old_mem)    // 旧内存已解绑，可以释放
```

整个系统退出时则要求：

```text
停止所有推理线程
  → npu_arena_destroy()
  → release_yolov5_model()/release_face_detect_model()/release_kws_model()
  → rknn_destroy(ctx)
```

Arena 销毁前会通过保存的反向指针清空已 adopt 模型的 `input_mems` 和 `output_mems`，避免模型释放函数再次释放 Arena 内存。

## 7. 图像低拷贝链路

当前 `start_ai_camera_v2()` 启动的是集成式 DMA-BUF 路径：

```text
Camera / ISP
  │ NV12 DMA-BUF fd
  ▼
V4L2 capture
  │
  ▼
RGA：NV12 640×480 → RGB 模型尺寸
  │ 目标直接写入 Arena input_mem 的 fd
  ▼
共享 CMA / RKNN tensor memory
  │
  ▼
RKNN NPU 输入
```

这条链路避免了传统路径中的：

```text
摄像头 → CPU Mat → CPU resize/color convert → memcpy 到 NPU 输入
```

需要注意，“零拷贝”主要描述 ISP、RGA 和 NPU 之间通过 DMA-BUF fd 传递图像的主路径。项目仍会：

- 通过 `virt_addr` 创建 OpenCV `Mat`，用于画框和运动检测；
- 使用 RGA 将 RGB 转成 BGR 双缓冲，供 `Application::UpdateLatestFrame()` 使用；
- 使用 RGA 生成 LCD 所需的 RGB565 图像；
- 在上传视觉图片时进行 JPEG 编码，硬件编码失败会退回 CPU 编码。

因此它是“主推理输入低拷贝”，不是整个业务流程绝对零 CPU 访问。

`zero_copy_pipeline.h/.cc` 还包含一套独立 PoC 封装，但当前主摄像头 V2 路径直接在 `AIcamera_c_interface.cc` 中设置 V4L2、RGA 和 Arena，没有直接调用该 PoC 的 `zero_copy_process()`。

## 8. 缓存一致性

DMA 设备共享缓冲时必须考虑 CPU、RGA 和 NPU 看到的数据是否一致。当前代码在关键位置调用：

- `dma_sync_device_to_cpu(fd)`：设备写入后，使 CPU/NPU 消费侧看到最新数据；
- `dma_sync_cpu_to_device(fd)`：CPU 修改图像后，使后续设备读取最新内容。

项目注释认为 RV1106 CMA heap 通常为 uncached，因此某些同步可能是 no-op；但这不能作为删除同步调用的依据，因为 allocator、内核配置或平台变化后缓存属性可能不同。

## 9. 与客户端状态机的关系

NPU 并不是独立运行的后台模块，它与应用状态直接耦合：

### 9.1 KWS 驱动语音状态

```text
IdleState::Enter
  → 启动录音和 KWS 线程
  → KWS NPU 检测到唤醒词
  → eventQueue.Enqueue(wake_detected)
  → 状态机离开 idle
```

KWS 只在 Idle 状态消费音频。退出 Idle 时停止录音线程并重置 KWS 环形缓冲。

### 9.2 视觉模型驱动主动视觉

```text
YOLO person / Face / CPU motion 检测
  → SubmitActiveVisionEvent()
  → 冷却时间与状态检查
  → eventQueue.Enqueue(vision_detected)
  → idle → thinking
  → ThinkingState 提交当前帧到 Server VisionService
  → vision_result 返回 Client
```

这里有两类“视觉能力”：

- 本地 NPU 检测负责低成本、持续地判断是否发生值得关注的事件；
- 云端视觉模型负责对选中的当前帧做自然语言描述或问答。

这是一种“边缘触发、云端理解”的分工方式，可减少持续上传图像的带宽和云端调用成本。

## 10. 线程和资源所有权

| 资源 | 主要所有者 | 并发保护 |
|---|---|---|
| 摄像头推理线程 | `AIcamera_c_interface` | `running_mutex`、原子 stop/running 标志 |
| YOLO context | 摄像头模块全局上下文 | Arena run mutex |
| Face context | 摄像头模块全局上下文 | `face_model_mutex` + Arena run mutex |
| KWS context | `IdleState` 静态上下文 | Idle 工作线程 + Arena run mutex |
| Arena | 全局 singleton 指针 | Arena 内部 `pthread_mutex_t` |
| 最新视觉帧 | `Application` | `VisionFrameBuffer` 内部同步 |

摄像头模块支持 consumer attach/detach：若摄像头已经运行，新 `Application` 不重新创建摄像头线程，而是附着为消费者。只有拥有摄像头的 Application 才在析构/停止时关闭全局摄像头资源。

## 11. 诊断与性能观测

### 11.1 启动日志

建议重点检查：

```text
[Arena] registered ...
[Arena] allocated ... bytes DMA
[Arena] adopted ... saved=... bytes
[Arena] bound to ctx=...
[AI Camera V2] Arena: ... bytes, ... saved
[KWS] model loaded...
```

如果看不到 KWS 的 adopted 日志，需要检查晚注册是否因张量尺寸超过 Arena 而失败。

### 11.2 Arena profiling

定义 `ARENA_PROFILE` 后，可输出：

- 模型 bind 耗时；
- `rknn_run()` 耗时。

这有助于判断多模型切换成本，以及 Face/KWS 是否影响 YOLO 帧率。

### 11.3 独立验证程序

`yolov5_demo/cpp/CMakeLists.txt` 的 `BUILD_STANDALONE_TESTS` 可构建：

- `face_detect_test`；
- `dual_model_test`；
- `verify_test`；
- `unified_test`；
- `zero_copy_test`。

推荐验证顺序：单模型初始化与输出、双模型切换、Arena 复用、V4L2/RGA 链路、完整 Client 状态机。

## 12. 当前实现的风险与改进方向

### 12.1 Arena 错误处理需要增强

`start_ai_camera_v2()` 当前没有逐项检查 `npu_arena_create/register/allocate/adopt` 的返回值。若注册或分配失败，后续代码仍可能访问空的 Arena 输入内存。建议为每一步增加失败回滚。

KWS 晚注册时也应只在 `register` 和 `adopt` 均成功后设置 `g_kws_uses_arena = true`。

### 12.2 人脸模型动态卸载与 Arena slot

`unload_face_detect_model()` 会释放 RKNN context，但 Arena 中没有公开的 unregister 操作，slot 仍保留旧 context 元数据。之后再次加载新 Face context 会继续消耗 slot，最多可能触达 `MAX_SLOTS=4`。同时旧 slot 的 adopted 反向指针生命周期需要谨慎处理。

更稳妥的方案是：

- 增加 `npu_arena_unregister(ctx)`；或
- 规定运行期不动态卸载模型；或
- 重建整个 Arena 和所有模型 context。

### 12.3 共享输入的模型尺寸假设

Arena 只保证缓冲“足够大”，并不自动为每个模型执行不同尺寸、颜色格式或布局的预处理。当前摄像头 RGA 输出尺寸按 YOLO 输入设置；如果 Face 模型输入尺寸不同，必须确认 Face 推理前是否正确重排/缩放，否则共享同一图像缓冲并不等价于输入正确。

### 12.4 重复 bind

摄像头循环先显式调用 `npu_arena_bind_yolov5()`，模型的 `inference_*()` 内又会调用一次 bind。由于 `bound_ctx` 的 no-op 判断，通常没有实际重复绑定成本，但接口职责略显重复。可以统一规定由调度层或模型层中的一层负责 bind。

### 12.5 `zero_copy_pipeline` 与主路径重复

独立 PoC 和 `AIcamera_c_interface.cc` 的集成实现存在重复逻辑。长期建议保留一个经过验证的抽象，避免两套生命周期和缓存同步策略逐渐不一致。

### 12.6 全局单例和静态上下文

Arena、摄像头上下文和 KWS 缓冲大量使用全局/静态变量，适合单设备 demo，但不利于：

- 多摄像头；
- 多 Application 实例；
- 单元测试；
- 可靠的失败重试和局部重建。

后续可封装为显式的 `EdgeNpuRuntime` 对象，统一拥有模型、Arena、调度器和 DMA 资源。

## 13. 关键代码索引

| 内容 | 文件 |
|---|---|
| Arena API 与设计约束 | `yolov5_demo/cpp/npu_memory_reuse.h` |
| Arena 实现 | `yolov5_demo/cpp/npu_memory_reuse.cc` |
| 摄像头、多模型调度和业务事件 | `yolov5_demo/cpp/AIcamera_c_interface.cc` |
| YOLO RKNN 封装 | `yolov5_demo/cpp/rknpu2/yolov5_rv1106_1103.cc` |
| YOLO 后处理 | `yolov5_demo/cpp/postprocess.cc` |
| 人脸模型与后处理 | `yolov5_demo/cpp/face_detect.cc` |
| KWS RKNN 封装 | `yolov5_demo/cpp/kws_detector.cc` |
| MFCC | `yolov5_demo/cpp/mfcc_extract.cc` |
| V4L2 DMA-BUF | `yolov5_demo/cpp/v4l2_capture.cc` |
| 独立零拷贝 PoC | `yolov5_demo/cpp/zero_copy_pipeline.cc` |
| NPU 与 Idle 状态集成 | `AIChat_demo/Client/Application/UserStates/Idle.cc` |
| 主动视觉与状态机集成 | `AIChat_demo/Client/Application/Application.cc` |
| 构建配置 | `AIChat_demo/Client/CMakeLists.txt`、`yolov5_demo/cpp/CMakeLists.txt` |

## 14. 一句话总结

本项目利用 RV1106 NPU 执行 YOLO、人脸检测和 KWS：摄像头图像通过 V4L2 DMA-BUF 和 RGA 进入共享 NPU 输入内存，多模型通过互斥锁串行执行并复用一组按最大张量尺寸分配的 DMA Arena；本地模型负责持续、低成本地发现人物、人脸和唤醒词，再由 AIChat 状态机触发语音或云端视觉理解。


