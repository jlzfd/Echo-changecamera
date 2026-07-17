// NPU-based Keyword Spotting detector for RV1106.
// Replaces snowboy with ONNX→RKNN converted KWS model.
// Model input: MFCC features int8 NHWC [1,40,98,1]
// Audio feed: 16kHz mono int16_t, 640 samples/frame (40ms).
// Buffers 1s of audio, extracts MFCC, runs inference every ~120ms.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kws_detector.h"
#include "mfcc_extract.h"
#include "npu_memory_reuse.h"

// ── internal ring buffer ───────────────────────────
typedef struct {
    int16_t samples[KWS_BUFFER_SAMPLES];
    int     write_pos;
    int     count;
    int     stride_counter;
} kws_audio_buffer_t;

static kws_audio_buffer_t g_audio_buf;

// ── model init ─────────────────────────────────────
int init_kws_model(const char* model_path, kws_app_context_t* ctx) {
    int ret;
    rknn_context rk_ctx = 0;

    ret = rknn_init(&rk_ctx, (char*)model_path, 0, 0, NULL);
    if (ret < 0) { printf("[KWS] rknn_init fail! ret=%d\n", ret); return -1; }

    rknn_input_output_num io_num;
    ret = rknn_query(rk_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) { printf("[KWS] query IN_OUT_NUM fail!\n"); return -1; }
    printf("[KWS] %d input(s), %d output(s)\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr in_attrs[io_num.n_input];
    rknn_tensor_attr out_attrs[io_num.n_output];
    memset(in_attrs,  0, sizeof(in_attrs));
    memset(out_attrs, 0, sizeof(out_attrs));

    for (int i = 0; i < io_num.n_input; i++) {
        in_attrs[i].index = i;
        ret = rknn_query(rk_ctx, RKNN_QUERY_INPUT_ATTR, &in_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) { printf("[KWS] query input attr fail!\n"); return -1; }
        printf("[KWS]  input[%d] dims=[%d,%d,%d,%d] fmt=%d type=%d size=%d\n", i,
               in_attrs[i].dims[0], in_attrs[i].dims[1],
               in_attrs[i].dims[2], in_attrs[i].dims[3],
               in_attrs[i].fmt, in_attrs[i].type, in_attrs[i].size);
    }
    for (int i = 0; i < io_num.n_output; i++) {
        out_attrs[i].index = i;
        ret = rknn_query(rk_ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &out_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) { printf("[KWS] query output attr fail!\n"); return -1; }
        printf("[KWS]  output[%d] dims=[%d,%d,%d,%d] size=%d\n", i,
               out_attrs[i].dims[0], out_attrs[i].dims[1],
               out_attrs[i].dims[2], out_attrs[i].dims[3], out_attrs[i].size);
    }

    // zero-copy: allocate NPU memory
    in_attrs[0].type = RKNN_TENSOR_UINT8;
    in_attrs[0].fmt  = RKNN_TENSOR_NHWC;
    ctx->input_mems[0] = rknn_create_mem(rk_ctx, in_attrs[0].size_with_stride);
    ret = rknn_set_io_mem(rk_ctx, ctx->input_mems[0], &in_attrs[0]);
    if (ret < 0) { printf("[KWS] set input mem fail!\n"); return -1; }

    for (uint32_t i = 0; i < io_num.n_output; i++) {
        ctx->output_mems[i] = rknn_create_mem(rk_ctx, out_attrs[i].size_with_stride);
        ret = rknn_set_io_mem(rk_ctx, ctx->output_mems[i], &out_attrs[i]);
        if (ret < 0) { printf("[KWS] set output mem fail!\n"); return -1; }
    }

    ctx->rknn_ctx = rk_ctx;
    ctx->io_num   = io_num;
    ctx->is_quant = (out_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC);

    ctx->input_attrs  = (rknn_tensor_attr*)malloc(io_num.n_input  * sizeof(rknn_tensor_attr));
    ctx->output_attrs = (rknn_tensor_attr*)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(ctx->input_attrs,  in_attrs,  io_num.n_input  * sizeof(rknn_tensor_attr));
    memcpy(ctx->output_attrs, out_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (in_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        ctx->model_channel = in_attrs[0].dims[1];
        ctx->model_height  = in_attrs[0].dims[2];
        ctx->model_width   = in_attrs[0].dims[3];
    } else {
        ctx->model_height  = in_attrs[0].dims[1];
        ctx->model_width   = in_attrs[0].dims[2];
        ctx->model_channel = in_attrs[0].dims[3];
    }
    printf("[KWS] init done, input NHWC=%dx%dx%d\n",
           ctx->model_width, ctx->model_height, ctx->model_channel);

    kws_reset_buffer(ctx);
    return 0;
}

int release_kws_model(kws_app_context_t* ctx) {
    // Destroy tensor mems BEFORE destroying context.
    // rknn_destroy_mem frees the rknn_tensor_mem* internally
    // (ALLOC_INSIDE flag set by rknn_create_mem).
    for (int i = 0; i < ctx->io_num.n_input; i++) {
        if (ctx->input_mems[i]) { rknn_destroy_mem(ctx->rknn_ctx, ctx->input_mems[i]); ctx->input_mems[i] = NULL; }
    }
    for (int i = 0; i < ctx->io_num.n_output; i++) {
        if (ctx->output_mems[i]) { rknn_destroy_mem(ctx->rknn_ctx, ctx->output_mems[i]); ctx->output_mems[i] = NULL; }
    }
    if (ctx->rknn_ctx != 0) { rknn_destroy(ctx->rknn_ctx); ctx->rknn_ctx = 0; }
    if (ctx->input_attrs)   { free(ctx->input_attrs);   ctx->input_attrs  = NULL; }
    if (ctx->output_attrs)  { free(ctx->output_attrs);  ctx->output_attrs = NULL; }
    printf("[KWS] model released.\n");
    return 0;
}

void kws_reset_buffer(kws_app_context_t* ctx) {
    (void)ctx;
    memset(&g_audio_buf, 0, sizeof(g_audio_buf));
}

// ── feed audio frame + inference ───────────────────
void kws_feed_frame(kws_app_context_t* ctx, const int16_t* samples,
                    kws_result_t* result)
{
    result->prob = 0;
    result->detected = false;

    // copy 640 new samples into ring buffer (write_pos circular)
    for (int i = 0; i < KWS_FRAME_SAMPLES; i++) {
        g_audio_buf.samples[g_audio_buf.write_pos] = samples[i];
        g_audio_buf.write_pos = (g_audio_buf.write_pos + 1) % KWS_BUFFER_SAMPLES;
    }
    g_audio_buf.count += KWS_FRAME_SAMPLES;
    g_audio_buf.stride_counter++;

    // run inference every N frames to reduce CPU/NPU overhead
    if (g_audio_buf.stride_counter < KWS_STRIDE_FRAMES) return;
    g_audio_buf.stride_counter = 0;

    // need at least 1 full second of audio before first inference
    if (g_audio_buf.count < KWS_BUFFER_SAMPLES) return;

    // read ring buffer in chronological order
    int16_t linear_buf[KWS_BUFFER_SAMPLES];
    for (int i = 0; i < KWS_BUFFER_SAMPLES; i++) {
        linear_buf[i] = g_audio_buf.samples[(g_audio_buf.write_pos + i) % KWS_BUFFER_SAMPLES];
    }

    // MFCC: 16000 samples int16 → 40×98 float → normalize → int8
    float mfcc_float[MFCC_N_MFCC * MFCC_N_FRAMES];  // 40 × 98 = 3920
    mfcc_extract(linear_buf, KWS_BUFFER_SAMPLES, mfcc_float);
    mfcc_normalize(mfcc_float, MFCC_N_MFCC * MFCC_N_FRAMES);

    int8_t* input = (int8_t*)ctx->input_mems[0]->virt_addr;
    int input_size = ctx->input_attrs[0].size;
    mfcc_quantize_int8(mfcc_float, MFCC_N_MFCC * MFCC_N_FRAMES, input);

    // Bind arena buffers if global arena is active
    npu_arena_t* arena = npu_get_global_arena();
    if (arena) {
        npu_arena_bind_kws(arena, ctx);
    }

    int ret = rknn_run(ctx->rknn_ctx, nullptr);
    if (ret < 0) { printf("[KWS] rknn_run fail! ret=%d\n", ret); return; }

    // parse output: class 0 = background, class 1 = wake word
    if (ctx->is_quant) {
        int8_t* out_i8 = (int8_t*)ctx->output_mems[0]->virt_addr;
        int32_t out_zp = ctx->output_attrs[0].zp;
        float   out_sc = ctx->output_attrs[0].scale;

        float probs[KWS_NUM_CLASSES] = {0};
        for (int i = 0; i < KWS_NUM_CLASSES; i++) {
            probs[i] = ((float)out_i8[i] - (float)out_zp) * out_sc;
        }
        // softmax
        float max_p = probs[0];
        for (int i = 1; i < KWS_NUM_CLASSES; i++)
            if (probs[i] > max_p) max_p = probs[i];
        float sum = 0;
        for (int i = 0; i < KWS_NUM_CLASSES; i++)
            sum += expf(probs[i] - max_p);
        result->prob = expf(probs[1] - max_p) / sum;
    } else {
        float* out_f32 = (float*)ctx->output_mems[0]->virt_addr;
        float probs[KWS_NUM_CLASSES];
        memcpy(probs, out_f32, KWS_NUM_CLASSES * sizeof(float));
        float max_p = probs[0];
        for (int i = 1; i < KWS_NUM_CLASSES; i++)
            if (probs[i] > max_p) max_p = probs[i];
        float sum = 0;
        for (int i = 0; i < KWS_NUM_CLASSES; i++)
            sum += expf(probs[i] - max_p);
        result->prob = expf(probs[1] - max_p) / sum;
    }

    result->detected = (result->prob > 0.8f);
    if (result->detected) {
        printf("[KWS] Wake word detected! prob=%.3f\n", result->prob);
    }
}
