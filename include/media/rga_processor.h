#pragma once

#include <memory>
#include <string>

#include "media/mjpeg_decoder.h"
#include "media/video_frame.h"

namespace visioncast {

class RgaProcessor {
public:
    RgaProcessor(int output_width, int output_height);

    // Converts incoming V4L2 frames to NV12 and scales to the configured output.
    bool process(const VideoFrame& input, VideoFrame& output, std::string& error);

private:
    MjpegDecoder mjpeg_decoder_;
    int output_width_;
    int output_height_;
};

}  // namespace visioncast
