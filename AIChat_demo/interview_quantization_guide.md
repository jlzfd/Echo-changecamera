# 混合精度量化知识手册

## 1. 为什么需要混合精度量化

### 问题背景

标准 INT8 量化是把模型所有层的 FP32 权重统一映射到 INT8，用同一套 scale/zp 参数。但这有个问题：

```
模型里有些层对精度极其敏感（比如第一层卷积、检测头）
     ↓ 统一量化后
敏感层精度损失大 → 整个模型 mAP 可能掉 3-5 个点
```

**混合精度量化的核心思想**：不是所有层都同等对待——敏感层保持 FP16/FP32，鲁棒层用 INT8 甚至 INT4。

---

## 2. 敏感度分析

### 逐层量化实验

用 rknn-toolkit2 的 `hybrid_quantization` 接口，逐层尝试量化：

```python
from rknn.api import RKNN

rknn = RKNN()
rknn.config(mean_values=[...], std_values=[...], target_platform='rv1106')

# 加载模型
rknn.load_onnx(model='yolov5s.onnx')

# build 时开启混合量化
rknn.build(
    do_quantization=True,
    quantized_dtype='asymmetric_quantized-u8',
    quantize_method='layer',              # 逐层量化，而非 channel 级
    quantized_algorithm='kl_divergence',  # KL 散度找最优 scale
    dataset='./calibration.txt'           # 校准数据集（500-1000 张图）
)
```

### 如何找出敏感层

rknn-toolkit2 的 `hybrid_quantization_step2` 接口可以输出每层的量化误差：

```python
# Step 1: 正常量化
ret = rknn.hybrid_quantization_step1(dataset='calibration.txt')

# Step 2: 分析每层量化前后的输出差异
# 返回每层的 cosine similarity 或 MSE 误差
ret = rknn.hybrid_quantization_step2(
    model_input='./model.onnx',
    data='./calibration.txt',
    model_quant='./model.quant.rknn',
    metric='cosine'          # cosine similarity 越小 = 越敏感
)

# 输出示例：
# Layer 0  (Conv_0):           cosine=0.9992  ← 误差很小，可量化
# Layer 1  (Conv_1):           cosine=0.9987
#   ...
# Layer 72 (Detect_Head):      cosine=0.8921  ← 误差大，敏感层！
# Layer 73 (Detect_Box):       cosine=0.8763  ← 必须保持 FP16
```

---

## 3. 混合量化策略

### 3.1 哪些层通常不适合 INT8

| 模型位置 | 原因 | 推荐精度 |
|---------|------|---------|
| 第一层卷积 | 输入 RGB 值范围 [0,255]，量化后信息损失大 | FP16 |
| 检测头（Detection Head） | 输出坐标/置信度，微小的量化误差会被放大 | FP16 |
| Softmax 层 | 指数运算对数值精度敏感 | FP16 |
| Anchor 解码层 | 涉及 exp/sigmoid 运算 | FP32 |
| 中间卷积层 | 权重分布相对均匀，对量化不敏感 | INT8 |

### 3.2 rknn 配置方式

```python
# 方式一：指定自定义量化层
rknn.build(
    do_quantization=True,
    quantized_dtype='asymmetric_quantized-u8',
    custom_quantization_layers=[
        'Conv_0',          # 第一层：FP16
        'Detect',          # 检测头：FP16
        # 其余层默认 INT8
    ]
)

# 方式二：用 hybrid_quantization 自动选择
ret = rknn.hybrid_quantization_step1(dataset='calibration.txt')
ret = rknn.hybrid_quantization_step2(...)  # 分析结果后手动选择
```

---

## 4. 精度验证

量化后必须在测试集上计算 mAP 对比：

```python
# 量化前后精度对比
# FP32 baseline:      mAP@0.5 = 55.2%
# INT8 uniform:       mAP@0.5 = 52.1%  ← 掉了 3.1%
# INT8 hybrid:        mAP@0.5 = 54.8%  ← 只掉 0.4%，可接受
```

**可接受标准**：混合量化后精度损失 < 1%，同时推理速度提升 2-4 倍。

---

## 5. 与你项目的结合点

### 你的后处理代码里已经用到了 zp/scale

`face_detect.cc` 中的 `process_face_output_i8` 和 YOLOv5 的 `process_i8_rv1106` 都依赖 `zp` 和 `scale` 做反量化：

```cpp
// 量化后的 int8 检测结果 → 还原为 float32
float confidence = deqnt_affine_to_f32(box_confidence_i8, zp, scale);

// 如果 zp/scale 是在混合量化下逐层计算的
// 那么每一层的 output_attrs[i].zp 和 output_attrs[i].scale 都是不同的
// 这就是为什么后处理要逐个读取 output_attrs
```

### 面试时的完整闭环

> "我不仅会用 .rknn 模型，还了解量化的完整流程。在转模型时做了敏感度分析，确保检测头和第一层卷积保持 FP16，其余层 INT8 混合量化。精度损失控制在 1% 以内，推理速度提升 3 倍。后处理里用 output_attrs 的逐层 zp/scale 做反量化，这部分代码是我自己写的。"

---

## 6. 常见面试追问

**Q: 为什么选 KL 散度而不是直接 min/max 校准？**
> KL 散度（Kullback-Leibler divergence）找的 scale 和 zp 能最大化保留原始分布的"形状"，尤其适合分类网络。对于检测网络，通常 KL 散度和 min/max 差距不大，但 KL 在存在长尾分布时更稳定。

**Q: 混合量和 QAT（量化感知训练）有什么区别？**
> 混合量化是后训练量化（PTQ），不改模型权重，只换精度。QAT 需要在训练中加入伪量化节点，重新训练。QAT 精度更高但成本也更重。我这个场景 PTQ 混精度足够。

**Q: RV1106 NPU 支持的精度有哪些？**
> INT8（主推）、INT16、FP16。RV1106 专为 INT8 优化，INT8 性能是 FP16 的 2-4 倍。
