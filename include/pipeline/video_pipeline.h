#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

#include "common/blocking_queue.h"
#include "common/types.h"
#include "media/display_renderer.h"
#include "media/mpp_encoder.h"
#include "media/rga_processor.h"
#include "media/video_capture.h"
#include "media/video_frame.h"
#include "transport/av_transport.h"

namespace visioncast {

class VideoPipeline {
public:
    VideoPipeline(VisionCastConfig config, std::shared_ptr<AvTransport> transport);
    ~VideoPipeline();

    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    bool start(std::string& error);
    void stop();

    bool is_running() const;
    std::string error() const;
    std::size_t dropped_frames() const;

private:
    void worker_loop();

    VisionCastConfig config_;
    VideoCapture capture_;
    RgaProcessor processor_;
    MppEncoder encoder_;
    std::shared_ptr<AvTransport> transport_;
    DisplayRenderer renderer_;
    BoundedBlockingQueue<VideoFrame> raw_queue_{2};
    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
};

}  // namespace visioncast
