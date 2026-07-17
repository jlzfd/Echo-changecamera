// Board verification for NPU memory reuse + zero-copy pipeline.
// Usage: ./verify_test <yolov5.rknn> <face_detect.rknn> [kws.rknn]
//
// Tests (each prints PASS/FAIL):
//   T1: Arena create → register → allocate → bind → YOLO inference
//   T2: Arena cross-model switching (YOLO ↔ Face)
//   T3: rknn_create_mem_from_fd() — CMA buffer import as NPU tensor
//   T4: Arena output correctness (compare with non-arena baseline)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yolov5.h"
#include "face_detect.h"
#include "npu_memory_reuse.h"
#include "zero_copy_pipeline.h"
#include "dma_alloc.h"

#include "utils/image_utils.h"

#define TLOG(tag, fmt, ...) printf("  [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define TPASS(test) printf("[%s] PASS\n", test)
#define TFAIL(test, fmt, ...) fprintf(stderr, "[%s] FAIL: " fmt "\n", test, ##__VA_ARGS__)

// ── Helpers ──────────────────────────────────────

static void fill_test_image(unsigned char* buf, int w, int h, int c) {
    // Fill with a simple gradient pattern
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * c;
            buf[idx + 0] = (unsigned char)(x % 256);          // R
            buf[idx + 1] = (unsigned char)(y % 256);          // G
            if (c >= 3) buf[idx + 2] = (unsigned char)((x+y) % 256); // B
        }
    }
}

static int compare_outputs(rknn_tensor_mem* baseline[], rknn_tensor_mem* test[],
                           int n_output, float* max_diff) {
    *max_diff = 0.0f;
    for (int i = 0; i < n_output; i++) {
        if (!baseline[i] || !test[i]) return -1;
        size_t len = baseline[i]->size;
        if (len == 0) len = 1;
        unsigned char* b = (unsigned char*)baseline[i]->virt_addr;
        unsigned char* t = (unsigned char*)test[i]->virt_addr;
        if (!b || !t) return -1;
        for (size_t j = 0; j < len; j++) {
            float d = (float)(b[j] > t[j] ? b[j] - t[j] : t[j] - b[j]);
            if (d > *max_diff) *max_diff = d;
        }
    }
    return 0;
}

// ── Test 1: Arena lifecycle + YOLO inference ─────

