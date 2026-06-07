/**
 * @file mpp_encoder.h
 * @brief VisionCast MPP (Media Process Platform) 视频编码器头文件
 * @details 负责将输入的 YUV 原始视频帧（例如 NV12 格式）使用瑞芯微（Rockchip）的
 *          MPP 硬件加速接口编码成 H.264/H.265 等压缩格式的数据包（EncodedPacket）。
 */

#pragma once

#include <memory>
#include <string>

#include "common/types.h"
#include "media/encoded_packet.h"
#include "media/video_frame.h"

namespace visioncast {

/**
 * @class MppEncoder
 * @brief 基于瑞芯微 MPP 硬件加速的视频编码器类
 */
class MppEncoder {
public:
    // 构造函数，传入视频采集配置和编码配置参数
    MppEncoder(VideoConfig video, EncoderConfig encoder);
    // 析构函数，负责安全关闭编码器并释放相关硬件资源
    ~MppEncoder();

    // 禁用拷贝构造和赋值
    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    // 打开并初始化 MPP 编码器，配置编码参数（如码率、帧率、GOP、编码格式等）
    bool open(std::string& error);
    // 关闭编码器并重置相关资源
    void close();
    // 编码一帧视频数据，将输入的 VideoFrame 转换为输出的 EncodedPacket
    bool encode(const VideoFrame& frame, EncodedPacket& packet, std::string& error);
    // 静态函数，获取底层 MPP 后台版本信息
    static std::string backend_version();

private:
    struct Impl;                  // 前置声明具体实现类，用于 Pimpl 模式
    std::unique_ptr<Impl> impl_;  // 指向具体实现类的智能指针
    VideoConfig video_;           // 视频输入配置参数
    EncoderConfig encoder_;       // 编码输出配置参数
};

}  // namespace visioncast
