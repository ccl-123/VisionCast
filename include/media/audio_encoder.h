/**
 * @file audio_encoder.h
 * @brief VisionCast 音频编码模块头文件
 * @details 负责将 S16_LE PCM 编码为 RTP/WebRTC/RTSP 使用的 Opus 音频包。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "media/audio_codec.h"
#include "media/audio_frame.h"
#include "media/encoded_packet.h"

namespace visioncast {

/**
 * @class AudioEncoder
 * @brief 有状态的 Opus 音频编码器
 */
class AudioEncoder {
public:
    explicit AudioEncoder(int frame_ms);
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;

    // Encodes S16_LE PCM to Opus for RTP/WebRTC/RTSP audio.
    // 将 S16_LE PCM 原始音频帧编码为 Opus，用于 RTP/WebRTC/RTSP 音频传输。
    EncodedPacket encode(const AudioFrame& frame, std::string& error);

private:
    bool ensure_opus_encoder(int sample_rate, int channels, std::string& error);
    void destroy_opus_encoder();

    void* opus_encoder_ = nullptr;
    int opus_sample_rate_ = 0;
    int opus_channels_ = 0;
    int opus_frame_ms_ = 20;
    std::vector<std::uint8_t> pending_pcm_;
    std::size_t pending_pcm_offset_ = 0;
    std::vector<std::int16_t> opus_pcm_;
    std::uint64_t pending_pts_us_ = 0;
};

}  // namespace visioncast
