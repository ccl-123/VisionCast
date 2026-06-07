/**
 * @file av_transport.h
 * @brief VisionCast 音视频传输控制模块头文件
 * 
 * 本文件定义了 AvTransport 类，该类是 VisionCast 系统中音视频数据传输的核心管理器。
 * 它集成了 UDP/RTP 传输、RTMP 推流以及 WebRTC 推流功能，并根据系统配置分发音视频流。
 * 
 * 主要职责包括：
 * 1. 统一管理各种底层发送器（UDP, RTMP, WebRTC）的初始化与销毁。
 * 2. 封装视频编码包（EncodedPacket）及音频帧（AudioFrame）的发送逻辑。
 * 3. 使用互斥锁保证多线程环境下发送逻辑的线程安全。
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "common/types.h"
#include "media/audio_encoder.h"
#include "media/audio_frame.h"
#include "media/encoded_packet.h"
#include "transport/rtmp_pusher.h"
#include "transport/rtp_packetizer.h"
#include "transport/udp_sender.h"
#include "transport/webrtc_pusher.h"

namespace visioncast {

/**
 * @class AvTransport
 * @brief 协调和控制音视频发送与分发的传输管理器
 */
class AvTransport {
public:
    /**
     * @brief 构造函数，使用指定的 VisionCast 配置进行初始化
     * @param config 系统全局与传输相关的配置结构体
     */
    explicit AvTransport(VisionCastConfig config);
    
    /**
     * @brief 析构函数，负责关闭传输通道并释放资源
     */
    ~AvTransport();

    // 禁用拷贝构造和赋值操作
    AvTransport(const AvTransport&) = delete;
    AvTransport& operator=(const AvTransport&) = delete;

    /**
     * @brief 打开并初始化所有配置的传输通道（如 UDP 发送器、RTMP/WebRTC 推流器等）
     * @param error 传入传出参数，用于返回出错时的详细错误信息
     * @return 成功返回 true，失败返回 false
     */
    bool open(std::string& error);

    /**
     * @brief 关闭所有已开启的传输通道并重置相关状态
     */
    void close();

    /**
     * @brief 发送编码后的视频数据包
     * @param packet 包含编码后视频帧数据（如 H.264/H.265 码流）及元信息的结构体
     * @param error 传入传出参数，用于返回出错时的详细错误信息
     * @return 发送成功返回 true，失败返回 false
     */
    bool send_video(const EncodedPacket& packet, std::string& error);

    /**
     * @brief 发送音频数据帧
     * @param frame 原始音频采样数据帧或已编码的音频帧
     * @param error 传入传出参数，用于返回出错时的详细错误信息
     * @return 发送成功返回 true，失败返回 false
     */
    bool send_audio(const AudioFrame& frame, std::string& error);

private:
    VisionCastConfig config_;       ///< 传输配置参数（例如目标 IP、端口、推流 URL 等）
    UdpSender video_udp_;           ///< 视频 UDP 发送器，用于传输 RTP 封包后的视频数据
    UdpSender audio_udp_;           ///< 音频 UDP 发送器，用于传输 RTP 封包后的音频数据
    RtpPacketizer video_rtp_;       ///< 视频 RTP 打包器，将 H.264/H.265 编码包切片并封装为 RTP 报文
    RtpPacketizer audio_rtp_;       ///< 音频 RTP 打包器，将音频数据打包为 RTP 报文
    AudioEncoder audio_encoder_;     ///< 音频编码器（若需要对原始音频进行 AAC/OPUS 等格式的编码）
    RtmpPusher rtmp_;               ///< RTMP 推流器组件，负责向 RTMP 服务器推送音视频流
    WebRtcPusher webrtc_;           ///< WebRTC 推流器组件，负责低延迟音视频互动流传输
    std::mutex mutex_;              ///< 互斥锁，保护传输状态的线程安全，避免并发冲突
    bool opened_ = false;           ///< 标记传输通道是否已处于打开状态
};

}  // namespace visioncast

