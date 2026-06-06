#pragma once

#include <memory>
#include <string>

#include "common/types.h"
#include "media/encoded_packet.h"
#include "media/video_frame.h"

namespace visioncast {

class MppEncoder {
public:
    MppEncoder(VideoConfig video, EncoderConfig encoder);
    ~MppEncoder();

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    bool open(std::string& error);
    void close();
    bool encode(const VideoFrame& frame, EncodedPacket& packet, std::string& error);
    static std::string backend_version();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VideoConfig video_;
    EncoderConfig encoder_;
};

}  // namespace visioncast
