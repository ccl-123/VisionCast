/**
 * @file rtp_packetizer.cpp
 * @brief VisionCast RTP 打包器实现文件
 * 
 * 本文件实现了 RtpPacketizer 类。主要功能包括：
 * 1. 扫描与提取 H.264/H.265 视频帧（Annex-B 码流格式，带 00 00 01 或 00 00 00 01 起始码）中的各个 NAL 单元。
 * 2. 视频帧分片打包：对于小于 MTU 的 NALU 直接以单一包发送；对于大于 MTU 的 NALU 执行 FU-A/FU 分片打包。
 * 3. 构造包含 Version、Marker、Payload Type、Sequence Number、Timestamp 及 SSRC 的 RFC 3550 标准 12 字节 RTP 头部。
 */

#include "transport/rtp_packetizer.h"

#include <algorithm>
#include <cstddef>

namespace visioncast {
namespace {

constexpr std::size_t kRtpHeaderSize = 12; // RFC 3550 定义的标准 RTP 报文头部大小

// 辅助函数：检测 Annex-B 起始码的长度（3字节或4字节）
std::size_t start_code_size(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos + 3 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        return 3;
    }
    if (pos + 4 <= data.size() && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 0 && data[pos + 3] == 1) {
        return 4;
    }
    return 0;
}

std::pair<std::size_t, std::size_t> next_annexb_nalu(const std::vector<std::uint8_t>& data,
                                                     std::size_t& pos) {
    while (pos < data.size()) {
        const std::size_t sc = start_code_size(data, pos);
        if (sc == 0) {
            ++pos;
            continue;
        }
        const std::size_t start = pos + sc;
        pos = start;
        while (pos < data.size() && start_code_size(data, pos) == 0) {
            ++pos;
        }
        if (pos > start) {
            return {start, pos - start};
        }
    }
    return {data.size(), 0};
}

}  // namespace

RtpPacketizer::RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc, std::size_t mtu)
    : payload_type_(payload_type), ssrc_(ssrc), mtu_(std::max<std::size_t>(mtu, 256)) {}

std::vector<RtpPacket> RtpPacketizer::packetize(const EncodedPacket& packet) {
    std::vector<RtpPacket> packets;
    packetize_each(packet, [&packets](const RtpPacket& rtp) {
        packets.push_back(rtp);
        return true;
    });
    return packets;
}

bool RtpPacketizer::packetize_each(const EncodedPacket& packet,
                                   const PacketHandler& handler) {
    // 视频需要解析 NALU 并依据 MTU 分片打包；Opus 音频帧直接作为单个 RTP 载荷发送。
    if (packet.media_type == MediaType::Video) {
        if (packet.video_codec == VideoCodec::H265) {
            return packetize_h265_each(packet, handler);
        }
        return packetize_h264_each(packet, handler);
    }
    if (packet.data.empty()) {
        return true;
    }
    RtpPacket rtp;
    make_packet_into(rtp, packet.data.data(), packet.data.size(), packet.rtp_timestamp, true);
    return handler(rtp);
}

bool RtpPacketizer::packetize_h265_each(const EncodedPacket& packet,
                                        const PacketHandler& handler) {
    const std::size_t max_payload = mtu_ - kRtpHeaderSize;
    RtpPacket rtp;
    std::size_t scan_pos = 0;
    auto current = next_annexb_nalu(packet.data, scan_pos);

    if (current.second == 0 && !packet.data.empty()) {
        current = {0, packet.data.size()};
        scan_pos = packet.data.size();
    }

    while (current.second > 0) {
        const auto [offset, size] = current;
        const auto next = next_annexb_nalu(packet.data, scan_pos);
        const bool last_nalu = next.second == 0;
        if (size < 2) {
            current = next;
            continue;
        }
        const std::uint8_t* nalu = packet.data.data() + offset;

        if (size <= max_payload) {
            make_packet_into(rtp, nalu, size, packet.rtp_timestamp, last_nalu);
            if (!handler(rtp)) {
                return false;
            }
            current = next;
            continue;
        }

        const std::uint8_t nalu_type = static_cast<std::uint8_t>((nalu[0] >> 1) & 0x3FU);
        const std::uint8_t fu_indicator[2] = {
            static_cast<std::uint8_t>((nalu[0] & 0x81U) | (49U << 1U)),
            nalu[1],
        };
        const std::size_t fragment_payload = max_payload - 3U;
        std::size_t consumed = 2;
        bool start = true;

        while (consumed < size) {
            const std::size_t chunk = std::min(fragment_payload, size - consumed);
            std::uint8_t fu_header = nalu_type;
            if (start) {
                fu_header |= 0x80U;
            }
            if (consumed + chunk >= size) {
                fu_header |= 0x40U;
            }

            make_h265_fu_packet_into(rtp,
                                     fu_indicator,
                                     fu_header,
                                     nalu + consumed,
                                     chunk,
                                     packet.rtp_timestamp,
                                     last_nalu && consumed + chunk >= size);
            if (!handler(rtp)) {
                return false;
            }
            consumed += chunk;
            start = false;
        }
        current = next;
    }
    return true;
}

