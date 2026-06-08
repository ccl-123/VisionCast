/**
 * @file webrtc_pusher.h
 * @brief VisionCast WebRTC (WHIP) 推流客户端模块头文件
 * 
 * 本文件定义了 WebRtcPusher 类，该类负责建立与 WebRTC 服务的连接，
 * 并通过 WHIP (WebRTC-HTTP Ingestion Protocol) 标准协议向远端 SFU/流媒体网关
 * 推送 RTP 封装后的音视频数据。主要用于实现超低延迟的音视频互动传输。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "transport/packetizer.h"

namespace visioncast {

/**
 * @class WebRtcPusher
 * @brief WebRTC 推流客户端类，提供音视频 RTP 报文通过 WebRTC/WHIP 进行推流的功能
 */
class WebRtcPusher {
public:
    WebRtcPusher(std::string whip_url, VideoConfig video, AudioConfig audio);
    ~WebRtcPusher();

    // 禁用拷贝构造和赋值操作
    WebRtcPusher(const WebRtcPusher&) = delete;
    WebRtcPusher& operator=(const WebRtcPusher&) = delete;

    /**
     * @brief 建立与 WebRTC 服务器/网关的 WHIP 连接
     * 
     * 通常流程包含：
     * 1. 发起 HTTP POST 请求发送本地 SDP Offer 至 WHIP 服务器。
     * 2. 接收并解析 HTTP 响应返回的 SDP Answer，进行本地 PeerConnection 配置。
     * 3. 开启 ICE 收集与握手，建立 DTLS 加密传输通道。
     * @param error 传入传出参数，用于返回建连失败的错误信息
     * @return 成功建立连接返回 true，失败返回 false
     */
    bool connect(std::string& error);

    /**
     * @brief 断开与 WebRTC 服务端的连接并释放 PeerConnection 资源
     */
    void disconnect();

    /**
     * @brief 通过 WebRTC 视频通道（SRTP）推送 RTP 视频报文序列
     * @param packets RTP 视频报文列表（FU-A 分片或单 NALU 报文）
     * @param error 传入传出参数，用于返回发送失败时的错误信息
     * @return 成功发送返回 true，失败返回 false
     */
    bool push_video_rtp(const std::vector<RtpPacket>& packets, std::string& error);
    bool push_video_rtp_packet(const RtpPacket& packet, std::string& error);

    /**
     * @brief 通过 WebRTC 音频通道（SRTP）推送 RTP 音频报文序列
     * @param packets RTP 音频报文列表
     * @param error 传入传出参数，用于返回发送失败时的错误信息
     * @return 成功发送返回 true，失败返回 false
     */
    bool push_audio_rtp(const std::vector<RtpPacket>& packets, std::string& error);
    bool push_audio_rtp_packet(const RtpPacket& packet, std::string& error);

private:
    struct Impl;                     ///< Pimpl 具体实现类，封装具体的 WebRTC SDK（如 WebRTC 官方 SDK 或 libdatachannel 等）逻辑
    std::unique_ptr<Impl> impl_;      ///< 指向实现类的智能指针
    std::string whip_url_;            ///< WebRTC WHIP 的服务端 URL
    VideoConfig video_;               ///< 视频格式与编码属性配置
    AudioConfig audio_;               ///< 音频格式与编码属性配置
};

}  // namespace visioncast
