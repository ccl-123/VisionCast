#pragma once

#include <cstdint>

#include "media/audio_frame.h"
#include "media/encoded_packet.h"

namespace visioncast {

class AudioEncoder {
public:
    // Encodes S16_LE PCM to ITU-T G.711 A-law (RTP payload type 8 / PCMA).
    EncodedPacket encode_pcma(const AudioFrame& frame) const;

private:
    static std::uint8_t linear_to_alaw(std::int16_t sample);
};

}  // namespace visioncast
