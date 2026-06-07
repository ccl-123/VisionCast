/**
 * @file audio_encoder.cpp
 * @brief VisionCast 音频编码器实现文件
 * @details 实现了将 16位有符号 PCM 音频数据编码为 G.711 A-law (PCMA) 算法。
 *          包含了从 48kHz (或其他采样率) 到 8kHz 的降采样逻辑，以适配标准的 PCMA 音频传输规范。
 */

#include "media/audio_encoder.h"

#include <algorithm>
#include <cstddef>

#include "media/media_clock.h"

namespace visioncast {
namespace {

// 辅助函数：以小端字节序读取 16 位有符号整数
std::int16_t read_s16le(const std::uint8_t* data) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

}  // namespace

EncodedPacket AudioEncoder::encode_pcma(const AudioFrame& frame) const {
    EncodedPacket packet;
    packet.media_type = MediaType::Audio;
    packet.pts_us = frame.pts_us;
    packet.rtp_timestamp = MediaClock::audio_rtp_timestamp(frame.pts_us, 8000);

    const std::size_t bytes_per_sample = 2;
    const std::size_t sample_count = frame.pcm.size() / bytes_per_sample /
                                     static_cast<std::size_t>(std::max(1, frame.channels));
    packet.data.reserve(sample_count);

    // The SDP advertises PCMA/8000/1, so downsample 48 kHz capture to 8 kHz by
    // keeping every sixth mono sample. Stereo input is mixed by selecting left.
    // SDP 声明为 PCMA/8000/1，因此通过每6个样本抽取1个的方式将 48 kHz 降采样至 8 kHz。
    // 如果输入是双声道，仅选择左声道样本，右声道丢弃。
    const int input_rate = std::max(1, frame.sample_rate);
    const int step = std::max(1, input_rate / 8000);
    const std::size_t channels = static_cast<std::size_t>(std::max(1, frame.channels));
    for (std::size_t sample = 0; sample < sample_count; sample += static_cast<std::size_t>(step)) {
        const std::size_t offset = sample * channels * bytes_per_sample;
        if (offset + 1 >= frame.pcm.size()) {
            break;
        }
        packet.data.push_back(linear_to_alaw(read_s16le(frame.pcm.data() + offset)));
    }

    return packet;
}

// 16位有符号 PCM 到 8位 G.711 A-law 算法转换实现
std::uint8_t AudioEncoder::linear_to_alaw(std::int16_t sample) {
    constexpr std::int16_t clip = 32635;
    std::uint8_t sign = 0x80;
    // 1. 处理符号位
    if (sample < 0) {
        sign = 0x00;
        sample = static_cast<std::int16_t>(-sample - 1);
    }
    // 2. 限幅
    if (sample > clip) {
        sample = clip;
    }

    // 3. 寻找最高非零位，确定指数 (exponent)
    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        --exponent;
    }

    // 4. 根据指数提取 4 位尾数 (mantissa)
    const int mantissa = exponent == 0 ? (sample >> 4) & 0x0F : (sample >> (exponent + 3)) & 0x0F;
    // 5. 将符号、指数、尾数组合，并按标准与 0x55 异或后输出
    return static_cast<std::uint8_t>((sign | (exponent << 4) | mantissa) ^ 0x55);
}

}  // namespace visioncast
