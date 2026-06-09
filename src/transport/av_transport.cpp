/**
 * @file av_transport.cpp
 * @brief VisionCast 音视频传输控制模块实现文件
 * 
 * 本文件实现了 AvTransport 类。它负责多路分发控制，包括以下逻辑：
 * 1. 解析系统流式协议配置（RTP/RTMP/WebRTC）。
 * 2. 调度 UDP 发送器、RTP 打包器、RTMP 推流器和 WebRTC 推流器的生命周期。
 * 3. RTP 和 WebRTC 共用 Opus 编码及 RTP 打包链路，RTMP 保持独立 AAC 编码。
 * 4. 使用 std::lock_guard 对各种操作进行同步保护，确保线程安全。
 */

#include "transport/av_transport.h"

#include <utility>

namespace visioncast {

AvTransport::AvTransport(VisionCastConfig config)
    : config_(std::move(config)),
      video_udp_(config_.stream.server_ip, config_.stream.video_port),
      audio_udp_(config_.stream.server_ip, config_.stream.audio_port),
      // 0x56435631U = "VCV1" (VisionCast Video 1), 0x56434131U = "VCA1" (VisionCast Audio 1) 作为 SSRC
      video_rtp_(96, 0x56435631U),
      audio_rtp_(kOpusPayloadType, 0x56434131U),
      audio_encoder_(config_.audio.frame_ms),
      rtmp_(config_.stream.rtmp_url, config_.video, config_.audio, config_.encoder),
      webrtc_(config_.stream.webrtc_url, config_.video, config_.audio, config_.encoder) {}

AvTransport::~AvTransport() {
    close();
}

bool AvTransport::open(std::string& error) {
    // 使用互斥锁保护，防止在多线程下多次执行 open 或 open/close 竞争
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) {
        return true;
    }
    
    // 根据配置的传输协议，初始化对应的通道
    if (config_.stream.protocol == "rtp") {
        // 打开视频和音频的 UDP 发送通道
        if (!video_udp_.open(error) || !audio_udp_.open(error)) {
            video_udp_.close();
            audio_udp_.close();
            return false;
        }
    } else if (config_.stream.protocol == "rtmp") {
        // 连接到远端 RTMP 服务器
        if (!rtmp_.connect(error)) {
            return false;
        }
    } else if (config_.stream.protocol == "webrtc") {
        // 建立 WebRTC (WHIP) 连接
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
    // 线程安全地关闭所有传输资源
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
    
    // 1. RTP 协议发送分支：对 H.264/H.265 包执行 RTP 打包并逐包发送
    if (config_.stream.protocol == "rtp") {
        return video_rtp_.packetize_each(packet, [this, &error](const RtpPacket& rtp) {
            if (!video_udp_.send(rtp.bytes.data(), rtp.bytes.size(), error)) {
                return false;
            }
            return true;
        });
    }
    
    // 2. WebRTC 协议发送分支：通过 FFmpeg 官方原生 WHIP 推送视频编码包
    if (config_.stream.protocol == "webrtc") {
        return webrtc_.push_video(packet, error);
    }
    
    // 3. RTMP 协议发送分支：将编码后的原始 H.264/H.265 帧推送至 RTMP 推流器
    return rtmp_.push_video(packet, error);
}

bool AvTransport::send_audio(const AudioFrame& frame, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        error = "transport is not open";
        return false;
    }
    
    // 1. RTMP 协议发送分支：直接把音频帧数据推送到 RTMP 发送队列中
    if (config_.stream.protocol == "rtmp") {
        return rtmp_.push_audio(
            frame.pcm.data(), frame.pcm.size(), frame.pts_us, error);
    }

    // 2. RTP 和 WebRTC 共用 Opus 编码及 RTP 打包。
    const EncodedPacket encoded = audio_encoder_.encode(frame, error);
    if (!error.empty()) {
        return false;
    }
    if (encoded.data.empty()) {
        return true;
    }
    
    // 2.1 RTP 协议分支：执行 RTP 打包并通过 UDP 音频套接字发送
    if (config_.stream.protocol == "rtp") {
        return audio_rtp_.packetize_each(encoded, [this, &error](const RtpPacket& rtp) {
            if (!audio_udp_.send(rtp.bytes.data(), rtp.bytes.size(), error)) {
                return false;
            }
            return true;
        });
    }
    
    // 2.2 WebRTC 协议分支：通过 FFmpeg 官方原生 WHIP 推送 Opus 音频编码包
    if (config_.stream.protocol == "webrtc") {
        return webrtc_.push_audio(encoded, error);
    }
    
    error = "unsupported stream protocol: " + config_.stream.protocol;
    return false;
}

}  // namespace visioncast
