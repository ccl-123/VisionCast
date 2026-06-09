/**
 * @file types.h
 * @brief VisionCast Core Types and Config Structs
 * 
 * 本文件定义了 VisionCast 项目的核心配置与状态结构体。
 * 包含视频配置（VideoConfig，支持 MIPI 和 USB 摄像头）、音频配置（AudioConfig）、
 * 编码参数（EncoderConfig）、传输流配置（StreamConfig）及调试配置（DebugConfig）。
 * 此外，本文件还定义了用于多媒体硬件探测的返回结果结构体，以便自动匹配可用的软硬件设备。
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace visioncast {

/**
 * @brief 视频采集与硬件参数配置结构体。
 */
struct VideoConfig {
    std::string source = "mipi_13855";          ///< 首选视频源标识，如 "mipi_13855" (MIPI 摄像头) 或 "usb_c270" (USB)
    std::string fallback_source = "usb_c270";   ///< 备用视频源标识，当首选源不可用时切换
    std::string device = "/dev/video11";        ///< 首选 V4L2 视频设备文件路径
    std::string fallback_device = "/dev/video21"; ///< 备用 V4L2 视频设备文件路径
    std::string fallback_format = "MJPEG";      ///< 备选源的视频格式（例如 "MJPEG" 或 "YUYV"）
    int width = 1280;                           ///< 目标视频帧宽度（像素）
    int height = 720;                           ///< 目标视频帧高度（像素）
    int fps = 30;                               ///< 目标采集及编码帧率（Frames Per Second）
    std::string format = "NV12";                ///< 编码前图像格式，例如 MPP 常用 "NV12"，或 "YUYV" 等
    std::string sensor_subdev;                  ///< 图像传感器的 V4L2 子设备文件路径（用于直控曝光与空白期参数）
    int crop_left = -1;                         ///< V4L2 crop 左上角 X，-1表示不手动配置
    int crop_top = -1;                          ///< V4L2 crop 左上角 Y，-1表示不手动配置
    int crop_width = 0;                         ///< V4L2 crop 宽度，0表示不手动配置
    int crop_height = 0;                        ///< V4L2 crop 高度，0表示不手动配置
    int sensor_exposure = 0;                    ///< 图像传感器手动曝光值（0表示自动曝光）
    int sensor_vblank = -1;                     ///< 图像传感器垂直空白期设置（vblank），用于调整帧率或曝光上限，-1表示不手动配置
    int sensor_analogue_gain = -1;              ///< 图像传感器模拟增益，-1表示不手动配置
};

/**
 * @brief 音频采集参数配置结构体（对接 ALSA 音频库）。
 */
struct AudioConfig {
    std::string device = "hw:1,0";              ///< ALSA 捕获设备名称，默认为 "hw:1,0"（声卡1，设备0）
    int sample_rate = 48000;                    ///< 音频采样率；RTP/WebRTC/RTSP Opus 支持 8000/12000/16000/24000/48000 Hz
    int channels = 1;                           ///< 输出/推流声道数，1 表示单声道，2 表示双声道/立体声
    int capture_channels = 0;                   ///< ALSA 采集声道数；0 表示跟随 channels，可配置为 1 或 2
    std::string format = "S16_LE";              ///< ALSA 音频采样格式，默认为 Signed 16-bit Little Endian（有符号16位小端）
    int frame_ms = 20;                          ///< 单个音频帧的时间长度（毫秒级，常用 20ms，决定单次读取的采样数）
};

/**
 * @brief 编码器参数配置结构体（支持瑞芯微 MPP 硬件加速编码与 FFmpeg 软件编码）。
 */
struct EncoderConfig {
    std::string video_codec = "h264";           ///< 视频编码格式，支持 "h264" 或 "h265"
    int bitrate = 4000000;                      ///< 目标编码码率（bps，比特每秒）
    int gop = 30;                               ///< 关键帧间隔（Group Of Pictures），即多少帧包含一个 I 帧
    bool low_latency = true;                    ///< 是否启用低延迟模式（如关闭 MPP B帧，开启立即刷新）
    int b_frames = 0;                           ///< 编码的 B 帧个数，实时流传输中通常为 0
};

