// Face detection model wrapper for RV1106 NPU.
// Follows the same zero-copy RKNN pattern as YOLOv5.
// Compatible with RetinaFace / SCRFD / UltraFace rknn models.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

#include "face_detect.h"
#include "yolov5.h"
#include "common.h"
#include "image_utils.h"
#include "npu_memory_reuse.h"

// ── 量化换算工具 ────────────────────────────────
static inline float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale) {
    return ((float)qnt - (float)zp) * scale;
}

static inline int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale) {
    float dst = (f32 / scale) + zp;
    if (dst > 127) dst = 127;
    if (dst < -128) dst = -128;
    return (int8_t)dst;
}

static float clamp_f32(float val, float min, float max) {
    return val < min ? min : (val > max ? max : val);
}

// ── dump tensor info (调试用) ────────────────────
static void dump_tensor_attr(rknn_tensor_attr* attr) {
    printf("  face: index=%d name=%s dims=[%d,%d,%d,%d] fmt=%s type=%s "
           "zp=%d scale=%f size=%d\n",
           attr->index, attr->name,
           attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           get_format_string(attr->fmt), get_type_string(attr->type),
           attr->zp, attr->scale, attr->size);
}

// ── 人脸后处理：Scrfd 输出格式（三路独立输出）──────
// Scrfd 典型的 3 个输：（全部展平为 [16800, channel]）
//   output[0] = bbox [16800, 4] — dx, dy, dw, dh 偏移量
//   output[1] = conf [16800, 2] — bg_score, face_score
//   output[2] = lmk  [16800,10] — 5 点 landmark 偏移
//   16800 = 12800(stride8) + 3200(stride16) + 800(stride32)
//   每个 stride 内：2 anchor × grid_h × grid_w，按行优先排列

#define FACE_CONF_THRESHOLD 0.5f
#define FACE_NMS_THRESHOLD  0.4f

// stride 配置
static const int   scrfd_strides[]        = {8, 16, 32};
static const int   scrfd_grid_sizes[]     = {80, 40, 20};       // grid_h = grid_w = 640/stride
static const int   scrfd_anchor_counts[]  = {12800, 3200, 800}; // 2 * grid * grid
static const float scrfd_min_sizes[][2]   = {
    {16, 32},     // stride 8
    {64, 128},    // stride 16
    {256, 512}    // stride 32
};
static const int   scrfd_num_strides = 3;

