// NPU-ISP Zero-Copy Pipeline — Phase 2 PoC
//
// Design: ISP DMA-BUF → RGA (hardware CSC+resize) → RKNN (fd import) → NPU inference
// Zero CPU memcpy for pixel data. CPU only orchestrates fd passing.
//
// Architecture:
//
//   Camera V4L2  ──VIDIOC_EXPBUF──►  NV12 dma-buf fd
//                                        │
//                     RGA importbuffer_fd(isp_fd)
//                     RGA imresize + imcvtcolor (NV12→RGB, 640x480→640x640)
//                                        │
//                     CMA buffer (fd shared RGA↔NPU)
//                                        │
//                     rknn_create_mem_from_fd(ctx, cma_fd, ...)
//                     rknn_set_io_mem(ctx, mem, &attr)
//                                        │
//                     rknn_run() ── NPU DMA reads from CMA
//
// Key constraint: RGA and NPU share the same CMA heap on RV1106.
// rknn_create_mem_from_fd() imports an external dma-buf fd into RKNN.
//
// PoC scope:
//   - Demonstrate rknn_create_mem_from_fd() works on RV1106
//   - Demonstrate RGA fd→fd processing path
//   - Full pipeline: test image → RGA → fd-backed NPU tensor → inference
//
//   V4L2 DMA-BUF export (VIDIOC_EXPBUF) is documented but requires
//   kernel driver verification — left as integration step.

#ifndef ZERO_COPY_PIPELINE_H_
#define ZERO_COPY_PIPELINE_H_

#include "rknn_api.h"
#include "utils/common.h"   // image_buffer_t
#include <stddef.h>         // size_t

#ifdef __cplusplus
extern "C" {
#endif

// ── Zero-copy pipeline context ─────────────────────

typedef struct {
    // CMA buffer shared between RGA (write) and NPU (read)
    void*   cma_virt_addr;       // CPU-mapped virtual address
    int     cma_fd;              // dma-buf fd (pass to RGA + RKNN)
    size_t  cma_size;            // buffer size in bytes

    // RKNN tensor memory imported from cma_fd
    rknn_tensor_mem* npu_input_mem;

    // RGA source/dest image descriptors
    image_buffer_t   rga_src;    // source image (NV12 from ISP or test)
    image_buffer_t   rga_dst;    // destination image (RGB, backed by CMA)

    // Dimensions
    int src_width;
    int src_height;
    int model_width;
    int model_height;
    int model_channels;

    // State
    bool initialized;
    bool npu_bound;
} zero_copy_ctx_t;

// ── Lifecycle ──────────────────────────────────────

// Initialize the zero-copy pipeline.
// model_ctx:      a valid rknn_context (model already loaded)
// input_attr:     the model's input tensor attributes
// src_w, src_h:   source image dimensions (e.g., 640x480)
// cma_heap:       CMA heap path (RV1106_CMA_HEAP_PATH or NULL for default)
int zero_copy_init(zero_copy_ctx_t* zc,
                   rknn_context model_ctx,
                   const rknn_tensor_attr* input_attr,
                   int src_w, int src_h,
                   const char* cma_heap);

// Process a frame through RGA → NPU, zero CPU copy.
// isp_fd:  dma-buf fd from V4L2 camera (or -1 to use CPU-filled test data)
// Returns 0 on success.
int zero_copy_process(zero_copy_ctx_t* zc, rknn_context model_ctx, int isp_fd);

// Bind the fd-backed tensor as NPU input (call before rknn_run).
int zero_copy_bind_to_npu(zero_copy_ctx_t* zc, rknn_context model_ctx,
                          const rknn_tensor_attr* input_attr);

// Get the CMA-backed image buffer for direct CPU access (debug/test only).
image_buffer_t* zero_copy_get_buffer(zero_copy_ctx_t* zc);

// Release all resources.
void zero_copy_destroy(zero_copy_ctx_t* zc);

#ifdef __cplusplus
}
#endif

#endif  // ZERO_COPY_PIPELINE_H_
