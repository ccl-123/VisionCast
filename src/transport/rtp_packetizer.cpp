/**
 * @file rtp_packetizer.cpp
 * @brief VisionCast RTP 打包器实现文件
 * 
 * 本文件实现了 RtpPacketizer 类。主要功能包括：
 * 1. 扫描与提取 H.264 视频帧（Annex-B 码流格式，带 00 00 01 或 00 00 00 01 起始码）中的各个 NAL 单元。
 * 2. 视频帧分片打包：对于小于 MTU 的 NALU 直接以单一包发送；对于大于 MTU 的 NALU 执行 FU-A (RFC 6184) 分片打包。
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

// 辅助函数：从输入的字节流中查找所有的 NALU 数据偏移与长度（去除了起始码）
std::vector<std::pair<std::size_t, std::size_t>> find_annexb_nalus(
    const std::vector<std::uint8_t>& data) {
    std::vector<std::pair<std::size_t, std::size_t>> nalus;
    std::size_t pos = 0;
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
            nalus.emplace_back(start, pos - start);
        }
    }
    // 如果没有起始码且数据不为空，将其作为一个单一的载荷返回
    if (nalus.empty() && !data.empty()) {
        nalus.emplace_back(0, data.size());
    }
    return nalus;
}

}  // namespace

RtpPacketizer::RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc, std::size_t mtu)
    : payload_type_(payload_type), ssrc_(ssrc), mtu_(std::max<std::size_t>(mtu, 256)) {}

std::vector<RtpPacket> RtpPacketizer::packetize(const EncodedPacket& packet) {
    // 视频需要解析 NALU 并依据 MTU 分片打包；Opus 音频帧直接作为单个 RTP 载荷发送。
    if (packet.media_type == MediaType::Video) {
        return packetize_h264(packet);
    }
    if (packet.data.empty()) {
        return {};
    }
    return {make_packet(packet.data.data(), packet.data.size(), packet.rtp_timestamp, true)};
}

std::vector<RtpPacket> RtpPacketizer::packetize_h264(const EncodedPacket& packet) {
    std::vector<RtpPacket> out;
    const auto nalus = find_annexb_nalus(packet.data);
    const std::size_t max_payload = mtu_ - kRtpHeaderSize;
    
    for (std::size_t nalu_index = 0; nalu_index < nalus.size(); ++nalu_index) {
        const auto [offset, size] = nalus[nalu_index];
        if (size == 0) {
            continue;
        }
        const std::uint8_t* nalu = packet.data.data() + offset;
        const bool last_nalu = nalu_index + 1 == nalus.size();
        
        // 情况 1：NALU 长度加上 RTP 头部没有超出 MTU 限制，封装为单一 RTP 包
        if (size <= max_payload) {
            out.push_back(make_packet(nalu, size, packet.rtp_timestamp, last_nalu));
            continue;
        }

        // 情况 2：超出 MTU，按照 RFC 6184 规范进行 FU-A 分片打包
        // FU indicator 格式： F (1bit) + NRI (2bit) + Type (5bit, 值为 28 表示 FU-A)
        const std::uint8_t nalu_header = nalu[0];
        const std::uint8_t fu_indicator = static_cast<std::uint8_t>((nalu_header & 0xE0U) | 28U);
        const std::uint8_t nalu_type = static_cast<std::uint8_t>(nalu_header & 0x1FU);
        
        // 每个 FU-A 分片都需要 1 字节 FU indicator 和 1 字节 FU header
        const std::size_t fragment_payload = max_payload - 2U;
        std::size_t consumed = 1; // 跳过原 NALU 的 1 字节头部
        bool start = true;
        
        while (consumed < size) {
            const std::size_t chunk = std::min(fragment_payload, size - consumed);
            std::vector<std::uint8_t> payload;
            payload.reserve(chunk + 2U);
            payload.push_back(fu_indicator);
            
            // FU header 格式： S (1bit, 开始标志) + E (1bit, 结束标志) + R (1bit, 保留) + Type (5bit, 原 NALU 类型)
            std::uint8_t fu_header = nalu_type;
            if (start) {
                fu_header |= 0x80U; // 设置 Start 位
            }
            if (consumed + chunk >= size) {
                fu_header |= 0x40U; // 设置 End 位
            }
            payload.push_back(fu_header);
            
            // 拷入 NALU 部分载荷数据
            payload.insert(payload.end(), nalu + consumed, nalu + consumed + chunk);
            
            // 组装 RTP 报文。当且仅当是最后一包且是原包的最后一个分片时，Marker 标志置为 true
            out.push_back(make_packet(payload.data(),
                                      payload.size(),
                                      packet.rtp_timestamp,
                                      last_nalu && consumed + chunk >= size));
            consumed += chunk;
            start = false;
        }
    }
    return out;
}

RtpPacket RtpPacketizer::make_packet(const std::uint8_t* payload,
                                     std::size_t payload_size,
                                     std::uint32_t timestamp,
                                     bool marker) {
    RtpPacket packet;
    packet.marker = marker;
    packet.bytes.resize(kRtpHeaderSize + payload_size);
    
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
    
    // 2. 将载荷数据拷贝到 RTP 头部之后
    std::copy(payload, payload + payload_size, packet.bytes.begin() + kRtpHeaderSize);
    return packet;
}

}  // namespace visioncast
