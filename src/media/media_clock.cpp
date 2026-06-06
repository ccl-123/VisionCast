#include "media/media_clock.h"

#include "common/time_utils.h"

namespace visioncast {

std::uint64_t MediaClock::now_us() {
    return monotonic_now_us();
}

std::uint32_t MediaClock::video_rtp_timestamp(std::uint64_t pts_us) {
    return static_cast<std::uint32_t>((pts_us * 90000ULL) / 1000000ULL);
}

std::uint32_t MediaClock::audio_rtp_timestamp(std::uint64_t pts_us, int sample_rate) {
    return static_cast<std::uint32_t>((pts_us * static_cast<std::uint64_t>(sample_rate)) / 1000000ULL);
}

}  // namespace visioncast
