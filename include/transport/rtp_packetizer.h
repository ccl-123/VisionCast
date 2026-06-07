/**
 * @file rtp_packetizer.h
 * @brief VisionCast RTP 协议打包器类头文件
 * 
 * 本文件实现了解析 H.264 等格式视频帧并将其进行 RTP（RFC 3550 / RFC 6184）封包的打包器。
 * 支持在超出 MTU 限制时自动执行分片（FU-A 封包），并处理 RTP 头部（含序列号、时间戳和 SSRC 等）。
 */

#pragma once

#include <cstdint>
#include <vector>

#include "transport/packetizer.h"

namespace visioncast {

/**
 * @class RtpPacketizer
 * @brief 继承自 Packetizer 的 RTP 打包实现类，用于实现音视频数据的 RTP 协议封装
 */
class RtpPacketizer : public Packetizer {
public:
    /**
     * @brief 构造函数，初始化 RTP 会话基本参数
     * @param payload_type 载荷类型（Payload Type，如 96 表示 H.264，97 表示 AAC 等）
     * @param ssrc 同步信源标识符（Synchronization Source）
     * @param mtu 最大传输单元（Maximum Transmission Unit），默认为 1200 字节以适配以太网/Wi-Fi
     */
    RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc, std::size_t mtu = 1200);

    /**
     * @brief 将编码数据包打包为一或多个 RTP 报文
     * @param packet 输入的编码后视频/音频数据包
     * @return 返回封装好的 RTP 报文列表
     */
    std::vector<RtpPacket> packetize(const EncodedPacket& packet) override;

private:
    /**
     * @brief 专门针对 H.264 NALU 进行打包
     * 
     * 主要策略：
     * 1. 若单 NALU 大小小于等于 MTU - 12 (RTP Header)，采用单一 NALU 单元打包。
     * 2. 若超出 MTU，则采用 FU-A (Fragmentation Unit A) 分片封包。
     * @param packet 包含 H.264 原始 NALU 的编码包
     * @return 返回生成的 RTP 报文序列
     */
    std::vector<RtpPacket> packetize_h264(const EncodedPacket& packet);

    /**
     * @brief 组装单个完整的 RTP 报文（包含 12 字节标准 RTP 头部和载荷）
     * @param payload 指向要写入载荷的数据指针
     * @param payload_size 载荷的字节长度
     * @param timestamp 当前报文的 RTP 时间戳
     * @param marker 是否为帧的最后一个包（设置 Marker 位）
     * @return 组装好的 RtpPacket 对象
     */
    RtpPacket make_packet(const std::uint8_t* payload,
                          std::size_t payload_size,
                          std::uint32_t timestamp,
                          bool marker);

    std::uint8_t payload_type_ = 96;  ///< RTP 载荷类型
    std::uint32_t ssrc_ = 0;           ///< 同步源标识符 (SSRC)
    std::uint16_t sequence_ = 0;       ///< 报文序列号 (Sequence Number)，每次发送递增
    std::size_t mtu_ = 1200;           ///< 最大传输单元大小限制
};

}  // namespace visioncast

