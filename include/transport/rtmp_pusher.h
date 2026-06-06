#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/types.h"

namespace visioncast {

/**
 * @brief RTMP 推流客户端类
 *
 * @note 该模块主要负责 RTMP 连接、音视频 Tag 封包与主动向服务端推送。
 *       开发时，必须参考 SDK 中的官方推流例程进行实现：
 *       例程路径：rk_media_sdk/rkadk/examples/rkadk_rtmp_test.c
 */
class RtmpPusher {
public:
    struct Impl;

    RtmpPusher(std::string rtmp_url, VideoConfig video, AudioConfig audio);
    ~RtmpPusher();

    // 建立连接
    bool connect(std::string& error);
    // 断开连接
    void disconnect();

    // 发送视频帧
    bool push_video(const uint8_t* data, size_t size, uint64_t pts_us, std::string& error);
    // 发送音频帧
    bool push_audio(const uint8_t* data, size_t size, uint64_t pts_us, std::string& error);

private:
    std::unique_ptr<Impl> impl_;
    std::string rtmp_url_;
    VideoConfig video_;
    AudioConfig audio_;
};

} // namespace visioncast
