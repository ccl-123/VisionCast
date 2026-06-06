#pragma once

#include <cstdint>
#include <vector>

namespace visioncast {

enum class MediaType {
    Video,
    Audio,
};

struct EncodedPacket {
    MediaType media_type = MediaType::Video;
    std::vector<std::uint8_t> data;
    std::uint64_t pts_us = 0;
    bool key_frame = false;
    std::uint32_t rtp_timestamp = 0;
};

}  // namespace visioncast
