#pragma once

#include <memory>
#include <string>

#include "media/video_frame.h"

namespace visioncast {

class MjpegDecoder {
public:
    struct Impl;

    MjpegDecoder();
    ~MjpegDecoder();

    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;

    // Decodes one complete V4L2 MJPEG image to NV12.
    bool decode(const VideoFrame& input, VideoFrame& output, std::string& error);

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace visioncast
