// NPU DMA tensor arena implementation.
// See npu_memory_reuse.h for design document and lifecycle.
//
// Key invariants (kernel-verified on RV1106):
//   - rknn_destroy_mem on an IO-bound mem triggers rknpu_mem_sync_ioctl
//     which NULL-derefs in the kernel driver → Oops. Always unbind first:
//     rknn_set_io_mem(new) replaces old binding → then rknn_destroy_mem(old).
//   - Cross-ctx rknn_set_io_mem: arena mems created from primary_ctx,
//     bound to each model's own ctx. RKNPU v1.7.x supports this.

#include "npu_memory_reuse.h"
#include "yolov5.h"
#include "kws_detector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

// ── Constants ──────────────────────────────────────
static constexpr int MAX_OUTPUTS = 3;
static constexpr int MAX_SLOTS   = 4;

// ── Per-model metadata ─────────────────────────────
struct ModelSlot {
    rknn_context ctx;
    bool        registered;

    // Cached tensor attrs (needed for npu_arena_bind → rknn_set_io_mem).
    rknn_tensor_attr input_attrs[1];
    rknn_tensor_attr output_attrs[MAX_OUTPUTS];
    uint32_t n_input;
    uint32_t n_output;

    // Back-pointers to adopted context's mem arrays.
    // npu_arena_destroy uses these to NULL stale pointers before freeing.
    rknn_tensor_mem** adopted_input_mems;
    rknn_tensor_mem** adopted_output_mems;
    int               adopted_n_output;
};

// ── Arena internal state ───────────────────────────
struct npu_arena_t {
    ArenaState state;

    ModelSlot slots[MAX_SLOTS];
    int       num_slots;

    // Max tensor sizes across all registered models
    size_t max_input_size;
    size_t max_output_sizes[MAX_OUTPUTS];
    int    max_num_outputs;

    // Shared DMA buffers (allocated once in npu_arena_allocate)
    rknn_tensor_mem* input_mem;
    rknn_tensor_mem* output_mems[MAX_OUTPUTS];

    // Context currently bound; primary = used for rknn_create_mem
    rknn_context bound_ctx;
    rknn_context primary_ctx;

    // Serializes bind+run across camera thread (YOLO/Face) and IdleState thread (KWS)
    pthread_mutex_t run_mutex;
};

// ── Helpers ────────────────────────────────────────

static const char* arena_state_name(ArenaState s) {
    switch (s) {
        case ARENA_EMPTY:       return "EMPTY";
        case ARENA_REGISTERED:  return "REGISTERED";
        case ARENA_ALLOCATED:   return "ALLOCATED";
        case ARENA_DESTROYED:   return "DESTROYED";
        default:                return "UNKNOWN";
    }
}

static ModelSlot* find_slot(npu_arena_t* a, rknn_context ctx) {
    for (int i = 0; i < a->num_slots; i++) {
        if (a->slots[i].ctx == ctx) return &a->slots[i];
    }
    return nullptr;
}

static bool slot_exists(npu_arena_t* a, rknn_context ctx) {
    return find_slot(a, ctx) != nullptr;
}

// Log output tensor differences between a model and arena max sizes.
// Only warns — never fails. Called once during register.
static void check_output_compat(const char* label, rknn_context ctx,
                                const rknn_tensor_attr* attrs, uint32_t n,
                                const size_t* max_sizes) {
    for (uint32_t i = 0; i < n && i < (uint32_t)MAX_OUTPUTS; i++) {
        if (attrs[i].size_with_stride < max_sizes[i]) {
            printf("[Arena] %s ctx=%p out[%u]: model=%d < arena_max=%zu — safe\n",
                   label, (void*)ctx, i, attrs[i].size_with_stride, max_sizes[i]);
        }
    }
}

// ── Lifecycle ──────────────────────────────────────

npu_arena_t* npu_arena_create(void) {
    npu_arena_t* a = (npu_arena_t*)calloc(1, sizeof(npu_arena_t));
    if (!a) return nullptr;

    a->state       = ARENA_EMPTY;
    a->num_slots   = 0;
    a->max_input_size = 0;
    a->max_num_outputs = 0;
    a->input_mem   = nullptr;
    a->bound_ctx   = 0;
    a->primary_ctx = 0;

    for (int i = 0; i < MAX_OUTPUTS; i++) {
        a->max_output_sizes[i] = 0;
        a->output_mems[i] = nullptr;
    }

    pthread_mutex_init(&a->run_mutex, NULL);

    printf("[Arena] created (state=%s)\n", arena_state_name(a->state));
    return a;
}

