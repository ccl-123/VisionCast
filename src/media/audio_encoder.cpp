#include "media/audio_encoder.h"

#include <algorithm>
#include <cstddef>

#include "media/media_clock.h"

namespace visioncast {
namespace {

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

std::uint8_t AudioEncoder::linear_to_alaw(std::int16_t sample) {
    constexpr std::int16_t clip = 32635;
    std::uint8_t sign = 0x80;
    if (sample < 0) {
        sign = 0x00;
        sample = static_cast<std::int16_t>(-sample - 1);
    }
    if (sample > clip) {
        sample = clip;
    }

    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        --exponent;
    }

    const int mantissa = exponent == 0 ? (sample >> 4) & 0x0F : (sample >> (exponent + 3)) & 0x0F;
    return static_cast<std::uint8_t>((sign | (exponent << 4) | mantissa) ^ 0x55);
}

}  // namespace visioncast
