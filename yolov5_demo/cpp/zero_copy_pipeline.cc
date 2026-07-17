// NPU-ISP Zero-Copy Pipeline — Phase 2 PoC implementation.
// See zero_copy_pipeline.h for architecture overview.

#include "zero_copy_pipeline.h"
#include "dma_alloc.h"
#include "utils/image_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Helpers ────────────────────────────────────────

static size_t rgb_size(int w, int h, int c) {
    return (size_t)w * h * c;
}

static size_t nv12_size(int w, int h) {
    return (size_t)w * h * 3 / 2;
}

// ── Init ───────────────────────────────────────────

int zero_copy_init(zero_copy_ctx_t* zc,
                   rknn_context model_ctx,
                   const rknn_tensor_attr* input_attr,
                   int src_w, int src_h,
                   const char* cma_heap) {
    if (!zc || !model_ctx || !input_attr) return -1;
    memset(zc, 0, sizeof(*zc));

    zc->src_width  = src_w;
    zc->src_height = src_h;
    zc->model_width  = input_attr->dims[2];  // NHWC: dims[1]=H, dims[2]=W
    zc->model_height = input_attr->dims[1];
    zc->model_channels = input_attr->dims[3];

    const char* heap = cma_heap ? cma_heap : RV1106_CMA_HEAP_PATH;

    // 1. Allocate CMA buffer for the model input (RGB, model dimensions)
    //    This buffer will be shared: RGA writes to it, NPU reads from it.
    zc->cma_size = rgb_size(zc->model_width, zc->model_height,
                            zc->model_channels);
    int ret = dma_buf_alloc(heap, zc->cma_size, &zc->cma_fd,
                            &zc->cma_virt_addr);
    if (ret < 0) {
        fprintf(stderr, "[ZeroCopy] CMA alloc failed (size=%zu, heap=%s)\n",
                zc->cma_size, heap);
        return -1;
    }
    printf("[ZeroCopy] CMA buffer: fd=%d virt=%p size=%zu\n",
           zc->cma_fd, zc->cma_virt_addr, zc->cma_size);

    // 2. Create NPU tensor memory from the CMA fd.
    //    NPU will DMA directly from this buffer — no CPU copy.
    zc->npu_input_mem = rknn_create_mem_from_fd(model_ctx, zc->cma_fd,
                                                 zc->cma_virt_addr,
                                                 (uint32_t)zc->cma_size, 0);
    if (!zc->npu_input_mem) {
        fprintf(stderr, "[ZeroCopy] rknn_create_mem_from_fd failed\n");
        dma_buf_free(zc->cma_size, &zc->cma_fd, zc->cma_virt_addr);
        return -1;
    }
    printf("[ZeroCopy] NPU tensor imported from fd=%d (phys=0x%llx)\n",
           zc->cma_fd, (unsigned long long)zc->npu_input_mem->phys_addr);

    // 3. Set up RGA source descriptor (NV12, placeholder for ISP frames)
    zc->rga_src.width  = src_w;
    zc->rga_src.height = src_h;
    zc->rga_src.width_stride  = src_w;
    zc->rga_src.height_stride = src_h;
    zc->rga_src.format = IMAGE_FORMAT_YUV420SP_NV12;
    zc->rga_src.virt_addr = NULL;  // filled per-frame
    zc->rga_src.fd  = -1;          // filled per-frame
    zc->rga_src.size = nv12_size(src_w, src_h);

    // 4. Set up RGA destination descriptor (CMA-backed RGB buffer)
    zc->rga_dst.width  = zc->model_width;
    zc->rga_dst.height = zc->model_height;
    zc->rga_dst.width_stride  = zc->model_width;
    zc->rga_dst.height_stride = zc->model_height;
    zc->rga_dst.format = IMAGE_FORMAT_RGB888;
    zc->rga_dst.virt_addr = (unsigned char*)zc->cma_virt_addr;
    zc->rga_dst.fd  = zc->cma_fd;
    zc->rga_dst.size = zc->cma_size;

    zc->initialized = true;
    zc->npu_bound = false;

    printf("[ZeroCopy] pipeline initialized. src=%dx%d(NV12) → dst=%dx%d(RGB)\n",
           src_w, src_h, zc->model_width, zc->model_height);
    return 0;
}

