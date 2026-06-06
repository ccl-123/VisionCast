#pragma once

#include <cstdint>
#include <vector>

#include "media/encoded_packet.h"

namespace visioncast {

struct RtpPacket {
    std::vector<std::uint8_t> bytes;
    bool marker = false;
};

class Packetizer {
public:
    virtual ~Packetizer() = default;
    virtual std::vector<RtpPacket> packetize(const EncodedPacket& packet) = 0;
};

}  // namespace visioncast
