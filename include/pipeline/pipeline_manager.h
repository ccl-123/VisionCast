#pragma once

#include "common/types.h"
#include "media/audio_capture.h"
#include "media/video_capture.h"

namespace visioncast {

struct PipelineProbeSummary {
    VideoProbeResult primary_video;
    VideoProbeResult fallback_video;
    AudioProbeResult audio;
    bool fallback_checked = false;
    bool video_ready = false;
    bool audio_ready = false;
};

class PipelineManager {
public:
    explicit PipelineManager(VisionCastConfig config);

    void print_config() const;
    PipelineProbeSummary probe_devices() const;
    int run_probe(bool require_devices) const;
    int run_stream();
    bool write_sdp_file(std::string& error) const;

private:
    VisionCastConfig config_;
};

}  // namespace visioncast