bool RtpPacketizer::packetize_h264_each(const EncodedPacket& packet,
                                        const PacketHandler& handler) {
    const std::size_t max_payload = mtu_ - kRtpHeaderSize;
    RtpPacket rtp;
    std::size_t scan_pos = 0;
    auto current = next_annexb_nalu(packet.data, scan_pos);

    if (current.second == 0 && !packet.data.empty()) {
        current = {0, packet.data.size()};
        scan_pos = packet.data.size();
    }

    while (current.second > 0) {
        const auto [offset, size] = current;
        const auto next = next_annexb_nalu(packet.data, scan_pos);
        const bool last_nalu = next.second == 0;
        if (size == 0) {
            current = next;
            continue;
        }
        const std::uint8_t* nalu = packet.data.data() + offset;

        if (size <= max_payload) {
            make_packet_into(rtp, nalu, size, packet.rtp_timestamp, last_nalu);
            if (!handler(rtp)) {
                return false;
            }
            current = next;
            continue;
        }

        const std::uint8_t nalu_header = nalu[0];
        const std::uint8_t fu_indicator = static_cast<std::uint8_t>((nalu_header & 0xE0U) | 28U);
        const std::uint8_t nalu_type = static_cast<std::uint8_t>(nalu_header & 0x1FU);
        const std::size_t fragment_payload = max_payload - 2U;
        std::size_t consumed = 1;
        bool start = true;

        while (consumed < size) {
            const std::size_t chunk = std::min(fragment_payload, size - consumed);
            std::uint8_t fu_header = nalu_type;
            if (start) {
                fu_header |= 0x80U;
            }
            if (consumed + chunk >= size) {
                fu_header |= 0x40U;
            }

            make_fua_packet_into(rtp,
                                 fu_indicator,
                                 fu_header,
                                 nalu + consumed,
                                 chunk,
                                 packet.rtp_timestamp,
                                 last_nalu && consumed + chunk >= size);
            if (!handler(rtp)) {
                return false;
            }
            consumed += chunk;
            start = false;
        }
        current = next;
    }
    return true;
}

void RtpPacketizer::make_packet_into(RtpPacket& packet,
                                     const std::uint8_t* payload,
                                     std::size_t payload_size,
                                     std::uint32_t timestamp,
                                     bool marker) {
    packet.bytes.resize(kRtpHeaderSize + payload_size);
    write_header(packet, timestamp, marker);

    // 将载荷数据拷贝到 RTP 头部之后
    std::copy(payload, payload + payload_size, packet.bytes.begin() + kRtpHeaderSize);
}

void RtpPacketizer::write_header(RtpPacket& packet, std::uint32_t timestamp, bool marker) {
    packet.marker = marker;

    // 1. 设置 RTP 固定头字段 (12 字节)
    // Byte 0: Version(2bits)=2, Padding(1bit)=0, Extension(1bit)=0, CSRC count(4bits)=0 -> 0x80
    packet.bytes[0] = 0x80U;
    
    // Byte 1: Marker(1bit) + Payload Type(7bits)
    packet.bytes[1] = static_cast<std::uint8_t>((marker ? 0x80U : 0x00U) | (payload_type_ & 0x7FU));
    
    // Bytes 2-3: Sequence Number (大端字节序)
    packet.bytes[2] = static_cast<std::uint8_t>(sequence_ >> 8);
    packet.bytes[3] = static_cast<std::uint8_t>(sequence_ & 0xFFU);
    ++sequence_;
    
    // Bytes 4-7: Timestamp (大端字节序)
    packet.bytes[4] = static_cast<std::uint8_t>(timestamp >> 24);
    packet.bytes[5] = static_cast<std::uint8_t>(timestamp >> 16);
    packet.bytes[6] = static_cast<std::uint8_t>(timestamp >> 8);
    packet.bytes[7] = static_cast<std::uint8_t>(timestamp);
    
    // Bytes 8-11: SSRC (大端字节序)
    packet.bytes[8] = static_cast<std::uint8_t>(ssrc_ >> 24);
    packet.bytes[9] = static_cast<std::uint8_t>(ssrc_ >> 16);
    packet.bytes[10] = static_cast<std::uint8_t>(ssrc_ >> 8);
    packet.bytes[11] = static_cast<std::uint8_t>(ssrc_);
}

void RtpPacketizer::make_fua_packet_into(RtpPacket& packet,
                                         std::uint8_t fu_indicator,
                                         std::uint8_t fu_header,
                                         const std::uint8_t* fragment,
                                         std::size_t fragment_size,
                                         std::uint32_t timestamp,
                                         bool marker) {
    packet.bytes.resize(kRtpHeaderSize + 2U + fragment_size);
    write_header(packet, timestamp, marker);
    packet.bytes[kRtpHeaderSize] = fu_indicator;
    packet.bytes[kRtpHeaderSize + 1U] = fu_header;
    std::copy(fragment,
              fragment + fragment_size,
              packet.bytes.begin() + kRtpHeaderSize + 2U);
}

void RtpPacketizer::make_h265_fu_packet_into(RtpPacket& packet,
                                             const std::uint8_t* fu_indicator,
                                             std::uint8_t fu_header,
                                             const std::uint8_t* fragment,
                                             std::size_t fragment_size,
                                             std::uint32_t timestamp,
                                             bool marker) {
    packet.bytes.resize(kRtpHeaderSize + 3U + fragment_size);
    write_header(packet, timestamp, marker);
    packet.bytes[kRtpHeaderSize] = fu_indicator[0];
    packet.bytes[kRtpHeaderSize + 1U] = fu_indicator[1];
    packet.bytes[kRtpHeaderSize + 2U] = fu_header;
    std::copy(fragment,
              fragment + fragment_size,
              packet.bytes.begin() + kRtpHeaderSize + 3U);
}

}  // namespace visioncast
