/**
 * @file audio_encoder.cpp
 * @brief VisionCast 音频编码器实现文件
 * @details 实现将 S16_LE PCM 编码为 Opus，并维护跨帧复用的编码器状态。
 */

#include "media/audio_encoder.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "media/media_clock.h"

#if defined(VISIONCAST_ENABLE_OPUS)
#include "opus.h"
#endif

namespace visioncast {
namespace {

constexpr std::size_t kBytesPerSample = 2;

// 辅助函数：以小端字节序读取 16 位有符号整数
std::int16_t read_s16le(const std::uint8_t* data) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

bool is_supported_opus_sample_rate(int sample_rate) {
    return sample_rate == 8000 ||
           sample_rate == 12000 ||
           sample_rate == 16000 ||
           sample_rate == 24000 ||
           sample_rate == 48000;
}

bool is_supported_opus_frame_size(int sample_rate, std::size_t samples_per_channel) {
    const int valid_sizes[] = {
        sample_rate / 400,       // 2.5 ms
        sample_rate / 200,       // 5 ms
        sample_rate / 100,       // 10 ms
        sample_rate / 50,        // 20 ms
        sample_rate / 25,        // 40 ms
        (sample_rate * 3) / 50,  // 60 ms
    };
    return std::any_of(std::begin(valid_sizes), std::end(valid_sizes), [samples_per_channel](int size) {
        return size > 0 && samples_per_channel == static_cast<std::size_t>(size);
    });
}

}  // namespace

AudioEncoder::AudioEncoder(int frame_ms)
    : opus_frame_ms_(frame_ms > 0 ? frame_ms : 20) {}

AudioEncoder::~AudioEncoder() {
    destroy_opus_encoder();
}

EncodedPacket AudioEncoder::encode(const AudioFrame& frame, std::string& error) {
    EncodedPacket packet;
    packet.media_type = MediaType::Audio;
    packet.pts_us = frame.pts_us;
    packet.rtp_timestamp = MediaClock::audio_rtp_timestamp(frame.pts_us, kOpusRtpClockRate);

#if !defined(VISIONCAST_ENABLE_OPUS)
    (void)frame;
    error = "Opus support is not enabled in this build";
    return packet;
#else
    if (frame.sample_rate <= 0 || !is_supported_opus_sample_rate(frame.sample_rate)) {
        error = "unsupported Opus input sample rate: " + std::to_string(frame.sample_rate);
        return packet;
    }

    if (frame.channels != 1 && frame.channels != 2) {
        error = "unsupported Opus input channel count: " + std::to_string(frame.channels);
        return packet;
    }

    const std::size_t channels = static_cast<std::size_t>(frame.channels);
    const std::size_t bytes_per_sample_frame = kBytesPerSample * channels;
    if (frame.pcm.empty() || frame.pcm.size() % bytes_per_sample_frame != 0) {
        error = "invalid S16_LE PCM buffer size for Opus encoding";
        return packet;
    }

    const std::size_t samples_per_channel =
        static_cast<std::size_t>(frame.sample_rate * opus_frame_ms_ / 1000);
    if (!is_supported_opus_frame_size(frame.sample_rate, samples_per_channel)) {
        error = "unsupported configured Opus frame duration: " + std::to_string(opus_frame_ms_) +
                " ms at " + std::to_string(frame.sample_rate) + " Hz";
        return packet;
    }

    if (opus_encoder_ != nullptr &&
        (opus_sample_rate_ != frame.sample_rate || opus_channels_ != frame.channels)) {
        pending_pcm_.clear();
        pending_pts_us_ = 0;
    }

    if (!ensure_opus_encoder(frame.sample_rate, frame.channels, error)) {
        return packet;
    }

    if (pending_pcm_.empty()) {
        pending_pts_us_ = frame.pts_us;
    }
    pending_pcm_.insert(pending_pcm_.end(), frame.pcm.begin(), frame.pcm.end());

    const std::size_t encoded_pcm_bytes = samples_per_channel * bytes_per_sample_frame;
    if (pending_pcm_.size() < encoded_pcm_bytes) {
        packet.data.clear();
        error.clear();
        return packet;
    }

    packet.pts_us = pending_pts_us_;
    packet.rtp_timestamp = MediaClock::audio_rtp_timestamp(packet.pts_us, kOpusRtpClockRate);

    std::vector<opus_int16> pcm(samples_per_channel * channels);
    for (std::size_t sample = 0; sample < samples_per_channel * channels; ++sample) {
        const std::size_t offset = sample * kBytesPerSample;
        pcm[sample] = static_cast<opus_int16>(read_s16le(pending_pcm_.data() + offset));
    }
    pending_pcm_.erase(pending_pcm_.begin(),
                       pending_pcm_.begin() + static_cast<std::ptrdiff_t>(encoded_pcm_bytes));
    if (pending_pcm_.empty()) {
        pending_pts_us_ = 0;
    } else {
        pending_pts_us_ += static_cast<std::uint64_t>(opus_frame_ms_) * 1000ULL;
    }

    packet.data.resize(kOpusMaxPacketBytes);
    const opus_int32 encoded_bytes = opus_encode(static_cast<OpusEncoder*>(opus_encoder_),
                                                 pcm.data(),
                                                 static_cast<int>(samples_per_channel),
                                                 packet.data.data(),
                                                 static_cast<opus_int32>(packet.data.size()));
    if (encoded_bytes < 0) {
        error = std::string("opus_encode failed: ") + opus_strerror(encoded_bytes);
        packet.data.clear();
        return packet;
    }
    packet.data.resize(static_cast<std::size_t>(encoded_bytes));
    error.clear();
    return packet;
#endif
}

bool AudioEncoder::ensure_opus_encoder(int sample_rate, int channels, std::string& error) {
#if !defined(VISIONCAST_ENABLE_OPUS)
    (void)sample_rate;
    (void)channels;
    error = "Opus support is not enabled in this build";
    return false;
#else
    if (opus_encoder_ != nullptr && opus_sample_rate_ == sample_rate && opus_channels_ == channels) {
        return true;
    }

    destroy_opus_encoder();

    int opus_error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(sample_rate, channels, OPUS_APPLICATION_VOIP, &opus_error);
    if (opus_error != OPUS_OK || encoder == nullptr) {
        error = std::string("opus_encoder_create failed: ") + opus_strerror(opus_error);
        return false;
    }

    opus_error = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(kOpusDefaultBitrate));
    if (opus_error == OPUS_OK) {
        opus_error = opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
    }
    if (opus_error != OPUS_OK) {
        error = std::string("opus_encoder_ctl failed: ") + opus_strerror(opus_error);
        opus_encoder_destroy(encoder);
        return false;
    }

    opus_encoder_ = encoder;
    opus_sample_rate_ = sample_rate;
    opus_channels_ = channels;
    return true;
#endif
}

void AudioEncoder::destroy_opus_encoder() {
#if defined(VISIONCAST_ENABLE_OPUS)
    if (opus_encoder_ != nullptr) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(opus_encoder_));
    }
#endif
    opus_encoder_ = nullptr;
    opus_sample_rate_ = 0;
    opus_channels_ = 0;
    pending_pcm_.clear();
    pending_pts_us_ = 0;
}

}  // namespace visioncast
