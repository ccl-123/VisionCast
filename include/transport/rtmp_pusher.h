/**
 * @file rtmp_pusher.h
 * @brief VisionCast RTMP 推流客户端模块头文件
 * 
 * 本文件定义了 RtmpPusher 类，负责将 H.264/H.265 编码后的视频帧以及音频帧（如 AAC）
 * 按照 RTMP 协议规范进行打包，并主动发送到远端 RTMP 直播流媒体服务器上。
 * 
 * 开发注意事项：
 * 该模块必须参考 Rockchip Media SDK 中的官方推流例程以获取最佳硬件级协议兼容性：
 * 例程路径：rk_media_sdk/rkadk/examples/rkadk_rtmp_test.c
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/types.h"

namespace visioncast {

/**
 * @class RtmpPusher
 * @brief RTMP 推流客户端类，提供音视频数据打包并推送至 RTMP 服务器的功能
 */
class RtmpPusher {
public:
    struct Impl; ///< 前置声明具体实现类（Pimpl 模式），隔离底层推流库（例如 librtmp）的依赖

    /**
     * @brief 构造函数，初始化 RTMP 目标 URL 以及音视频格式配置
     * @param rtmp_url 目标 RTMP 服务器推流地址（如 rtmp://server/live/stream）
     * @param video 视频编码及格式配置信息
     * @param audio 音频编码及格式配置信息
     */
    RtmpPusher(std::string rtmp_url, VideoConfig video, AudioConfig audio);

    /**
     * @brief 析构函数，负责断开连接并释放内部推流资源
     */
    ~RtmpPusher();

    /**
     * @brief 建立与 RTMP 服务器的握手并启动推流会话
     * @param error 传入传出参数，用于返回连接出错时的详细原因
     * @return 连接成功返回 true，失败返回 false
     */
    bool connect(std::string& error);

    /**
     * @brief 断开与 RTMP 服务器的连接并清空流通道状态
     */
    void disconnect();

    /**
     * @brief 向 RTMP 服务器发送一帧编码后的视频数据（例如 H.264/H.265 的 NALU）
     * @param data 待发送的视频码流缓冲区指针
     * @param size 视频帧的字节数大小
     * @param pts_us 视频帧的呈现时间戳（单位：微秒），用于 RTMP 时间戳同步
     * @param error 传入传出参数，用于返回发送失败时的错误消息
     * @return 推送成功返回 true，失败返回 false
     */
    bool push_video(const uint8_t* data, size_t size, uint64_t pts_us, std::string& error);

    /**
     * @brief 向 RTMP 服务器发送一帧编码后的音频数据（例如 AAC 音频包）
     * @param data 待发送的音频码流缓冲区指针
     * @param size 音频帧的字节数大小
     * @param pts_us 音频帧的呈现时间戳（单位：微秒），用于音视频同步（AV Sync）
     * @param error 传入传出参数，用于返回发送失败时的错误消息
     * @return 推送成功返回 true，失败返回 false
     */
    bool push_audio(const uint8_t* data, size_t size, uint64_t pts_us, std::string& error);

private:
    std::unique_ptr<Impl> impl_;      ///< Pimpl 实现指针，指向底层的具体 RTMP 推流封装（例如 librtmp 交互细节）
    std::string rtmp_url_;            ///< RTMP 服务器的目标推流 URL
    VideoConfig video_;               ///< 视频配置（分辨率、码率、帧率、编码格式等）
    AudioConfig audio_;               ///< 音频配置（采样率、通道数、格式、编码格式等）
};

} // namespace visioncast

