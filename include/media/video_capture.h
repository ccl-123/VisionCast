#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/types.h"
#include "media/video_frame.h"

namespace visioncast {

class VideoCapture {
public:
    using FrameCallback = std::function<void(VideoFrame&&)>;

    explicit VideoCapture(VideoConfig config);
    ~VideoCapture();

    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    VideoProbeResult probe() const;
    static VideoProbeResult probe_device(const std::string& device);

    bool start(FrameCallback callback);
    void stop();

    bool is_running() const;
    std::string active_device() const;
    std::string error() const;

private:
    void capture_loop();
    void finish_initialization(bool success, const std::string& error = {});

    VideoConfig config_;
    FrameCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool initialization_done_ = false;
    bool initialization_success_ = false;
    std::string active_device_;
    std::string error_;
};

}  // namespace visioncast
