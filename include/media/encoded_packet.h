/**
 * @file encoded_packet.h
 * @brief VisionCast 编码包数据结构头文件
 * @details 定义了媒体类型的枚举（音频/视频）以及表示编码后压缩媒体包的结构体（EncodedPacket），
 *          包含编码数据、时间戳、关键帧标志和 RTP 时间戳等元数据，用于网络流媒体传输。
 */

#pragma once

#include <cstdint>
#include <vector>

namespace visioncast {

/**
 * @enum MediaType
 * @brief 媒体数据类型枚举
 */
enum class MediaType {
    Video,  // 视频媒体
    Audio,  // 音频媒体
};

/**
 * @struct EncodedPacket
 * @brief 表示编码压缩后的媒体数据包（如 H.264/H.265 NAL 单元或 G.711 音频包）
 */
struct EncodedPacket {
    MediaType media_type = MediaType::Video; // 媒体类型，默认视频
    std::vector<std::uint8_t> data;          // 编码后的媒体二进制数据缓冲区
    std::uint64_t pts_us = 0;                // 演示时间戳（Presentation Time Stamp），单位微秒
    bool key_frame = false;                  // 是否为关键帧（例如视频的 I 帧，音频包通常为 false 或忽略）
    std::uint32_t rtp_timestamp = 0;         // 对应的 RTP 时间戳（根据流的时钟基计算得到）
};

}  // namespace visioncast
