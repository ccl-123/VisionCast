#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "transport/packetizer.h"

namespace visioncast {

class WebRtcPusher {
public:
    WebRtcPusher(std::string whip_url, VideoConfig video, AudioConfig audio);
    ~WebRtcPusher();

    WebRtcPusher(const WebRtcPusher&) = delete;
    WebRtcPusher& operator=(const WebRtcPusher&) = delete;

    bool connect(std::string& error);
    void disconnect();

    bool push_video_rtp(const std::vector<RtpPacket>& packets, std::string& error);
    bool push_audio_rtp(const std::vector<RtpPacket>& packets, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string whip_url_;
    VideoConfig video_;
    AudioConfig audio_;
};

}  // namespace visioncast
