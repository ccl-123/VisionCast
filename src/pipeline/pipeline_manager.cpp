/**
 * @file pipeline_manager.cpp
 * @brief VisionCast 流水线管理中心的实现
 * @details 实现了 PipelineManager 类。包括探测系统音视频硬件设备状态（V4L2, ALSA）、
 *          格式化控制台输出设备信息、创建 SDP 配置文件，以及控制音视频流水线的启动、
 *          健康监测与信号捕获（SIGINT/SIGTERM）处理以实现优雅退出。
 */

#include "pipeline/pipeline_manager.h"

#include <csignal>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <utility>

#include "common/config.h"
#include "common/log.h"
#include "pipeline/av_pipeline.h"

namespace visioncast {
namespace {

// 全局原子信号标志，用于在接收到 SIGINT 或 SIGTERM 时通知主线程安全退出
volatile std::sig_atomic_t g_stop_requested = 0;

// 信号处理回调函数
void signal_handler(int) {
    g_stop_requested = 1;
}

// 辅助函数：将布尔值转为 "yes" / "no"
std::string yes_no(bool value) {
    return value ? "yes" : "no";
}

// 辅助函数：打印 V4L2 视频设备的详细探测结果
void print_video_result(const std::string& label, const VideoProbeResult& result) {
    std::cout << label << " video device: " << result.device << '\n';
    std::cout << "  available: " << yes_no(result.available) << '\n';
    if (!result.available) {
        std::cout << "  error: " << result.error << '\n';
        return;
    }

    std::cout << "  driver: " << result.driver << '\n';
    std::cout << "  card: " << result.card << '\n';
    std::cout << "  bus: " << result.bus_info << '\n';
    std::cout << "  formats:";
    if (result.formats.empty()) {
        std::cout << " none reported";
    }
    std::cout << '\n';
    for (const auto& format : result.formats) {
        std::cout << "    - " << format.fourcc
                  << " (" << format.description
                  << ", " << format.buffer_type << ")\n";
    }
}

// 辅助函数：打印 ALSA 音频设备的详细探测结果
void print_audio_result(const AudioProbeResult& result) {
    std::cout << "Audio device: " << result.device << '\n';
    std::cout << "  available: " << yes_no(result.available) << '\n';
    if (!result.card_name.empty()) {
        std::cout << "  card: " << result.card_name << '\n';
    }
    if (!result.pcm_entry.empty()) {
        std::cout << "  pcm: " << result.pcm_entry << '\n';
    }
    if (!result.available) {
        std::cout << "  error: " << result.error << '\n';
    }
}

}  // namespace

PipelineManager::PipelineManager(VisionCastConfig config)
    : config_(std::move(config)) {}

void PipelineManager::print_config() const {
    // 调用全局辅助函数，打印解析后的最终推流和编码参数
    std::cout << summarize_config(config_) << "\n";
}

PipelineProbeSummary PipelineManager::probe_devices() const {
    PipelineProbeSummary summary;
    // 1. 探测配置的主视频设备
    summary.primary_video = VideoCapture::probe_device(config_.video.device);

    const bool has_fallback = !config_.video.fallback_device.empty() &&
                              config_.video.fallback_device != config_.video.device;
    // 2. 若主视频设备不可用且配置了备用设备，则探测备用视频设备
    if (!summary.primary_video.available && has_fallback) {
        summary.fallback_checked = true;
        summary.fallback_video = VideoCapture::probe_device(config_.video.fallback_device);
    }

    // 视频就绪条件：主设备可用，或者备用设备可用
    summary.video_ready = summary.primary_video.available ||
                          (summary.fallback_checked && summary.fallback_video.available);
    // 3. 探测音频设备状态
    summary.audio = AudioCapture::probe_device(config_.audio.device);
    summary.audio_ready = summary.audio.available;
    return summary;
}

int PipelineManager::run_probe(bool require_devices) const {
    VC_LOG_INFO("pipeline", "probing configured media devices");
    PipelineProbeSummary summary = probe_devices();

    // 格式化输出所有音视频设备探测的详细规格
    std::cout << "\nProbe result\n";
    std::cout << "------------\n";
    print_video_result("Primary", summary.primary_video);
    if (summary.fallback_checked) {
        print_video_result("Fallback", summary.fallback_video);
    }
    print_audio_result(summary.audio);

    std::cout << "\nReadiness\n";
    std::cout << "  video_ready: " << yes_no(summary.video_ready) << '\n';
    std::cout << "  audio_ready: " << yes_no(summary.audio_ready) << '\n';
    std::cout << std::flush;

    // 如果指定了 require_devices 为真，且音视频有不可用者，则直接返回错误码以终止推流
    if (require_devices && (!summary.video_ready || !summary.audio_ready)) {
        VC_LOG_ERROR("pipeline", "required media devices are not ready");
        return 2;
    }

    if (!summary.video_ready) {
        VC_LOG_WARN("pipeline", "video device is not ready; this is expected on non-RK3588 hosts");
    }
    if (!summary.audio_ready) {
        VC_LOG_WARN("pipeline", "audio capture device is not ready; this is expected on hosts without NAU8822");
    }

    return 0;
}

bool PipelineManager::write_sdp_file(std::string& error) const {
    std::filesystem::path path(config_.stream.sdp_path);
    if (path.empty()) {
        error = "sdp_path is empty";
        return false;
    }
    // 自动创建 SDP 输出文件所在的父目录
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "failed to create SDP directory: " + ec.message();
            return false;
        }
    }

    std::ofstream out(path);
    if (!out) {
        error = "failed to open SDP file: " + path.string();
        return false;
    }

    // 写入标准的 SDP 配置文本格式，用于单播/组播 RTP 串流
    // m=video 部分指定 RTP/AVP H264 荷载格式 (PT=96)
    // m=audio 部分指定 PCMA G.711a 荷载格式 (PT=8)
    out << "v=0\n"
        << "o=- 0 0 IN IP4 127.0.0.1\n"
        << "s=VisionCast RTP Stream\n"
        << "c=IN IP4 " << config_.stream.server_ip << "\n"
        << "t=0 0\n"
        << "m=video " << config_.stream.video_port << " RTP/AVP 96\n"
        << "a=rtpmap:96 H264/90000\n"
        << "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\n"
        << "a=recvonly\n"
        << "m=audio " << config_.stream.audio_port << " RTP/AVP 8\n"
        << "a=rtpmap:8 PCMA/8000/1\n"
        << "a=recvonly\n";
    return true;
}

