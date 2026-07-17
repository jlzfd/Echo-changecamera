#ifndef VISION_FRAME_BUFFER_H
#define VISION_FRAME_BUFFER_H

#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <opencv2/core/core.hpp>

struct FrameSnapshot {
    cv::Mat image;
    uint64_t seq = 0;
    int64_t captured_ms = 0;
    int fd = -1;
    bool valid = false;
};

class VisionFrameBuffer {
public:
    void Push(const cv::Mat& frame, int fd = -1);
    bool GetLatest(FrameSnapshot& out, int max_age_ms) const;
    bool WaitFirstFrame(int timeout_ms);

private:
    static int64_t NowMs();

    mutable std::mutex mutex_;
    std::condition_variable frame_ready_cv_;
    FrameSnapshot latest_;
    uint64_t next_seq_ = 0;
    bool ready_ = false;
};

#endif  // VISION_FRAME_BUFFER_H