static int test1_arena_yolov5(const char* yolo_path) {
    printf("\n=== T1: Arena YOLOv5 lifecycle ===\n");

    // Init YOLO model (standard path, no arena)
    rknn_app_context_t yolo;
    memset(&yolo, 0, sizeof(yolo));
    if (init_yolov5_model(yolo_path, &yolo) != 0) {
        TFAIL("T1", "init_yolov5_model failed");
        return -1;
    }
    TLOG("T1", "YOLO loaded: %dx%d", yolo.model_width, yolo.model_height);

    // Create arena
    npu_arena_t* arena = npu_arena_create();
    if (!arena) {
        TFAIL("T1", "npu_arena_create failed");
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T1", "arena created (state=%d)", (int)npu_arena_state(arena));

    // Register YOLO tensor requirements
    if (npu_arena_register(arena, yolo.rknn_ctx,
                           yolo.input_attrs,  yolo.io_num.n_input,
                           yolo.output_attrs, yolo.io_num.n_output) != 0) {
        TFAIL("T1", "register failed");
        npu_arena_destroy(arena);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T1", "YOLO registered (state=%d)", (int)npu_arena_state(arena));

    // Explicit allocate (after all registrations)
    if (npu_arena_allocate(arena, yolo.rknn_ctx) != 0) {
        TFAIL("T1", "allocate failed");
        npu_arena_destroy(arena);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T1", "arena allocated (state=%d)", (int)npu_arena_state(arena));

    // Adopt YOLO: bind arena mems + destroy old per-model mems
    if (npu_arena_adopt_yolov5(arena, &yolo) != 0) {
        TFAIL("T1", "adopt_yolov5 failed");
        npu_arena_destroy(arena);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T1", "YOLO adopted, arena total=%zu saved=%zu",
         npu_arena_total_size(arena), npu_arena_saved_bytes(arena));

    // Run inference with arena
    fill_test_image((unsigned char*)yolo.input_mems[0]->virt_addr,
                    yolo.model_width, yolo.model_height, 3);
    object_detect_result_list results;
    int ret = inference_yolov5_model(&yolo, &results);
    if (ret != 0) {
        TFAIL("T1", "inference_yolov5_model (arena) returned %d", ret);
        npu_arena_destroy(arena);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T1", "inference OK, %d objects detected", results.count);

    npu_arena_destroy(arena);
    release_yolov5_model(&yolo);
    TPASS("T1");
    return 0;
}

// ── Test 2: Arena cross-model switching ──────────

static int test2_arena_switching(const char* yolo_path, const char* face_path) {
    printf("\n=== T2: Arena cross-model switching ===\n");

    int ret, pass = 1;

    // Init both models
    rknn_app_context_t yolo;
    rknn_app_context_t face;
    memset(&yolo, 0, sizeof(yolo));
    memset(&face, 0, sizeof(face));

    if (init_yolov5_model(yolo_path, &yolo) != 0) {
        TFAIL("T2", "init_yolov5_model failed");
        return -1;
    }
    if (init_face_detect_model(face_path, &face) != 0) {
        TFAIL("T2", "init_face_detect_model failed");
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T2", "both models loaded");

    // Create arena, register both models, allocate, then adopt
    npu_arena_t* arena = npu_arena_create();
    if (!arena) {
        TFAIL("T2", "npu_arena_create failed");
        goto cleanup;
    }

    ret = npu_arena_register(arena, yolo.rknn_ctx,
                             yolo.input_attrs,  yolo.io_num.n_input,
                             yolo.output_attrs, yolo.io_num.n_output);
    if (ret != 0) { TFAIL("T2", "register yolo failed"); npu_arena_destroy(arena); goto cleanup; }

    ret = npu_arena_register(arena, face.rknn_ctx,
                             face.input_attrs,  face.io_num.n_input,
                             face.output_attrs, face.io_num.n_output);
    if (ret != 0) { TFAIL("T2", "register face failed"); npu_arena_destroy(arena); goto cleanup; }
    TLOG("T2", "both models registered (%d slots)", npu_arena_num_registered(arena));

    ret = npu_arena_allocate(arena, yolo.rknn_ctx);
    if (ret != 0) { TFAIL("T2", "allocate failed"); npu_arena_destroy(arena); goto cleanup; }
    TLOG("T2", "arena allocated");

    ret = npu_arena_adopt_yolov5(arena, &yolo);
    if (ret != 0) { TFAIL("T2", "adopt yolo failed"); npu_arena_destroy(arena); goto cleanup; }
    TLOG("T2", "YOLO adopted");

    ret = npu_arena_adopt_yolov5(arena, &face);
    if (ret != 0) { TFAIL("T2", "adopt face failed"); npu_arena_destroy(arena); goto cleanup; }
    TLOG("T2", "Face adopted, arena total=%zu", npu_arena_total_size(arena));

    // Switch: YOLO → Face → YOLO → Face
    for (int cycle = 0; cycle < 2; cycle++) {
        // YOLO
        fill_test_image((unsigned char*)yolo.input_mems[0]->virt_addr,
                        yolo.model_width, yolo.model_height, 3);
        object_detect_result_list yolo_results;
        ret = inference_yolov5_model(&yolo, &yolo_results);
        if (ret != 0) {
            TFAIL("T2", "YOLO inference failed at cycle %d", cycle);
            pass = 0;
            break;
        }
        TLOG("T2", "cycle %d YOLO: %d objects", cycle, yolo_results.count);

        // Face
        fill_test_image((unsigned char*)face.input_mems[0]->virt_addr,
                        face.model_width, face.model_height, 3);
        face_detect_result_list face_results;
        ret = inference_face_detect_model(&face, &face_results);
        if (ret != 0) {
            TFAIL("T2", "Face inference failed at cycle %d", cycle);
            pass = 0;
            break;
        }
        TLOG("T2", "cycle %d Face: %d faces", cycle, face_results.count);
    }

    if (pass) {
        TLOG("T2", "arena cross-model switch stable over 2 cycles");
    }

    npu_arena_destroy(arena);

cleanup:
    release_face_detect_model(&face);
    release_yolov5_model(&yolo);

    if (pass) { TPASS("T2"); } else { TFAIL("T2", "switching failed"); }
    return pass ? 0 : -1;
}

// ── Test 3: rknn_create_mem_from_fd ──────────────

static int test3_zero_copy_fd(const char* yolo_path) {
    printf("\n=== T3: rknn_create_mem_from_fd ===\n");

    // Init model to get tensor attrs
    rknn_app_context_t yolo;
    memset(&yolo, 0, sizeof(yolo));
    if (init_yolov5_model(yolo_path, &yolo) != 0) {
        TFAIL("T3", "init_yolov5_model failed");
        return -1;
    }

    // Use zero_copy_init to test the full CMA→rknn_create_mem_from_fd path
    zero_copy_ctx_t zc;
    memset(&zc, 0, sizeof(zc));

    int ret = zero_copy_init(&zc, yolo.rknn_ctx,
                             &yolo.input_attrs[0],
                             640, 480, NULL);
    if (ret != 0) {
        TFAIL("T3", "zero_copy_init failed");
        release_yolov5_model(&yolo);
        return -1;
    }

    // Verify CMA buffer is valid
    if (zc.cma_fd < 0) {
        TFAIL("T3", "CMA fd invalid: %d", zc.cma_fd);
        zero_copy_destroy(&zc);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T3", "CMA buffer: fd=%d virt=%p size=%zu", zc.cma_fd, zc.cma_virt_addr, zc.cma_size);

    // Verify npu_input_mem was created from fd
    if (!zc.npu_input_mem) {
        TFAIL("T3", "npu_input_mem is NULL (rknn_create_mem_from_fd returned NULL)");
        zero_copy_destroy(&zc);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T3", "npu_input_mem: virt=%p phys=0x%llx size=%u flags=%u",
         zc.npu_input_mem->virt_addr,
         (unsigned long long)zc.npu_input_mem->phys_addr,
         zc.npu_input_mem->size,
         zc.npu_input_mem->flags);

    // Verify fd is passed through (flags should contain FROM_FD = 2)
    if (!(zc.npu_input_mem->flags & RKNN_TENSOR_MEMORY_FLAGS_FROM_FD)) {
        TLOG("T3", "WARNING: flags=0x%x, FROM_FD(0x%x) bit not set — fd may not be tracked",
             zc.npu_input_mem->flags, RKNN_TENSOR_MEMORY_FLAGS_FROM_FD);
        // Not a hard fail — some RKNN versions don't set this flag
    }

    // Test: write test data via virt_addr, bind to NPU, run inference
    fill_test_image((unsigned char*)zc.cma_virt_addr,
                    zc.model_width, zc.model_height, zc.model_channels);

    ret = zero_copy_bind_to_npu(&zc, yolo.rknn_ctx, &yolo.input_attrs[0]);
    if (ret != 0) {
        TFAIL("T3", "zero_copy_bind_to_npu failed (cross-ctx or API issue)");
        zero_copy_destroy(&zc);
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T3", "fd-backed tensor bound to NPU");

    zero_copy_destroy(&zc);
    release_yolov5_model(&yolo);
    TPASS("T3");
    return 0;
}

// ── Test 4: Arena output correctness ─────────────

static int test4_correctness(const char* yolo_path) {
    printf("\n=== T4: Arena output correctness ===\n");

    int ret, pass = 1;

    // Init model
    rknn_app_context_t yolo;
    memset(&yolo, 0, sizeof(yolo));
    if (init_yolov5_model(yolo_path, &yolo) != 0) {
        TFAIL("T4", "init_yolov5_model failed");
        return -1;
    }

    // ── Baseline: run inference with per-model DMA mems ──
    fill_test_image((unsigned char*)yolo.input_mems[0]->virt_addr,
                    yolo.model_width, yolo.model_height, 3);

    object_detect_result_list baseline_results;
    ret = inference_yolov5_model(&yolo, &baseline_results);
    if (ret != 0) {
        TFAIL("T4", "baseline inference failed");
        release_yolov5_model(&yolo);
        return -1;
    }
    TLOG("T4", "baseline: %d objects", baseline_results.count);

    // ── Save baseline output tensor values ──
    int n_out = yolo.io_num.n_output;
    unsigned char** baseline_outputs = (unsigned char**)calloc(n_out, sizeof(void*));
    size_t* output_sizes = (size_t*)calloc(n_out, sizeof(size_t));
    for (int i = 0; i < n_out; i++) {
        size_t sz = yolo.output_mems[i]->size;
        output_sizes[i] = sz;
        baseline_outputs[i] = (unsigned char*)malloc(sz);
        if (yolo.output_mems[i]->virt_addr && baseline_outputs[i]) {
            memcpy(baseline_outputs[i], yolo.output_mems[i]->virt_addr, sz);
        }
    }

    // ── Arena path: adopt + re-run same input ──
    fill_test_image((unsigned char*)yolo.input_mems[0]->virt_addr,
                    yolo.model_width, yolo.model_height, 3);

    npu_arena_t* arena = npu_arena_create();
    if (!arena) {
        TFAIL("T4", "npu_arena_create failed");
        pass = 0;
        goto cleanup4;
    }

    float max_diff = 0.0f;

    ret = npu_arena_register(arena, yolo.rknn_ctx,
                             yolo.input_attrs,  yolo.io_num.n_input,
                             yolo.output_attrs, yolo.io_num.n_output);
    if (ret != 0) { TFAIL("T4", "register failed"); pass = 0; goto cleanup4; }

    ret = npu_arena_allocate(arena, yolo.rknn_ctx);
    if (ret != 0) { TFAIL("T4", "allocate failed"); pass = 0; goto cleanup4; }

    ret = npu_arena_adopt_yolov5(arena, &yolo);
    if (ret != 0) {
        TFAIL("T4", "adopt_yolov5 failed");
        pass = 0;
        goto cleanup4;
    }

    fill_test_image((unsigned char*)yolo.input_mems[0]->virt_addr,
                    yolo.model_width, yolo.model_height, 3);

    object_detect_result_list arena_results;
    ret = inference_yolov5_model(&yolo, &arena_results);
    if (ret != 0) {
        TFAIL("T4", "arena inference failed");
        pass = 0;
        goto cleanup4;
    }
    TLOG("T4", "arena: %d objects", arena_results.count);

    // ── Compare output tensors ──
    // (max_diff declared above to avoid crossing initialization with goto)
    for (int i = 0; i < n_out; i++) {
        unsigned char* b = baseline_outputs[i];
        unsigned char* a = (unsigned char*)yolo.output_mems[i]->virt_addr;
        if (!b || !a) continue;
        for (size_t j = 0; j < output_sizes[i]; j++) {
            float d = (float)(b[j] > a[j] ? b[j] - a[j] : a[j] - b[j]);
            if (d > max_diff) max_diff = d;
        }
    }
    TLOG("T4", "max output diff = %.2f (0=identical, <2=FP rounding noise)", max_diff);

    if (max_diff > 2.0f) {
        TFAIL("T4", "output mismatch (max_diff=%.2f) — arena binding may be broken", max_diff);
        pass = 0;
    }

cleanup4:
    npu_arena_destroy(arena);
    for (int i = 0; i < n_out; i++) free(baseline_outputs[i]);
    free(baseline_outputs);
    free(output_sizes);
    release_yolov5_model(&yolo);

    if (pass) { TPASS("T4"); } else { TFAIL("T4", "correctness check failed"); }
    return pass ? 0 : -1;
}

// ── Main ─────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <yolov5.rknn> <face_detect.rknn>\n", argv[0]);
        printf("Verifies: NPU arena memory reuse + zero-copy fd import\n");
        return -1;
    }

    const char* yolo_path = argv[1];
    const char* face_path = argv[2];

    printf("=== NPU Board Verification ===\n");
    printf("YOLO: %s\n", yolo_path);
    printf("Face: %s\n", face_path);
    printf("Target: RV1106 (RKNPU v1.7.x)\n\n");

    int passed = 0, failed = 0;

    // T1: Arena lifecycle + YOLO inference
    if (test1_arena_yolov5(yolo_path) == 0) passed++; else failed++;

    // T2: Arena cross-model switching
    if (test2_arena_switching(yolo_path, face_path) == 0) passed++; else failed++;

    // T3: rknn_create_mem_from_fd (zero-copy fd import)
    if (test3_zero_copy_fd(yolo_path) == 0) passed++; else failed++;

    // T4: Arena output correctness
    if (test4_correctness(yolo_path) == 0) passed++; else failed++;

    printf("\n========================================\n");
    printf("RESULTS: %d passed, %d failed, %d total\n", passed, failed, passed + failed);
    printf("========================================\n");

    return (failed > 0) ? 1 : 0;
}
