/**
 * @file rtsp_pusher.h
 * @brief VisionCast RTSP 推流客户端模块头文件
 *
 * 本文件定义 RtspPusher 类，负责通过 FFmpeg RTSP muxer 向 MediaMTX 等标准
 * RTSP 服务端发布 H.264/H.265 视频与 Opus 音频。
 */

#pragma once

#include <memory>
#include <string>

#include "common/types.h"
#include "media/encoded_packet.h"

namespace visioncast {

/**
 * @class RtspPusher
 * @brief RTSP 推流客户端类，封装 RTSP 会话建立和音视频包写出。
 */
class RtspPusher {
public:
    struct Impl; ///< Pimpl 实现类，隔离 FFmpeg 细节

    RtspPusher(std::string rtsp_url,
               std::string rtsp_transport,
               VideoConfig video,
               AudioConfig audio,
               EncoderConfig encoder);
    ~RtspPusher();

    RtspPusher(const RtspPusher&) = delete;
    RtspPusher& operator=(const RtspPusher&) = delete;

    /**
     * @brief 分配 RTSP 输出上下文并准备推流会话。
     */
    bool connect(std::string& error);

    /**
     * @brief 断开 RTSP 推流并释放内部资源。
     */
    void disconnect();

    /**
     * @brief 写出一帧 H.264/H.265 Annex-B 视频包。
     */
    bool push_video(const EncodedPacket& packet, std::string& error);

    /**
     * @brief 写出一帧 Opus 音频包。
     */
    bool push_audio(const EncodedPacket& packet, std::string& error);

private:
    std::unique_ptr<Impl> impl_;
    std::string rtsp_url_;
    std::string rtsp_transport_;
    VideoConfig video_;
    AudioConfig audio_;
    EncoderConfig encoder_;
};

}  // namespace visioncast
