#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "common/blocking_queue.h"
#include "media/video_frame.h"

namespace visioncast {

class DisplayRenderer {
public:
    explicit DisplayRenderer(std::string window_name = "VisionCast");
    ~DisplayRenderer();

    DisplayRenderer(const DisplayRenderer&) = delete;
    DisplayRenderer& operator=(const DisplayRenderer&) = delete;

    bool start();
    void stop();
    void submit(VideoFrame frame);

private:
    void render_loop();

    std::string window_name_;
    BoundedBlockingQueue<VideoFrame> queue_{1};
    std::thread thread_;
    std::atomic<bool> running_{false};
};

}  // namespace visioncast
