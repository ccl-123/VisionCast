#include "transport/av_transport.h"

#include <utility>

namespace visioncast {

AvTransport::AvTransport(VisionCastConfig config)
    : config_(std::move(config)),
      video_udp_(config_.stream.server_ip, config_.stream.video_port),
      audio_udp_(config_.stream.server_ip, config_.stream.audio_port),
      video_rtp_(96, 0x56435631U),
      audio_rtp_(8, 0x56434131U),
      rtmp_(config_.stream.rtmp_url, config_.video, config_.audio),
      webrtc_(config_.stream.webrtc_url, config_.video, config_.audio) {}

AvTransport::~AvTransport() {
    close();
}

bool AvTransport::open(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
        return true;
    }
    if (config_.stream.protocol == "rtp") {
        if (!video_udp_.open(error) || !audio_udp_.open(error)) {
            video_udp_.close();
            audio_udp_.close();
            return false;
        }
    } else if (config_.stream.protocol == "rtmp") {
        if (!rtmp_.connect(error)) {
            return false;
        }
    } else if (config_.stream.protocol == "webrtc") {
        if (!webrtc_.connect(error)) {
            return false;
        }
    } else {
        error = "unsupported stream protocol: " + config_.stream.protocol;
        return false;
    }
    opened_ = true;
    return true;
}

void AvTransport::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    video_udp_.close();
    audio_udp_.close();
    rtmp_.disconnect();
    webrtc_.disconnect();
    opened_ = false;
}

bool AvTransport::send_video(const EncodedPacket& packet, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        error = "transport is not open";
        return false;
    }
    if (config_.stream.protocol == "rtp") {
        for (const auto& rtp : video_rtp_.packetize(packet)) {
            if (!video_udp_.send(rtp.bytes.data(), rtp.bytes.size(), error)) {
                return false;
            }
        }
        return true;
    }
    if (config_.stream.protocol == "webrtc") {
        return webrtc_.push_video_rtp(video_rtp_.packetize(packet), error);
    }
    return rtmp_.push_video(packet.data.data(), packet.data.size(), packet.pts_us, error);
}

bool AvTransport::send_audio(const AudioFrame& frame, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        error = "transport is not open";
        return false;
    }
    if (config_.stream.protocol == "rtmp") {
        return rtmp_.push_audio(
            frame.pcm.data(), frame.pcm.size(), frame.pts_us, error);
    }

    const EncodedPacket encoded = audio_encoder_.encode_pcma(frame);
    if (config_.stream.protocol == "rtp") {
        for (const auto& rtp : audio_rtp_.packetize(encoded)) {
            if (!audio_udp_.send(rtp.bytes.data(), rtp.bytes.size(), error)) {
                return false;
            }
        }
        return true;
    }
    if (config_.stream.protocol == "webrtc") {
        return webrtc_.push_audio_rtp(audio_rtp_.packetize(encoded), error);
    }
    error = "unsupported stream protocol: " + config_.stream.protocol;
    return false;
}

}  // namespace visioncast
