#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <memory>
#include <mutex>

#include "common/blocking_queue.h"
#include "common/types.h"
#include "media/audio_capture.h"
#include "media/audio_frame.h"
#include "transport/av_transport.h"

namespace visioncast {

class AudioPipeline {
public:
    AudioPipeline(VisionCastConfig config, std::shared_ptr<AvTransport> transport);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool start(std::string& error);
    void stop();

    bool is_running() const;
    std::string error() const;
    std::size_t dropped_frames() const;

private:
    void worker_loop();

    VisionCastConfig config_;
    AudioCapture capture_;
    std::shared_ptr<AvTransport> transport_;
    BoundedBlockingQueue<AudioFrame> raw_queue_{8};
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace visioncast
