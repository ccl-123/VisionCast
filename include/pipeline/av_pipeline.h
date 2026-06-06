#pragma once

#include <string>
#include <memory>

#include "common/types.h"
#include "pipeline/audio_pipeline.h"
#include "pipeline/video_pipeline.h"
#include "transport/av_transport.h"

namespace visioncast {

class AvPipeline {
public:
    explicit AvPipeline(VisionCastConfig config);
    ~AvPipeline();

    AvPipeline(const AvPipeline&) = delete;
    AvPipeline& operator=(const AvPipeline&) = delete;

    bool start(std::string& error);
    void stop();
    bool is_healthy() const;
    std::string error() const;

private:
    VisionCastConfig config_;
    std::shared_ptr<AvTransport> transport_;
    VideoPipeline video_;
    AudioPipeline audio_;
    bool started_ = false;
};

}  // namespace visioncast