int PipelineManager::run_stream() {
    // 支持协议的完整性检测，包括 rtp, rtmp, webrtc
    if (config_.stream.protocol != "rtp" &&
        config_.stream.protocol != "rtmp" &&
        config_.stream.protocol != "webrtc") {
        VC_LOG_ERROR("pipeline", "unsupported stream protocol=" + config_.stream.protocol);
        return 1;
    }

    std::string error;
    if (config_.stream.protocol == "rtp") {
        // 如果是纯 RTP 发送，则导出供播放客户端使用的 SDP 文件
        if (!write_sdp_file(error)) {
            VC_LOG_ERROR("pipeline", error);
            return 1;
        }
        VC_LOG_INFO("pipeline", "wrote SDP: " + config_.stream.sdp_path);
        VC_LOG_INFO("pipeline",
                    "push target video=" + config_.stream.server_ip + ":" +
                        std::to_string(config_.stream.video_port) + " audio=" +
                        config_.stream.server_ip + ":" +
                        std::to_string(config_.stream.audio_port));
    } else if (config_.stream.protocol == "rtmp") {
        VC_LOG_INFO("pipeline", "RTMP push target=" + config_.stream.rtmp_url);
    } else {
        VC_LOG_INFO("pipeline", "WebRTC WHIP push target=" + config_.stream.webrtc_url);
    }

    // 注册信号处理器以捕获 Ctrl+C 或系统的 kill 命令，确保能够正常清理硬件上下文
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    g_stop_requested = 0;

    // 创建并开启音视频联合流水线
    AvPipeline pipeline(config_);
    if (!pipeline.start(error)) {
        VC_LOG_ERROR("pipeline", error);
        pipeline.stop();
        return 1;
    }

    VC_LOG_INFO("pipeline", "VisionCast streaming started; press Ctrl+C to stop");
    int result = 0;
    
    // 主循环：只要未收到退出信号就持续运行
    while (g_stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        
        // 动态监控流水线健康状态，如果有任何子流水线发生异常停止，则结束推流
        if (!pipeline.is_healthy()) {
            VC_LOG_ERROR("pipeline", pipeline.error());
            result = 1;
            break;
        }
    }

    // 接收到退出指令，开始执行优雅关机流程
    VC_LOG_INFO("pipeline", "shutting down media pipeline");
    pipeline.stop();
    return result;
}

}  // namespace visioncast