int npu_arena_register(npu_arena_t* a,
                       rknn_context ctx,
                       const rknn_tensor_attr* input_attrs,  uint32_t n_input,
                       const rknn_tensor_attr* output_attrs, uint32_t n_output) {
    if (!a || !ctx || n_input > 1 || n_output > MAX_OUTPUTS) {
        fprintf(stderr, "[Arena] register: invalid args\n");
        return -1;
    }
    if (a->state != ARENA_EMPTY && a->state != ARENA_REGISTERED && a->state != ARENA_ALLOCATED) {
        fprintf(stderr, "[Arena] register: invalid state %s\n",
                arena_state_name(a->state));
        return -1;
    }

    // Late registration: model loaded after arena already allocated.
    // Validate it fits in the existing arena buffers.
    if (a->state == ARENA_ALLOCATED) {
        size_t in_sz = 0;
        for (uint32_t i = 0; i < n_input && i < 1; i++) {
            in_sz = input_attrs[i].size_with_stride;
        }
        if (in_sz > a->max_input_size) {
            fprintf(stderr, "[Arena] register: late model input=%zu exceeds arena=%zu\n",
                    in_sz, a->max_input_size);
            return -1;
        }
        for (uint32_t i = 0; i < n_output && i < (uint32_t)MAX_OUTPUTS; i++) {
            if (output_attrs[i].size_with_stride > a->max_output_sizes[i]) {
                fprintf(stderr, "[Arena] register: late model out[%u]=%d exceeds arena=%zu\n",
                        i, output_attrs[i].size_with_stride, a->max_output_sizes[i]);
                return -1;
            }
        }
    }
    if (slot_exists(a, ctx)) {
        fprintf(stderr, "[Arena] register: ctx=%p already registered\n", (void*)ctx);
        return -1;
    }
    if (a->num_slots >= MAX_SLOTS) {
        fprintf(stderr, "[Arena] register: max slots (%d) reached\n", MAX_SLOTS);
        return -1;
    }

    ModelSlot* slot = &a->slots[a->num_slots];
    memset(slot, 0, sizeof(ModelSlot));
    slot->ctx        = ctx;
    slot->registered = true;
    slot->n_input    = n_input;
    slot->n_output   = n_output;

    // Copy and track input attrs
    size_t in_size = 0;
    for (uint32_t i = 0; i < n_input && i < 1; i++) {
        memcpy(&slot->input_attrs[i], &input_attrs[i], sizeof(rknn_tensor_attr));
        slot->input_attrs[i].fmt  = RKNN_TENSOR_NHWC;
        slot->input_attrs[i].type = RKNN_TENSOR_UINT8;
        in_size = input_attrs[i].size_with_stride;
    }

    // Copy and track output attrs
    for (uint32_t i = 0; i < n_output && i < MAX_OUTPUTS; i++) {
        memcpy(&slot->output_attrs[i], &output_attrs[i], sizeof(rknn_tensor_attr));
        size_t out_sz = output_attrs[i].size_with_stride;
        if (out_sz > a->max_output_sizes[i]) {
            a->max_output_sizes[i] = out_sz;
        }
    }

    // Update global maxes
    if (in_size > a->max_input_size) a->max_input_size = in_size;
    if ((int)n_output > a->max_num_outputs) a->max_num_outputs = (int)n_output;

    // Log output compatibility (warn only — arena is safe if arena ≥ model)
    check_output_compat("register", ctx, output_attrs, n_output, a->max_output_sizes);

    a->num_slots++;
    a->state = ARENA_REGISTERED;

    printf("[Arena] registered ctx=%p input=%zu", (void*)ctx, in_size);
    for (uint32_t i = 0; i < n_output; i++) {
        printf(" out[%u]=%d", i, output_attrs[i].size_with_stride);
    }
    printf(" (total slots=%d, state=%s)\n", a->num_slots, arena_state_name(a->state));
    return 0;
}

