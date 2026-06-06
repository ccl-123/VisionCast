#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/types.h"
#include "media/audio_frame.h"

namespace visioncast {

class AudioCapture {
public:
    using FrameCallback = std::function<void(AudioFrame&&)>;

    explicit AudioCapture(AudioConfig config);
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    AudioProbeResult probe() const;
    static AudioProbeResult probe_device(const std::string& device);

    bool start(FrameCallback callback);
    void stop();

    bool is_running() const;
    std::string error() const;

private:
    void capture_loop();
    void finish_initialization(bool success, const std::string& error = {});

    AudioConfig config_;
    FrameCallback callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;
    bool initialization_done_ = false;
    bool initialization_success_ = false;
    std::string error_;
};

}  // namespace visioncast
