// NPU DMA tensor arena — shared I/O buffer pool for multi-model inference.
//
// RV1106 has a single NPU core. rknn_run() is blocking. All inference is
// synchronous and serialized. This arena replaces per-model DMA allocations
// with a single shared pool, saving ~30% NPU DMA memory.
//
// Execution model (multi-threaded with mutex serialization):
//   YOLO:  lock → bind → rknn_run(block) → unlock
//   Face:  lock → bind → rknn_run(block) → unlock
//   KWS:   lock → bind → rknn_run(block) → unlock
//
// Arena mutex serializes bind+run across threads (KWS runs in IdleState
// thread, camera models run in camera thread).
//
// Lifecycle:
//   1. npu_arena_create()
//   2. npu_arena_register() × N   (after each rknn_init, tensor attrs known)
//   3. npu_arena_allocate()       (once, after all registrations)
//   4. npu_arena_adopt_xxx() × N  (bind arena mems + destroy per-model mems)
//   5. npu_arena_bind_xxx() before each rknn_run (no-op if already bound)
//   6. npu_arena_destroy()
//
// Bind-before-destroy ordering (kernel-verified on RV1106):
//   rknn_destroy_mem on an NPU-IO-bound mem triggers rknpu_mem_sync_ioctl
//   in the kernel driver, which crashes (NULL deref) if the mem is still
//   active. Always: rknn_set_io_mem(new) first → rknn_destroy_mem(old) after.

#ifndef NPU_MEMORY_REUSE_H_
#define NPU_MEMORY_REUSE_H_

#include "rknn_api.h"
#include "yolov5.h"
#include "kws_detector.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Arena state machine ─────────────────────────────

typedef enum {
    ARENA_EMPTY,        // created, no models registered
    ARENA_REGISTERED,   // ≥1 model registered, not yet allocated
    ARENA_ALLOCATED,    // DMA buffers allocated, ready for adopt/bind
    ARENA_DESTROYED     // resources released
} ArenaState;

// ── Profiling (compile-time optional) ───────────────

#ifdef ARENA_PROFILE
#define ARENA_PROFILE_BIND_BEGIN()  do { \
    struct timespec __t0, __t1; clock_gettime(CLOCK_MONOTONIC, &__t0)
#define ARENA_PROFILE_BIND_END(tag) \
    clock_gettime(CLOCK_MONOTONIC, &__t1); \
    long __ns = (__t1.tv_sec - __t0.tv_sec) * 1000000000L + (__t1.tv_nsec - __t0.tv_nsec); \
    printf("[Arena-profile] bind(%s) = %ld us\n", tag, __ns / 1000); \
    } while(0)
#define ARENA_PROFILE_RUN_BEGIN()  do { \
    struct timespec __rt0, __rt1; clock_gettime(CLOCK_MONOTONIC, &__rt0)
#define ARENA_PROFILE_RUN_END(tag) \
    clock_gettime(CLOCK_MONOTONIC, &__rt1); \
    long __rns = (__rt1.tv_sec - __rt0.tv_sec) * 1000000000L + (__rt1.tv_nsec - __rt0.tv_nsec); \
    printf("[Arena-profile] run(%s)  = %ld us\n", tag, __rns / 1000); \
    } while(0)
#else
#define ARENA_PROFILE_BIND_BEGIN()  ((void)0)
#define ARENA_PROFILE_BIND_END(tag) ((void)0)
#define ARENA_PROFILE_RUN_BEGIN()   ((void)0)
#define ARENA_PROFILE_RUN_END(tag)  ((void)0)
#endif

// ── Opaque handle ──────────────────────────────────
typedef struct npu_arena_t npu_arena_t;

// ── Lifecycle ──────────────────────────────────────

// Create an empty arena. No DMA memory allocated yet.
npu_arena_t* npu_arena_create(void);

// Register a model's tensor requirements.
// Call AFTER rknn_init / rknn_query so tensor sizes are known.
// Only valid in ARENA_EMPTY or ARENA_REGISTERED state.
// Returns 0 on success.
int npu_arena_register(npu_arena_t* arena,
                       rknn_context ctx,
                       const rknn_tensor_attr* input_attrs,  uint32_t n_input,
                       const rknn_tensor_attr* output_attrs, uint32_t n_output);

// Allocate shared DMA buffers (sized for the largest registered model).
// primary_ctx: any valid rknn_context from a registered model.
// Only valid after all models are registered. Must NOT be called in adopt().
// Returns 0 on success.
int npu_arena_allocate(npu_arena_t* arena, rknn_context primary_ctx);

// Bind arena buffers to a model AND destroy its old per-model DMA mems.
// Replaces: arena_bind + app_ctx pointer update + old mem cleanup.
// Call AFTER npu_arena_allocate(). Only valid in ARENA_ALLOCATED state.
// bind-before-destroy: arena mems are bound via rknn_set_io_mem FIRST,
// then old per-model mems are destroyed (already unbound → kernel-safe).
int npu_arena_adopt_yolov5(npu_arena_t* arena, rknn_app_context_t* app_ctx);
int npu_arena_adopt_kws(npu_arena_t* arena, kws_app_context_t* ctx);

// Re-bind arena to a specific model before rknn_run.
// Call when switching from a different model. No-op if already bound.
int npu_arena_bind_yolov5(npu_arena_t* arena, rknn_app_context_t* app_ctx);
int npu_arena_bind_kws(npu_arena_t* arena, kws_app_context_t* ctx);

// Serialize bind+run across threads. Call lock BEFORE arena_bind+arkn_run,
// unlock AFTER rknn_run. Required because KWS runs in a different pthread
// than the camera thread (YOLO/Face).
void npu_arena_lock(npu_arena_t* arena);
void npu_arena_unlock(npu_arena_t* arena);

// Release all DMA memory. Clears adopted context pointers before freeing.
// Call BEFORE rknn_destroy on registered contexts.
void npu_arena_destroy(npu_arena_t* arena);

// ── Query ──────────────────────────────────────────

ArenaState npu_arena_state(npu_arena_t* arena);
size_t     npu_arena_total_size(npu_arena_t* arena);
size_t     npu_arena_saved_bytes(npu_arena_t* arena);
int        npu_arena_num_registered(npu_arena_t* arena);

// ── Accessors (valid after adopt) ───────────────────

rknn_tensor_mem* npu_arena_input_mem(npu_arena_t* arena);
rknn_tensor_mem* npu_arena_output_mem(npu_arena_t* arena, int index);

// ── Global singleton ───────────────────────────────

void npu_set_global_arena(npu_arena_t* arena);
npu_arena_t* npu_get_global_arena(void);

#ifdef __cplusplus
}
#endif

#endif  // NPU_MEMORY_REUSE_H_
