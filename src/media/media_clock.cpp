/**
 * @file media_clock.cpp
 * @brief VisionCast 媒体时钟及时间戳转换工具实现文件
 * @details 实现了基于单调时钟的时间获取，以及音视频帧展示时间戳 (PTS) 到 RTP 传输时间戳的数学映射。
 */

#include "common/time_utils.h"
#include "media/media_clock.h"

namespace visioncast {

// 获取当前的高精度系统单调递增时间戳（微秒）
std::uint64_t MediaClock::now_us() {
    return monotonic_now_us();
}

// 将微秒级的视频 PTS 转换为标准 90kHz 时钟基准下的 RTP 时间戳
std::uint32_t MediaClock::video_rtp_timestamp(std::uint64_t pts_us) {
    return static_cast<std::uint32_t>((pts_us * 90000ULL) / 1000000ULL);
}

// 将微秒级的音频 PTS 转换为指定采样率时钟基准下的 RTP 时间戳（如 8000Hz）
std::uint32_t MediaClock::audio_rtp_timestamp(std::uint64_t pts_us, int sample_rate) {
    return static_cast<std::uint32_t>((pts_us * static_cast<std::uint64_t>(sample_rate)) / 1000000ULL);
}

}  // namespace visioncast