int npu_arena_allocate(npu_arena_t* a, rknn_context primary_ctx) {
    if (!a || !primary_ctx) return -1;
    if (a->state != ARENA_REGISTERED) {
        fprintf(stderr, "[Arena] allocate: invalid state %s (expected REGISTERED)\n",
                arena_state_name(a->state));
        return -1;
    }
    if (a->num_slots == 0) {
        fprintf(stderr, "[Arena] allocate: no models registered\n");
        return -1;
    }

    a->primary_ctx = primary_ctx;
    size_t total = 0;

    // Allocate input buffer
    if (a->max_input_size > 0) {
        a->input_mem = rknn_create_mem(primary_ctx, a->max_input_size);
        if (!a->input_mem) {
            fprintf(stderr, "[Arena] allocate: failed to alloc input (%zu bytes)\n",
                    a->max_input_size);
            return -1;
        }
        total += a->max_input_size;
    }

    // Allocate output buffers
    for (int i = 0; i < a->max_num_outputs; i++) {
        if (a->max_output_sizes[i] > 0) {
            a->output_mems[i] = rknn_create_mem(primary_ctx, a->max_output_sizes[i]);
            if (!a->output_mems[i]) {
                fprintf(stderr, "[Arena] allocate: failed to alloc output[%d] (%zu bytes)\n",
                        i, a->max_output_sizes[i]);
                npu_arena_destroy(a);
                return -1;
            }
            total += a->max_output_sizes[i];
        }
    }

    a->state = ARENA_ALLOCATED;
    printf("[Arena] allocated %zu bytes DMA (input=%zu, outputs=%d, state=%s)\n",
           total, a->max_input_size, a->max_num_outputs, arena_state_name(a->state));
    return 0;
}

// ── Bind (internal) ────────────────────────────────

static int arena_bind_locked(npu_arena_t* a, rknn_context ctx,
                             const rknn_tensor_attr* input_attr,
                             const rknn_tensor_attr* output_attrs,
                             uint32_t n_output) {
    // Already bound to this ctx → no-op
    if (a->bound_ctx == ctx) return 0;

    ARENA_PROFILE_BIND_BEGIN();

    int ret;

    // Bind input
    if (a->input_mem) {
        ret = rknn_set_io_mem(ctx, a->input_mem, const_cast<rknn_tensor_attr*>(input_attr));
        if (ret < 0) {
            fprintf(stderr, "[Arena] bind input to ctx=%p failed, ret=%d\n",
                    (void*)ctx, ret);
            return -1;
        }
    }

    // Bind outputs
    for (uint32_t i = 0; i < n_output && i < (uint32_t)MAX_OUTPUTS; i++) {
        if (!a->output_mems[i]) {
            fprintf(stderr, "[Arena] bind: output[%u] is NULL for ctx=%p\n",
                    i, (void*)ctx);
            return -1;
        }
        ret = rknn_set_io_mem(ctx, a->output_mems[i], const_cast<rknn_tensor_attr*>(&output_attrs[i]));
        if (ret < 0) {
            fprintf(stderr, "[Arena] bind output[%u] to ctx=%p failed, ret=%d\n",
                    i, (void*)ctx, ret);
            return -1;
        }
    }

    a->bound_ctx = ctx;
    printf("[Arena] bound to ctx=%p\n", (void*)ctx);

    ARENA_PROFILE_BIND_END("bind");
    return 0;
}

// ── Adopt (bind + swap pointers + destroy old mems) ─