// ── Bind fd-backed tensor to NPU ───────────────────

int zero_copy_bind_to_npu(zero_copy_ctx_t* zc, rknn_context model_ctx,
                          const rknn_tensor_attr* input_attr) {
    if (!zc || !zc->initialized || !model_ctx || !input_attr) return -1;

    // The input_attr must have fmt/type set for zero-copy
    rknn_tensor_attr attr;
    memcpy(&attr, input_attr, sizeof(attr));
    attr.type = RKNN_TENSOR_UINT8;
    attr.fmt  = RKNN_TENSOR_NHWC;

    int ret = rknn_set_io_mem(model_ctx, zc->npu_input_mem, &attr);
    if (ret < 0) {
        fprintf(stderr, "[ZeroCopy] rknn_set_io_mem (fd-backed) failed, ret=%d\n",
                ret);
        return -1;
    }

    zc->npu_bound = true;
    printf("[ZeroCopy] fd-backed tensor bound as NPU input\n");
    return 0;
}

// ── Process a frame (RGA → NPU, zero CPU copy) ─────

int zero_copy_process(zero_copy_ctx_t* zc, rknn_context model_ctx,
                      int isp_fd) {
    if (!zc || !zc->initialized || !model_ctx) return -1;

    // 1. RGA: resize + CSC (NV12→RGB) from ISP fd to CMA fd
    //    Both source and destination are fd-backed → RGA hardware DMA
    if (isp_fd >= 0) {
        // Real zero-copy path: ISP dma-buf fd → RGA → CMA dma-buf fd
        zc->rga_src.fd = isp_fd;
        zc->rga_src.virt_addr = NULL;  // RGA will use fd, not virt
    } else {
        // Test path: CPU fills CMA buffer directly (for PoC validation)
        // RGA not used in this path
        printf("[ZeroCopy] no ISP fd — skip RGA, expect CPU pre-fill\n");
        return 0;
    }

    // Use existing RGA wrapper: convert_image handles fd-based buffers
    int ret = convert_image(&zc->rga_src, &zc->rga_dst,
                            NULL, NULL, 0);
    if (ret != 0) {
        fprintf(stderr, "[ZeroCopy] RGA convert failed, ret=%d\n", ret);
        return -1;
    }

    // 2. Cache sync: ensure NPU sees RGA's writes
    //    On RV1106, CMA heap is uncached, so this is a no-op.
    //    On cached allocators, dma_sync_device_to_cpu may be needed.
    //    RKNN also provides rknn_mem_sync() for explicit cache control.
    dma_sync_device_to_cpu(zc->cma_fd);

    printf("[ZeroCopy] RGA processed: ISP fd=%d → CMA fd=%d\n",
           isp_fd, zc->cma_fd);
    return 0;
}

// ── Accessors ──────────────────────────────────────

image_buffer_t* zero_copy_get_buffer(zero_copy_ctx_t* zc) {
    if (!zc || !zc->initialized) return NULL;
    return &zc->rga_dst;
}

// ── Destroy ────────────────────────────────────────

void zero_copy_destroy(zero_copy_ctx_t* zc) {
    if (!zc) return;

    // rknn_destroy_mem with FROM_FD flag: won't close fd, just frees the struct
    if (zc->npu_input_mem && zc->npu_bound) {
        // Note: don't call rknn_destroy_mem on fd-backed mem while
        // the model context is still using it as I/O.
        // Best practice: rknn_destroy(ctx) first, then cleanup.
        // For now, just null the pointer — the CMA free below handles cleanup.
    }

    if (zc->cma_fd >= 0 && zc->cma_virt_addr) {
        dma_buf_free(zc->cma_size, &zc->cma_fd, zc->cma_virt_addr);
        zc->cma_fd = -1;
        zc->cma_virt_addr = NULL;
    }

    memset(zc, 0, sizeof(*zc));
    printf("[ZeroCopy] pipeline destroyed\n");
}
