#pragma once

#include <cstdint>
#include <vector>

#include "transport/packetizer.h"

namespace visioncast {

class RtpPacketizer : public Packetizer {
public:
    RtpPacketizer(std::uint8_t payload_type, std::uint32_t ssrc, std::size_t mtu = 1200);

    std::vector<RtpPacket> packetize(const EncodedPacket& packet) override;

private:
    std::vector<RtpPacket> packetize_h264(const EncodedPacket& packet);
    RtpPacket make_packet(const std::uint8_t* payload,
                          std::size_t payload_size,
                          std::uint32_t timestamp,
                          bool marker);

    std::uint8_t payload_type_ = 96;
    std::uint32_t ssrc_ = 0;
    std::uint16_t sequence_ = 0;
    std::size_t mtu_ = 1200;
};

}  // namespace visioncast
