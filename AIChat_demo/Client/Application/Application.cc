#include "Application.h"
#include "../Utils/user_log.h"
#include "StateConfig.h"
#include "WS_Handler.h"

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/opencv.hpp>

#include <chrono>
#include <cstdlib>
#include <sstream>

#include "AIcamera_c_interface.h"
#include "hw_jpeg_encoder.h"

WSHandler app_handler;

extern std::string base64_encode(unsigned char const*, unsigned int);

namespace {

int ReadEnvIntOrDefault(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

}  // namespace

Application::Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                         const std::string& aliyun_api_key, int protocolVersion, int sample_rate, int channels,
                         int frame_duration)
    : Application(address, port, token, deviceId, aliyun_api_key, protocolVersion, sample_rate, channels,
                  frame_duration, "./model/yolov5.rknn") {}

Application::Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                         const std::string& aliyun_api_key, int protocolVersion, int sample_rate, int channels,
                         int frame_duration, const std::string& vision_model_path)
    : Application(address, port, token, deviceId, aliyun_api_key, protocolVersion,
                  sample_rate, channels, frame_duration, vision_model_path, "") {}

Application::Application(const std::string& address, int port, const std::string& token, const std::string& deviceId,
                         const std::string& aliyun_api_key, int protocolVersion, int sample_rate, int channels,
                         int frame_duration, const std::string& vision_model_path,
                         const std::string& face_model_path)
    : ws_client_(address, port, token, deviceId, protocolVersion),
      aliyun_api_key_(aliyun_api_key),
      ws_protocolVersion_(protocolVersion),
      client_state_(static_cast<int>(AppState::startup)),
      audio_processor_(sample_rate, channels, frame_duration),
      vision_model_path_(vision_model_path),
      face_detect_model_path_(face_model_path),
      active_vision_default_cooldown_ms_(ReadEnvIntOrDefault("AICHAT_ACTIVE_VISION_COOLDOWN_MS", 5000)),
      vision_context_ttl_ms_(ReadEnvIntOrDefault("AICHAT_VISION_CONTEXT_TTL_MS", 15000)) {
    printf("Application init start\n");

    StartCameraV2(vision_model_path_, face_detect_model_path_, face_interval_);

    ws_client_.SetMessageCallback([this](const std::string& message, bool is_binary) {
        app_handler.ws_msg_handle(message, is_binary, this);
    });
    ws_client_.SetCloseCallback([this]() {
        eventQueue_.Enqueue(static_cast<int>(AppEvent::fault_happen));
    });
}

Application::~Application() {
    threads_stop_flag_.store(true);
    if (ws_msg_thread_.joinable()) ws_msg_thread_.join();
    if (state_trans_thread_.joinable()) state_trans_thread_.join();
    StopCamera();
    USER_LOG_WARN("Application destruct.");
}

#ifdef USE_OPENCV_CAMERA
bool Application::StartCamera(const std::string& model_path) {
    const int camera_result = start_ai_camera(model_path.c_str(), this);
    if (camera_result == 0) {
        owns_camera_ = true;
    } else if (camera_result == 1) {
        owns_camera_ = false;
        attach_ai_camera_consumer(this);
        USER_LOG_INFO("AI camera already running; Application attached as consumer.");
    } else {
        owns_camera_ = false;
        USER_LOG_WARN("AI camera did not start from Application.");
        return false;
    }

    if (frame_buffer_.WaitFirstFrame(1500)) {
        printf("AI camera ready\n");
    } else {
        USER_LOG_WARN("AI camera started but no frame arrived within timeout.");
    }

    // 自动加载人脸检测模型
    if (!face_detect_model_path_.empty()) {
        if (load_face_detect_model(face_detect_model_path_.c_str()) == 0) {
            USER_LOG_INFO("Face detect model loaded: %s", face_detect_model_path_.c_str());
        } else {
            USER_LOG_WARN("Failed to load face detect model: %s", face_detect_model_path_.c_str());
        }
    }

    return true;
}
#endif

bool Application::StartCameraV2(const std::string& yolo_path,
                                 const std::string& face_path,
                                 int face_interval) {
    const int camera_result = start_ai_camera_v2(
        yolo_path.c_str(), face_path.c_str(), this, 1, face_interval);

    if (camera_result == 0) {
        owns_camera_ = true;
    } else if (camera_result == 1) {
        owns_camera_ = false;
        attach_ai_camera_consumer(this);
        USER_LOG_INFO("AI camera V2 already running; Application attached as consumer.");
    } else {
        owns_camera_ = false;
        USER_LOG_WARN("AI camera V2 did not start from Application.");
        return false;
    }

    if (frame_buffer_.WaitFirstFrame(1500)) {
        printf("AI camera V2 ready\n");
    } else {
        USER_LOG_WARN("AI camera V2 started but no frame arrived within timeout.");
    }

    return true;
}