// 解析 Scrfd 格式（平铺输出，三路独立）
static int process_scrfd_output(
    int8_t* bbox_data, int8_t* conf_data, int8_t* lmk_data,
    int32_t bbox_zp, float bbox_scale,
    int32_t conf_zp, float conf_scale,
    int32_t lmk_zp,  float lmk_scale,
    int img_w, int img_h,
    std::vector<float>& boxes,
    std::vector<float>& scores,
    std::vector<std::vector<float>>& landmarks_out)
{
    int valid = 0;
    int abs_idx = 0;

    for (int s = 0; s < scrfd_num_strides; s++) {
        int stride    = scrfd_strides[s];
        int grid      = scrfd_grid_sizes[s];
        int num_anch  = scrfd_anchor_counts[s];
        float min_s0  = scrfd_min_sizes[s][0];
        float min_s1  = scrfd_min_sizes[s][1];

        for (int a = 0; a < num_anch; a++, abs_idx++) {
            // ── 置信度：取 face_score（索引 1） ──────
            int8_t  conf_i8 = conf_data[abs_idx * 2 + 1];
            float   conf_f  = deqnt_affine_to_f32(conf_i8, conf_zp, conf_scale);
            if (conf_f < FACE_CONF_THRESHOLD) continue;

            // ── 计算 anchor 中心 ─────────────────
            int pos   = a / 2;           // 0..grid^2-1
            int sub_a = a % 2;           // 0 or 1
            int gy = pos / grid;
            int gx = pos % grid;
            float anchor_size = (sub_a == 0) ? min_s0 : min_s1;
            float cx = (gx + 0.5f) * stride;
            float cy = (gy + 0.5f) * stride;

            // ── bbox 解码：dx,dy 用 stride, dw,dh 用 anchor_size ─
            float dx = deqnt_affine_to_f32(bbox_data[abs_idx * 4 + 0], bbox_zp, bbox_scale);
            float dy = deqnt_affine_to_f32(bbox_data[abs_idx * 4 + 1], bbox_zp, bbox_scale);
            float dw = deqnt_affine_to_f32(bbox_data[abs_idx * 4 + 2], bbox_zp, bbox_scale);
            float dh = deqnt_affine_to_f32(bbox_data[abs_idx * 4 + 3], bbox_zp, bbox_scale);

            float box_cx = cx + dx * stride;
            float box_cy = cy + dy * stride;
            float box_w  = expf(dw) * anchor_size;
            float box_h  = expf(dh) * anchor_size;

            float x1 = box_cx - box_w * 0.5f;
            float y1 = box_cy - box_h * 0.5f;
            float x2 = box_cx + box_w * 0.5f;
            float y2 = box_cy + box_h * 0.5f;

            x1 = clamp_f32(x1, 0, img_w);
            y1 = clamp_f32(y1, 0, img_h);
            x2 = clamp_f32(x2, 0, img_w);
            y2 = clamp_f32(y2, 0, img_h);

            if (x2 - x1 < 4 || y2 - y1 < 4) continue;

            // ── landmark 解码 ─────────────────────
            std::vector<float> lmk(10);
            for (int k = 0; k < 5; k++) {
                float lx = deqnt_affine_to_f32(lmk_data[abs_idx * 10 + k * 2],     lmk_zp, lmk_scale);
                float ly = deqnt_affine_to_f32(lmk_data[abs_idx * 10 + k * 2 + 1], lmk_zp, lmk_scale);
                lmk[k * 2]     = cx + lx * stride;
                lmk[k * 2 + 1] = cy + ly * stride;
            }

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2);
            boxes.push_back(y2);
            scores.push_back(conf_f);
            landmarks_out.push_back(lmk);
            valid++;
        }
    }
    return valid;
}

// ── 简易 NMS（IoU-based） ─────────────────────────
static void face_nms(std::vector<float>& boxes,
                     std::vector<float>& scores,
                     std::vector<std::vector<float>>& landmarks,
                     float nms_thresh)
{
    std::vector<int> indices;
    for (size_t i = 0; i < scores.size(); i++) indices.push_back(i);

    // 按置信度降序排序
    for (size_t i = 0; i < indices.size(); i++) {
        for (size_t j = i + 1; j < indices.size(); j++) {
            if (scores[indices[j]] > scores[indices[i]]) {
                int tmp = indices[i];
                indices[i] = indices[j];
                indices[j] = tmp;
            }
        }
    }

    std::vector<bool> suppressed(scores.size(), false);

    for (size_t i = 0; i < indices.size(); i++) {
        int idx_i = indices[i];
        if (suppressed[idx_i]) continue;
        float x1_i = boxes[idx_i * 4], y1_i = boxes[idx_i * 4 + 1];
        float x2_i = boxes[idx_i * 4 + 2], y2_i = boxes[idx_i * 4 + 3];
        float area_i = (x2_i - x1_i) * (y2_i - y1_i);

        for (size_t j = i + 1; j < indices.size(); j++) {
            int idx_j = indices[j];
            if (suppressed[idx_j]) continue;
            float x1_j = boxes[idx_j * 4], y1_j = boxes[idx_j * 4 + 1];
            float x2_j = boxes[idx_j * 4 + 2], y2_j = boxes[idx_j * 4 + 3];

            float inter_x1 = (x1_i > x1_j) ? x1_i : x1_j;
            float inter_y1 = (y1_i > y1_j) ? y1_i : y1_j;
            float inter_x2 = (x2_i < x2_j) ? x2_i : x2_j;
            float inter_y2 = (y2_i < y2_j) ? y2_i : y2_j;

            float inter_w = inter_x2 - inter_x1;
            float inter_h = inter_y2 - inter_y1;
            if (inter_w <= 0 || inter_h <= 0) continue;

            float inter_area = inter_w * inter_h;
            float area_j = (x2_j - x1_j) * (y2_j - y1_j);
            float iou = inter_area / (area_i + area_j - inter_area);

            if (iou > nms_thresh) {
                suppressed[idx_j] = true;
            }
        }
    }

    // 原地整理: 保留未抑制的
    std::vector<float> kept_boxes, kept_scores;
    std::vector<std::vector<float>> kept_lmk;
    for (size_t i = 0; i < indices.size(); i++) {
        int idx = indices[i];
        if (!suppressed[idx]) {
            kept_boxes.push_back(boxes[idx * 4]);
            kept_boxes.push_back(boxes[idx * 4 + 1]);
            kept_boxes.push_back(boxes[idx * 4 + 2]);
            kept_boxes.push_back(boxes[idx * 4 + 3]);
            kept_scores.push_back(scores[idx]);
            kept_lmk.push_back(landmarks[idx]);
        }
    }
    boxes    = kept_boxes;
    scores   = kept_scores;
    landmarks = kept_lmk;
}

