#pragma once

#include <cstdint>
#include <string>

namespace visioncast {

std::uint64_t monotonic_now_us();
std::string format_duration_us(std::uint64_t duration_us);

}  // namespace visioncast
