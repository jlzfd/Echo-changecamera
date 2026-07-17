#include "VisionFrameBuffer.h"

#include <chrono>

int64_t VisionFrameBuffer::NowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock::now().time_since_epoch())
        .count();
}

void VisionFrameBuffer::Push(const cv::Mat& frame, int fd) {
    if (frame.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.image = frame;  // shared refcount, no deep copy
        latest_.seq = ++next_seq_;
        latest_.captured_ms = NowMs();
        latest_.fd = fd;
        latest_.valid = true;
        ready_ = true;
    }
    frame_ready_cv_.notify_all();
}

bool VisionFrameBuffer::GetLatest(FrameSnapshot& out, int max_age_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_.valid) {
        return false;
    }

    if (max_age_ms > 0 && NowMs() - latest_.captured_ms > max_age_ms) {
        return false;
    }

    out = latest_;  // shared refcount, no deep copy
    return true;
}

bool VisionFrameBuffer::WaitFirstFrame(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return frame_ready_cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this]() { return ready_ && latest_.valid; });
}