void Application::StopCamera() {
    detach_ai_camera_consumer(this);
    if (owns_camera_) {
        stop_ai_camera();
        owns_camera_ = false;
    }
}

void Application::UpdateLatestFrame(const cv::Mat& frame, int fd) {
    frame_buffer_.Push(frame, fd);
}

cv::Mat Application::GetLatestFrame() {
    FrameSnapshot snapshot;
    if (!frame_buffer_.GetLatest(snapshot, 500)) {
        return cv::Mat();
    }
    return snapshot.image;
}

bool Application::GetLatestFrameSnapshot(FrameSnapshot& snapshot, int max_age_ms) {
    return frame_buffer_.GetLatest(snapshot, max_age_ms);
}

int64_t Application::NowMs() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now().time_since_epoch())
        .count();
}

std::string Application::NextVisionRequestId() {
    const uint64_t id = ++vision_request_counter_;
    std::ostringstream oss;
    oss << "vision-" << id;
    return oss.str();
}

void Application::SetActiveVisionRequest(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(vision_request_mutex_);
    active_vision_request_id_ = request_id;
}

bool Application::IsActiveVisionRequest(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(vision_request_mutex_);
    return !request_id.empty() && request_id == active_vision_request_id_;
}

void Application::ClearActiveVisionRequest(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(vision_request_mutex_);
    if (request_id.empty() || request_id == active_vision_request_id_) {
        active_vision_request_id_.clear();
    }
}

bool Application::IsVisionContextFreshLocked(int64_t now_ms) const {
    return latest_vision_context_.valid &&
           (vision_context_ttl_ms_ <= 0 ||
            now_ms - latest_vision_context_.updated_ms <= vision_context_ttl_ms_);
}

std::string Application::BuildVisionPrompt(const std::string& prompt, const std::string& source) const {
    std::lock_guard<std::mutex> lock(vision_context_mutex_);
    const int64_t now_ms = NowMs();
    if (!IsVisionContextFreshLocked(now_ms)) {
        return prompt;
    }

    if (source != "passive_voice" && source != "legacy") {
        return prompt;
    }

    std::ostringstream oss;
    oss << prompt
        << "\n\nRecent visual context (reuse only if it helps answer follow-up references such as left/right/that one): "
        << latest_vision_context_.result_text
        << "\nRecent frame_seq=" << latest_vision_context_.frame_seq
        << ", captured_ms=" << latest_vision_context_.captured_ms << ".";
    return oss.str();
}

bool Application::ShouldAcceptIncomingAudio() const {
    return accept_incoming_audio_.load();
}

void Application::SetAcceptIncomingAudio(bool accept) {
    accept_incoming_audio_.store(accept);
    if (!accept) {
        audio_processor_.clearPlaybackAudioQueue();
    }
}

bool Application::SubmitVisionQuestion(const std::string& prompt, const std::string& source) {
    FrameSnapshot snapshot;
    if (!GetLatestFrameSnapshot(snapshot, 500)) {
        USER_LOG_WARN("No fresh camera frame available for vision request.");
        return false;
    }

    const std::string request_id = NextVisionRequestId();
    SetActiveVisionRequest(request_id);
    const std::string final_prompt = BuildVisionPrompt(prompt, source);
    DescribeCurrentScene(snapshot.image, request_id, source, snapshot.seq, snapshot.captured_ms, final_prompt, snapshot.fd);
    return true;
}

bool Application::SubmitVisionEvent(const VisionEvent& event) {
    const int64_t now_ms = event.occurred_ms > 0 ? event.occurred_ms : NowMs();
    if (client_state_.GetCurrentState() != static_cast<int>(AppState::idle)) {
        USER_LOG_INFO("Suppress vision event while app is busy. event=%s", event.event_type.c_str());
        return false;
    }

    const int cooldown_ms = event.cooldown_ms > 0 ? event.cooldown_ms : active_vision_default_cooldown_ms_;
    {
        std::lock_guard<std::mutex> lock(active_vision_mutex_);
        if (now_ms - last_active_vision_ms_ < cooldown_ms) {
            return false;
        }
        last_active_vision_ms_ = now_ms;
    }

    std::string prompt = event.prompt;
    if (prompt.empty()) {
        std::ostringstream oss;
        oss << "You are a desktop robot. Visual event detected: " << event.event_type
            << ", confidence=" << event.confidence
            << ". Please describe the current frame with one short Chinese reminder.";
        prompt = oss.str();
    }

    QueuePendingVisionQuestion(prompt, event.source);
    eventQueue_.Enqueue(static_cast<int>(AppEvent::vision_detected));
    return true;
}

bool Application::SubmitActiveVisionEvent(const std::string& event_label, float confidence, int cooldown_ms) {
    VisionEvent event;
    event.event_type = event_label;
    event.source = "active_opencv";
    event.confidence = confidence;
    event.cooldown_ms = cooldown_ms;
    event.occurred_ms = NowMs();
    return SubmitVisionEvent(event);
}

