#ifndef APPLICATION_H
#define APPLICATION_H

#include "../Audio/AudioProcess.h"
#include "../StateMachine/StateMachine.h"
#include "../Events/EventQueue.h"
#include "../Events/AppEvents.h"
#include "../WebSocket/WebsocketClient.h"
#include "../Intent/IntentHandler.h"
#include "VisionFrameBuffer.h"
#include "/home/ubuntu/Echo-Mate-main/Demo/yolov5_demo/cpp/include/opencv4/opencv2/core/core.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct VisionEvent {
    std::string event_type;
    std::string source = "active_opencv";
    std::string prompt;
    float confidence = 0.0f;
    int cooldown_ms = 5000;
    int priority = 0;
    int64_t occurred_ms = 0;
};

struct VisionContext {
    std::string request_id;
    std::string source;
    std::string prompt;
    std::string result_text;
    uint64_t frame_seq = 0;
    int64_t captured_ms = 0;
    int64_t updated_ms = 0;
    bool valid = false;
};

struct PendingVisionQuestion {
    std::string prompt;
    std::string source;
    bool valid = false;
};

class Application {
public:
    Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                const std::string& aliyun_api_key, int protocolVersion,
                int sample_rate, int channels, int frame_duration);
    Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                const std::string& aliyun_api_key, int protocolVersion,
                int sample_rate, int channels, int frame_duration, const std::string& vision_model_path);
    Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                const std::string& aliyun_api_key, int protocolVersion,
                int sample_rate, int channels, int frame_duration,
                const std::string& vision_model_path, const std::string& face_model_path);
    ~Application();

    void Run();

    void Stop(void) {
        eventQueue_.Enqueue(static_cast<int>(AppEvent::to_stop));
    }

    AudioProcess audio_processor_;
    StateMachine client_state_;
    EventQueue<int> eventQueue_;
    EventQueue<Json::Value> IntentQueue_;
    WebSocketClient ws_client_;
    IntentHandler intent_handler_;

    void set_first_audio_msg_received(bool flag) {
        first_audio_msg_received_ = flag;
    }
    bool get_first_audio_msg_received() {
        return first_audio_msg_received_;
    }

    void set_tts_completed(bool flag) {
        tts_completed_ = flag;
    }
    bool get_tts_completed() {
        return tts_completed_;
    }

    void set_dialogue_completed(bool flag) {
        dialogue_completed_ = flag;
    }
    bool get_dialogue_completed() {
        return dialogue_completed_;
    }

    void set_aliyun_api_key(const std::string& key) {
        aliyun_api_key_ = key;
    }
    std::string get_aliyun_api_key() {
        return aliyun_api_key_;
    }

    void set_threads_stop_sig(bool flag) {
        threads_stop_flag_.store(flag);
    }
    bool get_threads_stop_sig() {
        return threads_stop_flag_.load();
    }

    void set_ws_protocolVersion(int version) {
        ws_protocolVersion_ = version;
    }
    int get_ws_protocolVersion() {
        return ws_protocolVersion_;
    }

    int getState() {
        return client_state_.GetCurrentState();
    }

    void UpdateLatestFrame(const cv::Mat& frame, int fd = -1);
    cv::Mat GetLatestFrame();
    bool GetLatestFrameSnapshot(FrameSnapshot& snapshot, int max_age_ms);
    bool SubmitVisionQuestion(const std::string& prompt, const std::string& source);
    bool SubmitVisionEvent(const VisionEvent& event);
    bool SubmitActiveVisionEvent(const std::string& event_label, float confidence, int cooldown_ms);
    void QueuePendingVisionQuestion(const std::string& prompt, const std::string& source);
    bool ConsumePendingVisionQuestion(PendingVisionQuestion& question);
    void DescribeCurrentScene(const cv::Mat& image);
    void DescribeCurrentScene(const cv::Mat& image,
                              const std::string& request_id,
                              const std::string& source,
                              uint64_t frame_seq,
                              int64_t captured_ms,
                              const std::string& prompt,
                              int frame_fd = -1);
    void UpdateVisionContext(const std::string& request_id,
                             const std::string& source,
                             const std::string& prompt,
                             const std::string& result_text,
                             uint64_t frame_seq,
                             int64_t captured_ms);
    VisionContext GetVisionContext() const;
    void InterruptTTS(const std::string& reason);
    bool IsActiveVisionRequest(const std::string& request_id);
    void ClearActiveVisionRequest(const std::string& request_id);
    bool ShouldAcceptIncomingAudio() const;
    void SetAcceptIncomingAudio(bool accept);

private:
    std::string NextVisionRequestId();
    void SetActiveVisionRequest(const std::string& request_id);
    bool StartCamera(const std::string& model_path);
    bool StartCameraV2(const std::string& yolo_path, const std::string& face_path, int face_interval);
    void StopCamera();
    int64_t NowMs() const;
    std::string BuildVisionPrompt(const std::string& prompt, const std::string& source) const;
    bool IsVisionContextFreshLocked(int64_t now_ms) const;

    bool first_audio_msg_received_ = false;
    bool tts_completed_ = false;
    bool dialogue_completed_ = false;
    std::string aliyun_api_key_;
    int ws_protocolVersion_;
    std::atomic<bool> threads_stop_flag_{false};
    std::atomic<bool> accept_incoming_audio_{true};
    std::thread ws_msg_thread_;
    std::thread state_trans_thread_;
    bool owns_camera_ = false;
    std::string vision_model_path_;
    std::string face_detect_model_path_;
    int face_interval_ = 3;
    VisionFrameBuffer frame_buffer_;
    std::atomic<uint64_t> vision_request_counter_{0};
    std::mutex vision_request_mutex_;
    std::string active_vision_request_id_;
    std::mutex active_vision_mutex_;
    int64_t last_active_vision_ms_ = 0;
    int active_vision_default_cooldown_ms_ = 5000;
    int vision_context_ttl_ms_ = 15000;
    mutable std::mutex vision_context_mutex_;
    VisionContext latest_vision_context_;
    std::mutex pending_vision_mutex_;
    PendingVisionQuestion pending_vision_question_;
};

#endif  // APPLICATION_H