int npu_arena_adopt_yolov5(npu_arena_t* a, rknn_app_context_t* app_ctx) {
    if (!a || !app_ctx) return -1;
    if (a->state != ARENA_ALLOCATED) {
        fprintf(stderr, "[Arena] adopt_yolov5: invalid state %s (expected ALLOCATED)\n",
                arena_state_name(a->state));
        return -1;
    }
    if (!slot_exists(a, app_ctx->rknn_ctx)) {
        fprintf(stderr, "[Arena] adopt_yolov5: ctx=%p not registered\n",
                (void*)app_ctx->rknn_ctx);
        return -1;
    }

    ModelSlot* slot = find_slot(a, app_ctx->rknn_ctx);

    // 1. Bind arena buffers (replaces old per-model NPU IO bindings)
    int ret = arena_bind_locked(a, app_ctx->rknn_ctx,
                                &slot->input_attrs[0],
                                slot->output_attrs, slot->n_output);
    if (ret != 0) {
        fprintf(stderr, "[Arena] adopt_yolov5: bind failed\n");
        return -1;
    }

    // 2. Save old mems, swap in arena mems
    rknn_tensor_mem* old_in  = app_ctx->input_mems[0];
    rknn_tensor_mem* old_out0 = app_ctx->output_mems[0];
    rknn_tensor_mem* old_out1 = app_ctx->output_mems[1];
    rknn_tensor_mem* old_out2 = app_ctx->output_mems[2];

    app_ctx->input_mems[0]  = a->input_mem;
    app_ctx->output_mems[0] = a->output_mems[0];
    app_ctx->output_mems[1] = a->output_mems[1];
    app_ctx->output_mems[2] = a->output_mems[2];

    // 3. Destroy old per-model mems (safe — already unbound from NPU in step 1)
    //    rknn_destroy_mem frees the rknn_tensor_mem* internally (ALLOC_INSIDE flag).
    if (old_in)  rknn_destroy_mem(app_ctx->rknn_ctx, old_in);
    if (old_out0) rknn_destroy_mem(app_ctx->rknn_ctx, old_out0);
    if (old_out1) rknn_destroy_mem(app_ctx->rknn_ctx, old_out1);
    if (old_out2) rknn_destroy_mem(app_ctx->rknn_ctx, old_out2);

    // 4. Save back-pointers for arena_destroy cleanup
    slot->adopted_input_mems  = app_ctx->input_mems;
    slot->adopted_output_mems = app_ctx->output_mems;
    slot->adopted_n_output    = app_ctx->io_num.n_output;

    printf("[Arena] adopted YOLO/Face ctx=%p, saved=%zu bytes\n",
           (void*)app_ctx->rknn_ctx, npu_arena_saved_bytes(a));
    return 0;
}

int npu_arena_adopt_kws(npu_arena_t* a, kws_app_context_t* ctx) {
    if (!a || !ctx) return -1;
    if (a->state != ARENA_ALLOCATED) {
        fprintf(stderr, "[Arena] adopt_kws: invalid state %s (expected ALLOCATED)\n",
                arena_state_name(a->state));
        return -1;
    }
    if (!slot_exists(a, ctx->rknn_ctx)) {
        fprintf(stderr, "[Arena] adopt_kws: ctx=%p not registered\n",
                (void*)ctx->rknn_ctx);
        return -1;
    }

    ModelSlot* slot = find_slot(a, ctx->rknn_ctx);

    // 1. Bind arena buffers (replaces old per-model NPU IO bindings)
    int ret = arena_bind_locked(a, ctx->rknn_ctx,
                                &slot->input_attrs[0],
                                slot->output_attrs, slot->n_output);
    if (ret != 0) {
        fprintf(stderr, "[Arena] adopt_kws: bind failed\n");
        return -1;
    }

    // 2. Save old mems, swap in arena mems
    rknn_tensor_mem* old_in  = ctx->input_mems[0];
    rknn_tensor_mem* old_out = ctx->output_mems[0];

    ctx->input_mems[0]  = a->input_mem;
    ctx->output_mems[0] = a->output_mems[0];

    // 3. Destroy old per-model mems (safe — already unbound from NPU in step 1)
    if (old_in)  rknn_destroy_mem(ctx->rknn_ctx, old_in);
    if (old_out) rknn_destroy_mem(ctx->rknn_ctx, old_out);

    // 4. Save back-pointers
    slot->adopted_input_mems  = ctx->input_mems;
    slot->adopted_output_mems = ctx->output_mems;
    slot->adopted_n_output    = ctx->io_num.n_output;

    printf("[Arena] adopted KWS ctx=%p, saved=%zu bytes\n",
           (void*)ctx->rknn_ctx, npu_arena_saved_bytes(a));
    return 0;
}

// ── Re-bind (no-op if same ctx already bound) ─────

int npu_arena_bind_yolov5(npu_arena_t* a, rknn_app_context_t* app_ctx) {
    if (!a || !app_ctx) return -1;
    if (!slot_exists(a, app_ctx->rknn_ctx)) return -1;

    ModelSlot* slot = find_slot(a, app_ctx->rknn_ctx);
    int ret = arena_bind_locked(a, app_ctx->rknn_ctx,
                                &slot->input_attrs[0],
                                slot->output_attrs, slot->n_output);
    if (ret != 0) return ret;

    app_ctx->input_mems[0]  = a->input_mem;
    app_ctx->output_mems[0] = a->output_mems[0];
    app_ctx->output_mems[1] = a->output_mems[1];
    app_ctx->output_mems[2] = a->output_mems[2];
    return 0;
}

