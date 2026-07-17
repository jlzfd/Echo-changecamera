#ifndef _RKNN_DEMO_KWS_DETECTOR_H_
#define _RKNN_DEMO_KWS_DETECTOR_H_

#define RV1106_1103
#include "rknn_api.h"
#include "common.h"

#include <stdint.h>
#include <vector>

// KWS audio parameters (matching AudioProcess config)
#define KWS_SAMPLE_RATE      16000
#define KWS_FRAME_MS         40
#define KWS_FRAME_SAMPLES    (KWS_SAMPLE_RATE * KWS_FRAME_MS / 1000)  // 640

// Model input: stacked frames for ~1s context window
#define KWS_BUFFER_FRAMES    25     // 25 × 40ms = 1000ms
#define KWS_BUFFER_SAMPLES   (KWS_FRAME_SAMPLES * KWS_BUFFER_FRAMES) // 16000
#define KWS_STRIDE_FRAMES    3      // slide window every 3 frames (~120ms)

#define KWS_NUM_CLASSES      2      // background + wake_word

typedef struct {
    rknn_context rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs;
    rknn_tensor_attr* output_attrs;
    rknn_tensor_mem* input_mems[1];
    rknn_tensor_mem* output_mems[1];
    int model_channel;
    int model_width;
    int model_height;
    bool is_quant;
} kws_app_context_t;

typedef struct {
    float prob;                     // wake word probability
    bool detected;                  // threshold exceeded
} kws_result_t;

// ── Main API ──────────────────────────────────────

int  init_kws_model(const char* model_path, kws_app_context_t* ctx);
int  release_kws_model(kws_app_context_t* ctx);

// feed one 40ms audio frame (640 samples int16_t) and optionally get result
void kws_feed_frame(kws_app_context_t* ctx, const int16_t* samples,
                    kws_result_t* result);

// reset internal audio buffer (on state transition)
void kws_reset_buffer(kws_app_context_t* ctx);

#endif
