#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "common/types.h"
#include "media/audio_encoder.h"
#include "media/audio_frame.h"
#include "media/encoded_packet.h"
#include "transport/rtmp_pusher.h"
#include "transport/rtp_packetizer.h"
#include "transport/rtsp_server.h"
#include "transport/udp_sender.h"
#include "transport/webrtc_pusher.h"

namespace visioncast {

class AvTransport {
public:
    explicit AvTransport(VisionCastConfig config);
    ~AvTransport();

    AvTransport(const AvTransport&) = delete;
    AvTransport& operator=(const AvTransport&) = delete;

    bool open(std::string& error);
    void close();
    bool send_video(const EncodedPacket& packet, std::string& error);
    bool send_audio(const AudioFrame& frame, std::string& error);

private:
    VisionCastConfig config_;
    UdpSender video_udp_;
    UdpSender audio_udp_;
    RtpPacketizer video_rtp_;
    RtpPacketizer audio_rtp_;
    AudioEncoder audio_encoder_;
    RtmpPusher rtmp_;
    RtspServer rtsp_;
    WebRtcPusher webrtc_;
    std::mutex mutex_;
    bool opened_ = false;
};

}  // namespace visioncast
