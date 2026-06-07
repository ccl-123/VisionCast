/**
 * @file media_clock.h
 * @brief VisionCast 媒体时钟及时间戳转换工具头文件
 * @details 提供高精度的单调时间戳获取，以及将微秒级 Presentation Time Stamp (PTS)
 *          转换成音视频各自对应 RTP 采样时钟基准下时间戳的静态工具函数。
 */

#pragma once

#include <cstdint>

namespace visioncast {

/**
 * @class MediaClock
 * @brief 媒体时钟类，用于高精度系统时间获取及 RTP 时间戳映射
 */
class MediaClock {
public:
    // 获取当前系统的单调递增时间戳，单位为微秒
    static std::uint64_t now_us();

    // 根据微秒级 PTS 计算视频 RTP 时间戳（标准视频时钟频率通常为 90000Hz）
    static std::uint32_t video_rtp_timestamp(std::uint64_t pts_us);

    // 根据微秒级 PTS 和采样率计算音频 RTP 时间戳（音频时钟频率等于采样率，如 48000Hz）
    static std::uint32_t audio_rtp_timestamp(std::uint64_t pts_us, int sample_rate);
};

}  // namespace visioncast