/**
 * @brief 流媒体传输与协议配置结构体。
 */
struct StreamConfig {
    std::string protocol = "rtp";               ///< 传输流协议，支持 "rtp" (UDP 单播/组播), "rtmp", "webrtc" (WHIP), "rtsp"
    std::string rtmp_url = "rtmp://{server_ip}:1935/live/stream";      ///< RTMP 推流地址模板
    std::string webrtc_url = "http://{server_ip}:8889/live/stream/whip"; ///< WebRTC WHIP 推流服务器地址
    std::string rtsp_url = "rtsp://{server_ip}:8554/live/stream";      ///< RTSP 推流服务器地址
    std::string rtsp_transport = "udp";        ///< RTSP 媒体承载方式，支持 "udp" 或 "tcp"
    std::string server_ip = "192.168.190.128";  ///< 目标流媒体服务器 IP 地址，用于替换模板
    int video_port = 5004;                      ///< RTP 视频流输出端口
    int audio_port = 5006;                      ///< RTP 音频流输出端口
    std::string sdp_path = "test.sdp";          ///< 导出的 SDP 描述文件路径（供 RTP 单播流拉流使用）
};

/**
 * @brief 调试与性能监控配置结构体。
 */
struct DebugConfig {
    bool enable_perf_log = true;                ///< 是否定时输出各流水线节点的延迟与帧率统计日志
    bool enable_preview = false;                ///< 是否开启桌面/Framebuffer 本地预览显示
    bool enable_dump_frame = false;             ///< 是否将采集或编码后的裸流写入本地文件以供排查问题
};

/**
 * @brief 汇总的全局配置结构体。
 */
struct VisionCastConfig {
    VideoConfig video;                          ///< 视频配置子集
    std::map<std::string, VideoConfig> video_profiles; ///< 可选摄像头配置档，由运行脚本按摄像头类型选择
    AudioConfig audio;                          ///< 音频配置子集
    EncoderConfig encoder;                      ///< 编码配置子集
    StreamConfig stream;                        ///< 传输流配置子集
    DebugConfig debug;                          ///< 调试配置子集
};

/**
 * @brief 探测到的 V4L2 摄像头支持的视频格式信息。
 */
struct VideoFormatInfo {
    std::string fourcc;                         ///< 图像格式的 FOURCC 标志字符，例如 "NV12", "MJPG", "YUYV"
    std::string description;                    ///< V4L2 驱动提供的格式描述信息
    std::string buffer_type;                    ///< 缓冲区映射类型（如 Capture, Output 等）
};

/**
 * @brief 视频硬件设备 V4L2 探测结果。
 */
struct VideoProbeResult {
    bool available = false;                     ///< 该设备节点是否可正常打开并访问
    std::string device;                         ///< 探测的设备节点路径（如 /dev/video11）
    std::string driver;                         ///< 驱动名称
    std::string card;                           ///< 声卡/摄像头设备商用名称
    std::string bus_info;                       ///< 总线连接路径信息（如 usb-xhci-hcd.0.auto-1.1）
    std::uint32_t capabilities = 0;             ///< 设备底层能力标志位集合
    std::uint32_t device_caps = 0;              ///< 具体视频设备节点的特有能力标志位集合
    std::vector<VideoFormatInfo> formats;       ///< 该节点支持的像素格式列表
    std::string error;                          ///< 探测失败时的具体错误详情
};

/**
 * @brief 音频硬件设备 ALSA 探测结果。
 */
struct AudioProbeResult {
    bool available = false;                     ///< 目标音频设备是否可以正常打开并使用
    std::string device;                         ///< 探测的 ALSA 设备字符串（如 hw:1,0）
    int card = -1;                              ///< 声卡索引号（-1 表示未知或无效）
    int pcm_device = -1;                        ///< PCM 设备索引号
    std::string card_name;                      ///< 声卡系统商用名称
    std::string pcm_entry;                      ///< ALSA 系统注册的 PCM 设备项描述
    std::string error;                          ///< 探测失败时的具体错误详情
};

}  // namespace visioncast
