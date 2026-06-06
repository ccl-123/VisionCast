#pragma once

#include <cstdint>
#include <vector>

namespace visioncast {

struct AudioFrame {
    std::vector<std::uint8_t> pcm;
    std::uint64_t pts_us = 0;
    int sample_rate = 48000;
    int channels = 1;
    std::uint64_t sequence = 0;
};

}  // namespace visioncast