// ── 模型初始化 ───────────────────────────────────
int init_face_detect_model(const char* model_path, rknn_app_context_t* app_ctx)
{
    int ret;
    rknn_context ctx = 0;

    ret = rknn_init(&ctx, (char*)model_path, 0, 0, NULL);
    if (ret < 0) {
        printf("[FaceDetect] rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("[FaceDetect] rknn_query IN_OUT_NUM fail! ret=%d\n", ret);
        return -1;
    }
    printf("[FaceDetect] model: %d input(s), %d output(s)\n",
           io_num.n_input, io_num.n_output);

    // 查询输⼊属性
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i],
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("[FaceDetect] query input attr fail!\n");
            return -1;
        }
        dump_tensor_attr(&input_attrs[i]);
    }

    // 查询输出属性
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR,
                         &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("[FaceDetect] query output attr fail!\n");
            return -1;
        }
        dump_tensor_attr(&output_attrs[i]);
    }

    // 零拷贝：预分配 NPU 输⼊/输出内存
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt  = RKNN_TENSOR_NHWC;
    app_ctx->input_mems[0] = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
    ret = rknn_set_io_mem(ctx, app_ctx->input_mems[0], &input_attrs[0]);
    if (ret < 0) {
        printf("[FaceDetect] set input mem fail! ret=%d\n", ret);
        return -1;
    }

    for (uint32_t i = 0; i < io_num.n_output; i++) {
        app_ctx->output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(ctx, app_ctx->output_mems[i], &output_attrs[i]);
        if (ret < 0) {
            printf("[FaceDetect] set output mem fail! ret=%d\n", ret);
            return -1;
        }
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num   = io_num;
    app_ctx->is_quant = (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);

    app_ctx->input_attrs = (rknn_tensor_attr*)malloc(io_num.n_input
                                                     * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr*)malloc(io_num.n_output
                                                      * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height  = input_attrs[0].dims[2];
        app_ctx->model_width   = input_attrs[0].dims[3];
    } else {
        app_ctx->model_height  = input_attrs[0].dims[1];
        app_ctx->model_width   = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    printf("[FaceDetect] init done, input=%dx%dx%d\n",
           app_ctx->model_width, app_ctx->model_height, app_ctx->model_channel);

    return 0;
}

// ── 模型释放 ─────────────────────────────────────
int release_face_detect_model(rknn_app_context_t* app_ctx)
{
    // Destroy tensor mems BEFORE destroying context.
    // rknn_destroy_mem frees the rknn_tensor_mem* internally
    // (ALLOC_INSIDE flag set by rknn_create_mem).
    for (int i = 0; i < app_ctx->io_num.n_input; i++) {
        if (app_ctx->input_mems[i]) {
            rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->input_mems[i]);
            app_ctx->input_mems[i] = NULL;
        }
    }
    for (int i = 0; i < app_ctx->io_num.n_output; i++) {
        if (app_ctx->output_mems[i]) {
            rknn_destroy_mem(app_ctx->rknn_ctx, app_ctx->output_mems[i]);
            app_ctx->output_mems[i] = NULL;
        }
    }
    if (app_ctx->rknn_ctx != 0) {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    if (app_ctx->input_attrs)  { free(app_ctx->input_attrs);  app_ctx->input_attrs = NULL; }
    if (app_ctx->output_attrs) { free(app_ctx->output_attrs); app_ctx->output_attrs = NULL; }
    printf("[FaceDetect] model released.\n");
    return 0;
}

// ── 推理入口 ─────────────────────────────────────
int inference_face_detect_model(rknn_app_context_t* app_ctx,
                                face_detect_result_list* fd_results)
{
    fd_results->count = 0;

    // Bind arena buffers if global arena is active
    npu_arena_t* arena = npu_get_global_arena();
    if (arena) {
        npu_arena_bind_yolov5(arena, app_ctx);
    }

    int ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("[FaceDetect] rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    int img_w = app_ctx->model_width;
    int img_h = app_ctx->model_height;

    std::vector<float> all_boxes, all_scores;
    std::vector<std::vector<float>> all_landmarks;

    // Scrfd 三路独立输出，一次性处理全部 16800 个 anchor
    if (app_ctx->io_num.n_output == 3 && app_ctx->is_quant) {
        int8_t* bbox_data = (int8_t*)app_ctx->output_mems[0]->virt_addr;
        int8_t* conf_data = (int8_t*)app_ctx->output_mems[1]->virt_addr;
        int8_t* lmk_data  = (int8_t*)app_ctx->output_mems[2]->virt_addr;

        int32_t bbox_zp = app_ctx->output_attrs[0].zp;  float bbox_scale = app_ctx->output_attrs[0].scale;
        int32_t conf_zp = app_ctx->output_attrs[1].zp;  float conf_scale = app_ctx->output_attrs[1].scale;
        int32_t lmk_zp  = app_ctx->output_attrs[2].zp;  float lmk_scale  = app_ctx->output_attrs[2].scale;

        process_scrfd_output(bbox_data, conf_data, lmk_data,
                             bbox_zp, bbox_scale,
                             conf_zp, conf_scale,
                             lmk_zp,  lmk_scale,
                             img_w, img_h,
                             all_boxes, all_scores, all_landmarks);
    } else if (!app_ctx->is_quant) {
        printf("[FaceDetect] WARN: model not quantized, fp32 path not implemented\n");
    } else {
        printf("[FaceDetect] WARN: unexpected output count %d\n",
               app_ctx->io_num.n_output);
    }

    // NMS
    face_nms(all_boxes, all_scores, all_landmarks, FACE_NMS_THRESHOLD);

    // 填充结果
    int face_count = (int)(all_scores.size());
    if (face_count > MAX_FACE_COUNT) face_count = MAX_FACE_COUNT;

    for (int i = 0; i < face_count; i++) {
        fd_results->results[i].box.left   = (int)all_boxes[i * 4];
        fd_results->results[i].box.top    = (int)all_boxes[i * 4 + 1];
        fd_results->results[i].box.right  = (int)all_boxes[i * 4 + 2];
        fd_results->results[i].box.bottom = (int)all_boxes[i * 4 + 3];
        fd_results->results[i].confidence = all_scores[i];
        for (int k = 0; k < 5; k++) {
            fd_results->results[i].landmarks[k][0] = all_landmarks[i][k * 2];
            fd_results->results[i].landmarks[k][1] = all_landmarks[i][k * 2 + 1];
        }
    }
    fd_results->count = face_count;

    return 0;
}
