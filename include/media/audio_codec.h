/**
 * @file audio_codec.h
 * @brief VisionCast audio codec constants.
 */

#pragma once

#include <cstdint>

namespace visioncast {

constexpr std::uint8_t kOpusPayloadType = 111;
constexpr int kOpusRtpClockRate = 48000;
constexpr int kOpusDefaultBitrate = 64000;
constexpr int kOpusMaxPacketBytes = 4000;

}  // namespace visioncast
