#pragma once

#include <cstdint>

namespace visioncast {

class MediaClock {
public:
    static std::uint64_t now_us();
    static std::uint32_t video_rtp_timestamp(std::uint64_t pts_us);
    static std::uint32_t audio_rtp_timestamp(std::uint64_t pts_us, int sample_rate);
};

}  // namespace visioncast