int npu_arena_bind_kws(npu_arena_t* a, kws_app_context_t* ctx) {
    if (!a || !ctx) return -1;
    if (!slot_exists(a, ctx->rknn_ctx)) return -1;

    ModelSlot* slot = find_slot(a, ctx->rknn_ctx);
    int ret = arena_bind_locked(a, ctx->rknn_ctx,
                                &slot->input_attrs[0],
                                slot->output_attrs, slot->n_output);
    if (ret != 0) return ret;

    ctx->input_mems[0]  = a->input_mem;
    ctx->output_mems[0] = a->output_mems[0];
    return 0;
}

// ── Destroy ────────────────────────────────────────

void npu_arena_destroy(npu_arena_t* a) {
    if (!a || a->state == ARENA_DESTROYED) return;

    // 1. NULL out all adopted models' mem pointers FIRST.
    //    Their release functions will see NULL and skip rknn_destroy_mem.
    for (int s = 0; s < a->num_slots; s++) {
        ModelSlot* slot = &a->slots[s];
        if (slot->adopted_input_mems) {
            slot->adopted_input_mems[0] = nullptr;
        }
        if (slot->adopted_output_mems) {
            for (int i = 0; i < slot->adopted_n_output && i < MAX_OUTPUTS; i++) {
                slot->adopted_output_mems[i] = nullptr;
            }
        }
    }

    // 2. Destroy arena DMA buffers
    if (a->primary_ctx != 0) {
        if (a->input_mem) {
            rknn_destroy_mem(a->primary_ctx, a->input_mem);
            a->input_mem = nullptr;
        }
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (a->output_mems[i]) {
                rknn_destroy_mem(a->primary_ctx, a->output_mems[i]);
                a->output_mems[i] = nullptr;
            }
        }
    }

    a->bound_ctx = 0;
    a->state = ARENA_DESTROYED;
    pthread_mutex_destroy(&a->run_mutex);
    printf("[Arena] destroyed\n");
    free(a);
}

// ── Query ──────────────────────────────────────────

ArenaState npu_arena_state(npu_arena_t* a) {
    return a ? a->state : ARENA_DESTROYED;
}

size_t npu_arena_total_size(npu_arena_t* a) {
    if (!a) return 0;
    size_t total = a->max_input_size;
    for (int i = 0; i < a->max_num_outputs; i++) {
        total += a->max_output_sizes[i];
    }
    return total;
}

size_t npu_arena_saved_bytes(npu_arena_t* a) {
    if (!a || a->num_slots == 0) return 0;

    size_t per_model_total = 0;
    for (int s = 0; s < a->num_slots; s++) {
        ModelSlot* slot = &a->slots[s];
        for (uint32_t i = 0; i < slot->n_input; i++) {
            per_model_total += slot->input_attrs[i].size_with_stride;
        }
        for (uint32_t i = 0; i < slot->n_output; i++) {
            per_model_total += slot->output_attrs[i].size_with_stride;
        }
    }

    size_t arena_total = npu_arena_total_size(a);
    return (per_model_total > arena_total) ? (per_model_total - arena_total) : 0;
}

int npu_arena_num_registered(npu_arena_t* a) {
    return a ? a->num_slots : 0;
}

// ── Accessors ──────────────────────────────────────

rknn_tensor_mem* npu_arena_input_mem(npu_arena_t* a) {
    if (!a || a->state != ARENA_ALLOCATED) return nullptr;
    return a->input_mem;
}

rknn_tensor_mem* npu_arena_output_mem(npu_arena_t* a, int index) {
    if (!a || a->state != ARENA_ALLOCATED) return nullptr;
    if (index < 0 || index >= MAX_OUTPUTS) return nullptr;
    return a->output_mems[index];
}

// ── Global singleton ───────────────────────────────

static npu_arena_t* g_npu_arena = nullptr;

void npu_set_global_arena(npu_arena_t* arena) {
    g_npu_arena = arena;
}

npu_arena_t* npu_get_global_arena(void) {
    return g_npu_arena;
}

// ── Thread safety ──────────────────────────────────

void npu_arena_lock(npu_arena_t* a) {
    if (a) pthread_mutex_lock(&a->run_mutex);
}

void npu_arena_unlock(npu_arena_t* a) {
    if (a) pthread_mutex_unlock(&a->run_mutex);
}
