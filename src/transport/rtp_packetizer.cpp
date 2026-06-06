#include "transport/rtp_packetizer.h"

#include <algorithm>
#include <cstddef>

namespace visioncast {
namespace {

constexpr std::size_t kRtpHeaderSize = 12;

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
    if (nalus.empty() && !data.empty()) {
        nalus.emplace_back(0, data.size());
    }
    return nalus;
}

}  // namespace

RtpPacketizer::RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc, std::size_t mtu)
    : payload_type_(payload_type), ssrc_(ssrc), mtu_(std::max<std::size_t>(mtu, 256)) {}

std::vector<RtpPacket> RtpPacketizer::packetize(const EncodedPacket& packet) {
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
        if (size <= max_payload) {
            out.push_back(make_packet(nalu, size, packet.rtp_timestamp, last_nalu));
            continue;
        }

        // FU-A fragmentation: first byte carries F/NRI/type, fragments carry S/E flags.
        const std::uint8_t nalu_header = nalu[0];
        const std::uint8_t fu_indicator = static_cast<std::uint8_t>((nalu_header & 0xE0U) | 28U);
        const std::uint8_t nalu_type = static_cast<std::uint8_t>(nalu_header & 0x1FU);
        const std::size_t fragment_payload = max_payload - 2U;
        std::size_t consumed = 1;
        bool start = true;
        while (consumed < size) {
            const std::size_t chunk = std::min(fragment_payload, size - consumed);
            std::vector<std::uint8_t> payload;
            payload.reserve(chunk + 2U);
            payload.push_back(fu_indicator);
            std::uint8_t fu_header = nalu_type;
            if (start) {
                fu_header |= 0x80U;
            }
            if (consumed + chunk >= size) {
                fu_header |= 0x40U;
            }
            payload.push_back(fu_header);
            payload.insert(payload.end(), nalu + consumed, nalu + consumed + chunk);
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
    packet.bytes[0] = 0x80U;
    packet.bytes[1] = static_cast<std::uint8_t>((marker ? 0x80U : 0x00U) | (payload_type_ & 0x7FU));
    packet.bytes[2] = static_cast<std::uint8_t>(sequence_ >> 8);
    packet.bytes[3] = static_cast<std::uint8_t>(sequence_ & 0xFFU);
    ++sequence_;
    packet.bytes[4] = static_cast<std::uint8_t>(timestamp >> 24);
    packet.bytes[5] = static_cast<std::uint8_t>(timestamp >> 16);
    packet.bytes[6] = static_cast<std::uint8_t>(timestamp >> 8);
    packet.bytes[7] = static_cast<std::uint8_t>(timestamp);
    packet.bytes[8] = static_cast<std::uint8_t>(ssrc_ >> 24);
    packet.bytes[9] = static_cast<std::uint8_t>(ssrc_ >> 16);
    packet.bytes[10] = static_cast<std::uint8_t>(ssrc_ >> 8);
    packet.bytes[11] = static_cast<std::uint8_t>(ssrc_);
    std::copy(payload, payload + payload_size, packet.bytes.begin() + kRtpHeaderSize);
    return packet;
}

}  // namespace visioncast
