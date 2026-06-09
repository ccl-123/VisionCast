/**
 * @file rtp_packetizer.h
 * @brief VisionCast RTP 协议打包器类头文件
 * 
 * 本文件实现了解析 H.264/H.265 视频帧并将其进行 RTP（RFC 3550 / RFC 6184 / RFC 7798）封包的打包器。
 * 支持在超出 MTU 限制时自动执行分片（FU-A 封包），并处理 RTP 头部（含序列号、时间戳和 SSRC 等）。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "transport/packetizer.h"

namespace visioncast {

/**
 * @class RtpPacketizer
 * @brief 继承自 Packetizer 的 RTP 打包实现类，用于实现音视频数据的 RTP 协议封装
 */
class RtpPacketizer : public Packetizer {
public:
    using PacketHandler = std::function<bool(const RtpPacket&)>;

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

    bool packetize_each(const EncodedPacket& packet, const PacketHandler& handler);

private:
    bool packetize_h264_each(const EncodedPacket& packet, const PacketHandler& handler);
    bool packetize_h265_each(const EncodedPacket& packet, const PacketHandler& handler);

    void make_packet_into(RtpPacket& packet,
                          const std::uint8_t* payload,
                          std::size_t payload_size,
                          std::uint32_t timestamp,
                          bool marker);

    void write_header(RtpPacket& packet, std::uint32_t timestamp, bool marker);

    void make_fua_packet_into(RtpPacket& packet,
                              std::uint8_t fu_indicator,
                              std::uint8_t fu_header,
                              const std::uint8_t* fragment,
                              std::size_t fragment_size,
                              std::uint32_t timestamp,
                              bool marker);

    void make_h265_fu_packet_into(RtpPacket& packet,
                                  const std::uint8_t* fu_indicator,
                                  std::uint8_t fu_header,
                                  const std::uint8_t* fragment,
                                  std::size_t fragment_size,
                                  std::uint32_t timestamp,
                                  bool marker);

    std::uint8_t payload_type_ = 96;  ///< RTP 载荷类型
    std::uint32_t ssrc_ = 0;           ///< 同步源标识符 (SSRC)
    std::uint16_t sequence_ = 0;       ///< 报文序列号 (Sequence Number)，每次发送递增
    std::size_t mtu_ = 1200;           ///< 最大传输单元大小限制
};

}  // namespace visioncast
