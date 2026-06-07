/**
 * @file packetizer.h
 * @brief VisionCast 数据流打包器接口头文件
 * 
 * 本文件定义了 RTP 报文结构体（RtpPacket）和打包器抽象基类（Packetizer）。
 * 不同的流媒体格式（如 H.264/H.265 视频或 AAC/PCM 音频）将通过继承该基类，
 * 实现将特定格式的编码包分割、打包并转换为符合 RTP 协议要求的报文序列。
 */

#pragma once

#include <cstdint>
#include <vector>

#include "media/encoded_packet.h"

namespace visioncast {

/**
 * @struct RtpPacket
 * @brief 表示一个完整的 RTP 报文字节数据及其控制标记
 */
struct RtpPacket {
    std::vector<std::uint8_t> bytes;    ///< 包含 RTP 头部与载荷（Payload）的完整二进制数据
    bool marker = false;                ///< RTP 头部中的 Marker (M) 标记，通常对于视频帧最后一个切片标记为 true
};

/**
 * @class Packetizer
 * @brief 打包器接口类，定义将编码包打包为 RTP 报文序列的接口
 */
class Packetizer {
public:
    /**
     * @brief 虚析构函数，保证派生类的安全析构
     */
    virtual ~Packetizer() = default;

    /**
     * @brief 将编码数据包切片并封装为 RTP 报文集合
     * @param packet 输入的编码后视频/音频数据包
     * @return 返回封装好的 RTP 报文列表
     */
    virtual std::vector<RtpPacket> packetize(const EncodedPacket& packet) = 0;
};

}  // namespace visioncast

