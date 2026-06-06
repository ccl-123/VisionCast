#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visioncast {

struct VideoConfig {
    std::string source = "mipi_13855";
    std::string fallback_source = "usb_c270";
    std::string device = "/dev/video11";
    std::string fallback_device = "/dev/video21";
    std::string fallback_format = "MJPEG";
    int width = 1280;
    int height = 720;
    int fps = 30;
    std::string format = "NV12";
};

struct AudioConfig {
    std::string device = "hw:1,0";
    int sample_rate = 48000;
    int channels = 1;
    std::string format = "S16_LE";
    int frame_ms = 20;
};

struct EncoderConfig {
    std::string video_codec = "h264";
    int bitrate = 4000000;
    int gop = 30;
    bool low_latency = true;
    int b_frames = 0;
};

struct StreamConfig {
    std::string protocol = "udp";
    std::string rtmp_url = "rtmp://192.168.137.1:1935/live/stream";
    std::string webrtc_url = "http://192.168.137.1:8889/live/stream";
    std::string server_ip = "192.168.137.1";
    int video_port = 5004;
    int audio_port = 5006;
    int rtsp_port = 8554;
    std::string rtsp_path = "/live";
    std::string sdp_path = "test.sdp";
};

struct DebugConfig {
    bool enable_perf_log = true;
    bool enable_dump_frame = false;
};

struct VisionCastConfig {
    VideoConfig video;
    AudioConfig audio;
    EncoderConfig encoder;
    StreamConfig stream;
    DebugConfig debug;
};

struct VideoFormatInfo {
    std::string fourcc;
    std::string description;
    std::string buffer_type;
};

struct VideoProbeResult {
    bool available = false;
    std::string device;
    std::string driver;
    std::string card;
    std::string bus_info;
    std::uint32_t capabilities = 0;
    std::uint32_t device_caps = 0;
    std::vector<VideoFormatInfo> formats;
    std::string error;
};

struct AudioProbeResult {
    bool available = false;
    std::string device;
    int card = -1;
    int pcm_device = -1;
    std::string card_name;
    std::string pcm_entry;
    std::string error;
};

}  // namespace visioncast
