#include "Idle.h"
#include "../../Utils/user_log.h"
#include "../../Events/AppEvents.h"
#include "kws_detector.h"
#include "npu_memory_reuse.h"

// 静态成员变量定义
std::atomic<bool> IdleState::state_running_{false};
std::thread IdleState::state_running_thread_;

// KWS context — owned by IdleState, initialized on first Enter
static kws_app_context_t g_kws_ctx;
static bool g_kws_initialized = false;
static bool g_kws_uses_arena = false;

void IdleState::Enter(Application* app) {
    std::string json_message = R"({"type": "state", "state": "idle"})";
    app->ws_client_.SendText(json_message);
    // clear recorded audio queue
    app->audio_processor_.clearRecordedAudioQueue();
    // 开启录音
    app->audio_processor_.startRecording();
    // start state running
    state_running_.store(true);
    state_running_thread_ = std::thread([app]() { Run(app); });
    USER_LOG_INFO("Into Idle state.");
}

void IdleState::Run(Application* app) {
    USER_LOG_INFO("Idle state run.");

    // init KWS model on first entry (lazy, one-time)
    if (!g_kws_initialized) {
        memset(&g_kws_ctx, 0, sizeof(g_kws_ctx));
        if (init_kws_model("model/kws_marvin.rknn", &g_kws_ctx) != 0) {
            USER_LOG_ERROR("KWS model init failed — wake word disabled");
            g_kws_initialized = true; // mark done so we don't retry
        } else {
            g_kws_initialized = true;
            USER_LOG_INFO("KWS model loaded, wake word active.");

            // Register with arena + adopt if arena is ready
            npu_arena_t* arena = npu_get_global_arena();
            if (arena && npu_arena_state(arena) == ARENA_ALLOCATED) {
                npu_arena_register(arena,
                                   g_kws_ctx.rknn_ctx,
                                   g_kws_ctx.input_attrs,  g_kws_ctx.io_num.n_input,
                                   g_kws_ctx.output_attrs, g_kws_ctx.io_num.n_output);
                npu_arena_adopt_kws(arena, &g_kws_ctx);
                g_kws_uses_arena = true;
                USER_LOG_INFO("KWS model registered + adopted into shared DMA arena.");
            } else if (arena) {
                USER_LOG_WARN("Arena exists but not allocated; KWS uses private DMA");
            } else {
                USER_LOG_INFO("No NPU arena; KWS uses private DMA");
            }
        }
    }

    std::vector<int16_t> data;
    kws_result_t kw_res;

    while (state_running_.load() == true) {
        if (app->audio_processor_.recordedQueueIsEmpty() == false) {
            app->audio_processor_.getRecordedAudio(data);

            if (g_kws_initialized && g_kws_ctx.rknn_ctx != 0) {
                npu_arena_t* arena = npu_get_global_arena();
                if (g_kws_uses_arena && arena) npu_arena_lock(arena);
                kws_feed_frame(&g_kws_ctx, data.data(), &kw_res);
                if (g_kws_uses_arena && arena) npu_arena_unlock(arena);
                if (kw_res.detected) {
                    USER_LOG_INFO("Wake detected (KWS prob=%.3f).", kw_res.prob);
                    app->eventQueue_.Enqueue(static_cast<int>(AppEvent::wake_detected));
                    break;
                }
            }
        }
    }
}

void IdleState::Exit(Application* app) {
    // stop录音
    app->audio_processor_.stopRecording();
    // stop running
    state_running_.store(false);
    state_running_thread_.join();
    // reset KWS buffer for next idle cycle
    if (g_kws_initialized && g_kws_ctx.rknn_ctx != 0) {
        kws_reset_buffer(&g_kws_ctx);
    }

    // playing waked up sound
    std::string waked_sound_path = "third_party/audio/waked.pcm";
    auto audioQueue = app->audio_processor_.loadAudioFromFile(waked_sound_path, 40);
    while (!audioQueue.empty()) {
        const auto& frame = audioQueue.front();
        app->audio_processor_.addFrameToPlaybackQueue(frame);
        audioQueue.pop();
    }
    app->set_tts_completed(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    USER_LOG_INFO("Idle State exit.");
}
