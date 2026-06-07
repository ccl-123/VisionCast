/**
 * @file audio_encoder.h
 * @brief VisionCast 音频编码模块头文件
 * @details 负责将原始音频帧数据（例如 S16_LE PCM）编码成压缩格式的音频包（例如 G.711 PCMA），
 *          以便在网络（如 RTSP/RTP）中进行高效传输。
 */

#pragma once

#include <cstdint>

#include "media/audio_frame.h"
#include "media/encoded_packet.h"

namespace visioncast {

/**
 * @class AudioEncoder
 * @brief 音频编码器类，目前支持 G.711 A-law 算法编码
 */
class AudioEncoder {
public:
    // Encodes S16_LE PCM to ITU-T G.711 A-law (RTP payload type 8 / PCMA).
    // 将 S16_LE PCM 原始音频帧编码为 ITU-T G.711 A-law 格式（RTP 载荷类型为 8 / PCMA）
    EncodedPacket encode_pcma(const AudioFrame& frame) const;

private:
    // 将 16位线性 PCM 样本转换为 8位 A-law 样本的静态辅助函数
    static std::uint8_t linear_to_alaw(std::int16_t sample);
};

}  // namespace visioncast
