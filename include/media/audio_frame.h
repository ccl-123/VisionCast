/**
 * @file audio_frame.h
 * @brief VisionCast 音频帧数据结构头文件
 * @details 定义了表示原始音频数据帧（PCM）的结构体，包含音频字节流及采样率、通道数、时间戳等基本元数据。
 */

#pragma once

#include <cstdint>
#include <vector>

namespace visioncast {

/**
 * @struct AudioFrame
 * @brief 表示一帧未压缩的 PCM 音频数据及元信息
 */
struct AudioFrame {
    std::vector<std::uint8_t> pcm;   // 原始 PCM 数据缓冲区
    std::uint64_t pts_us = 0;        // 演示时间戳（Presentation Time Stamp），单位为微秒
    int sample_rate = 48000;         // 采样率（默认 48000Hz）
    int channels = 1;                // 通道数（例如 1 表示单声道，2 表示双声道）
    std::uint64_t sequence = 0;      // 音频帧序列号，用于顺序校验和丢包检测
};

}  // namespace visioncast
