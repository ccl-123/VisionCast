#include "pipeline/av_pipeline.h"

#include <utility>

namespace visioncast {

AvPipeline::AvPipeline(VisionCastConfig config)
    : config_(std::move(config)),
      transport_(std::make_shared<AvTransport>(config_)),
      video_(config_, transport_),
      audio_(config_, transport_) {}

AvPipeline::~AvPipeline() {
    stop();
}

bool AvPipeline::start(std::string& error) {
    if (started_) {
        return true;
    }
    if (!transport_->open(error)) {
        return false;
    }
    if (!video_.start(error)) {
        transport_->close();
        return false;
    }
    if (!audio_.start(error)) {
        video_.stop();
        transport_->close();
        return false;
    }
    started_ = true;
    return true;
}

void AvPipeline::stop() {
    audio_.stop();
    video_.stop();
    transport_->close();
    started_ = false;
}

bool AvPipeline::is_healthy() const {
    return started_ && video_.is_running() && audio_.is_running();
}

std::string AvPipeline::error() const {
    const std::string video_error = video_.error();
    if (!video_error.empty()) {
        return "video pipeline: " + video_error;
    }
    const std::string audio_error = audio_.error();
    if (!audio_error.empty()) {
        return "audio pipeline: " + audio_error;
    }
    return "media pipeline stopped unexpectedly";
}

}  // namespace visioncast
