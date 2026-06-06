#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visioncast {

struct VideoFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t pts_us = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    int vertical_stride = 0;
    std::string format;
    std::uint64_t sequence = 0;
};

}  // namespace visioncast