void Application::QueuePendingVisionQuestion(const std::string& prompt, const std::string& source) {
    std::lock_guard<std::mutex> lock(pending_vision_mutex_);
    pending_vision_question_.prompt = prompt;
    pending_vision_question_.source = source;
    pending_vision_question_.valid = true;
}

bool Application::ConsumePendingVisionQuestion(PendingVisionQuestion& question) {
    std::lock_guard<std::mutex> lock(pending_vision_mutex_);
    if (!pending_vision_question_.valid) {
        return false;
    }

    question = pending_vision_question_;
    pending_vision_question_ = PendingVisionQuestion{};
    return true;
}

void Application::DescribeCurrentScene(const cv::Mat& image) {
    const std::string request_id = NextVisionRequestId();
    SetActiveVisionRequest(request_id);
    DescribeCurrentScene(image, request_id, "legacy", 0, 0, "Describe the current camera frame.");
}

void Application::DescribeCurrentScene(const cv::Mat& image,
                                       const std::string& request_id,
                                       const std::string& source,
                                       uint64_t frame_seq,
                                       int64_t captured_ms,
                                       const std::string& prompt,
                                       int frame_fd) {
    if (!ws_client_.IsConnected()) {
        USER_LOG_WARN("WebSocket not connected, cannot describe scene.");
        return;
    }

    if (image.empty()) {
        USER_LOG_WARN("Image is empty, cannot send to cloud.");
        return;
    }

    std::string img_base64;

    // Try HW encoder path (RGA + MPP) if CMA fd is available
    if (frame_fd >= 0) {
        uint8_t* jpeg_data = NULL;
        size_t   jpeg_size = 0;
        int hw_ret = hw_jpeg_encode(frame_fd, &jpeg_data, &jpeg_size);
        if (hw_ret > 0 && jpeg_data && jpeg_size > 0) {
            img_base64 = base64_encode(jpeg_data, (unsigned int)jpeg_size);
            USER_LOG_INFO("HW JPEG: %zu bytes (fd=%d)", jpeg_size, frame_fd);
        } else {
            USER_LOG_WARN("HW JPEG encode failed ret=%d, fallback to CPU", hw_ret);
        }
    }

    // CPU fallback
    if (img_base64.empty()) {
        cv::Mat small_image;
        cv::resize(image, small_image, cv::Size(448, 448));
        std::vector<uchar> buf;
        cv::imencode(".jpg", small_image, buf);
        img_base64 = base64_encode(buf.data(), buf.size());
    }

    Json::Value msg;
    msg["type"] = "describe_scene";
    msg["request_id"] = request_id;
    msg["source"] = source;
    msg["frame_seq"] = Json::UInt64(frame_seq);
    msg["captured_ms"] = Json::Int64(captured_ms);
    msg["prompt"] = prompt;
    msg["image"] = img_base64;

    Json::FastWriter writer;
    ws_client_.SendText(writer.write(msg));
    USER_LOG_INFO("Sent scene description request. request_id=%s source=%s frame_seq=%llu",
                  request_id.c_str(), source.c_str(), static_cast<unsigned long long>(frame_seq));
}

void Application::UpdateVisionContext(const std::string& request_id,
                                      const std::string& source,
                                      const std::string& prompt,
                                      const std::string& result_text,
                                      uint64_t frame_seq,
                                      int64_t captured_ms) {
    if (result_text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(vision_context_mutex_);
    latest_vision_context_.request_id = request_id;
    latest_vision_context_.source = source;
    latest_vision_context_.prompt = prompt;
    latest_vision_context_.result_text = result_text;
    latest_vision_context_.frame_seq = frame_seq;
    latest_vision_context_.captured_ms = captured_ms;
    latest_vision_context_.updated_ms = NowMs();
    latest_vision_context_.valid = true;
}

VisionContext Application::GetVisionContext() const {
    std::lock_guard<std::mutex> lock(vision_context_mutex_);
    return latest_vision_context_;
}

void Application::InterruptTTS(const std::string& reason) {
    USER_LOG_WARN("Interrupt current TTS playback. reason=%s", reason.c_str());
    SetAcceptIncomingAudio(false);
    audio_processor_.clearPlaybackAudioQueue();
    set_tts_completed(true);
}

void Application::Run() {
    state_trans_thread_ = std::thread([this]() {
        StateConfig::Configure(client_state_, this);
        while (!threads_stop_flag_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!eventQueue_.IsEmpty()) {
                if (auto event_opt = eventQueue_.Dequeue(); event_opt) {
                    client_state_.HandleEvent(event_opt.value());
                }
            }
        }
    });

    state_trans_thread_.join();
    if (ws_client_.IsConnected()) {
        ws_client_.Close();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    USER_LOG_WARN("ai chat app run end");
}
