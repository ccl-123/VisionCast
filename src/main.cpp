/**
 * @file main.cpp
 * @brief VisionCast Application Entry Point
 * 
 * 本文件是 VisionCast 系统的入口函数文件。主要负责：
 * 1. 命令行参数（CLI Options）的解析与校验。
 * 2. 从本地 JSON 配置文件加载全局系统配置，并支持通过命令行选项覆盖特定参数。
 * 3. 统一处理服务器 IP 模板替换（如将 RTMP/WebRTC 地址中的 `{server_ip}` 替换为实际 IP）。
 * 4. 运行硬件设备探测（--probe）或启动音视频流水线引擎（PipelineManager::run_stream）。
 */

#include <iostream>
#include <exception>
#include <optional>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "pipeline/pipeline_manager.h"

namespace {

/**
 * @brief 打印命令行帮助说明信息。
 * @param program 当前执行的文件路径（argv[0]）。
 */
void print_help(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  -c, --config <path>     Load config file (default: config/visioncast_config.json)\n"
        << "      --protocol <mode>   Override stream protocol: rtp, rtmp or webrtc\n"
        << "      --rtmp-url <url>    Override RTMP push URL\n"
        << "      --webrtc-url <url>  Override WebRTC WHIP URL\n"
        << "      --server-ip <ip>    Override RTP receiver IP\n"
        << "      --video-port <n>    Override RTP video port\n"
        << "      --audio-port <n>    Override RTP audio port\n"
        << "      --sdp-path <path>   Override generated SDP path\n"
        << "      --probe             Probe configured media devices and exit\n"
        << "      --require-devices   Return non-zero if configured devices are missing\n"
        << "      --debug             Enable debug logging\n"
        << "  -h, --help              Show this help\n"
        << "      --version           Show version\n";
}

/**
 * @brief 解析整型数值字符串（主要用于端口号解析）。
 * 
 * @param text 输入的字符串数字。
 * @param value 输出参数，解析成功的整数值。
 * @return 若完整解析为整数且无冗余字符返回 true；否则返回 false。
 */
bool parse_int(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed, 10);
        if (consumed != text.size()) {
            return false; // 如果有非数字后缀，判定为非法
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/visioncast_config.json"; // 默认配置文件路径
    bool probe = false;                                       // 是否为设备探测模式
    bool require_devices = false;                             // 探测时，若配置的设备缺失，是否强制返回非零错误码
    std::optional<std::string> protocol_override;
    std::optional<std::string> rtmp_url_override;
    std::optional<std::string> webrtc_url_override;
    std::optional<std::string> server_ip_override;
    std::optional<int> video_port_override;
    std::optional<int> audio_port_override;
    std::optional<std::string> sdp_path_override;

    // 循环扫描并解析命令行传入的参数
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "VisionCast 0.1.0\n";
            return 0;
        }
        if (arg == "-c" || arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << '\n';
                return 1;
            }
            config_path = argv[++i];
            continue;
        }
        // 处理各种推流参数的参数覆盖选项
        if (arg == "--protocol" || arg == "--rtmp-url" || arg == "--webrtc-url" ||
            arg == "--server-ip" || arg == "--video-port" || arg == "--audio-port" ||
            arg == "--sdp-path") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << arg << '\n';
                return 1;
            }
            std::string value = argv[++i];
            if (arg == "--protocol") {
                protocol_override = value;
            } else if (arg == "--rtmp-url") {
                rtmp_url_override = value;
            } else if (arg == "--webrtc-url") {
                webrtc_url_override = value;
            } else if (arg == "--server-ip") {
                server_ip_override = value;
            } else if (arg == "--sdp-path") {
                sdp_path_override = value;
            } else {
                int parsed = 0;
                // 解析并校验端口号范围 (1 ~ 65535)
                if (!parse_int(value, parsed) || parsed <= 0 || parsed > 65535) {
                    std::cerr << "invalid port for " << arg << ": " << value << '\n';
                    return 1;
                }
                if (arg == "--video-port") {
                    video_port_override = parsed;
                } else if (arg == "--audio-port") {
                    audio_port_override = parsed;
                }
            }
            continue;
        }
        if (arg == "--probe") {
            probe = true;
            continue;
        }
        if (arg == "--require-devices") {
            require_devices = true;
            continue;
        }
        if (arg == "--debug") {
            // 将全局日志记录等级修改为 Debug，以便查看极详细的底层软硬件输出日志
            visioncast::set_log_level(visioncast::LogLevel::Debug);
            continue;
        }

        std::cerr << "unknown option: " << arg << "\n\n";
        print_help(argv[0]);
        return 1;
    }

    visioncast::VisionCastConfig config;
    std::string error;
    // 加载配置文件（包含轻量级 JSON 文本手动解析过程）
    if (!visioncast::load_config_file(config_path, config, error)) {
        VC_LOG_ERROR("main", error);
        return 1;
    }

    // 应用命令行传入的参数，覆盖配置文件中的对应值
    if (protocol_override) config.stream.protocol = *protocol_override;
    if (rtmp_url_override) config.stream.rtmp_url = *rtmp_url_override;
    if (webrtc_url_override) config.stream.webrtc_url = *webrtc_url_override;
    if (server_ip_override) config.stream.server_ip = *server_ip_override;
    if (video_port_override) config.stream.video_port = *video_port_override;
    if (audio_port_override) config.stream.audio_port = *audio_port_override;
    if (sdp_path_override) config.stream.sdp_path = *sdp_path_override;

    // 统一用最终确定的 server_ip 替换 URL 中的占位符
    visioncast::replace_all(config.stream.rtmp_url, "{server_ip}", config.stream.server_ip);
    visioncast::replace_all(config.stream.webrtc_url, "{server_ip}", config.stream.server_ip);

    if (!visioncast::validate_config(config, error)) {
        VC_LOG_ERROR("main", error);
        return 1;
    }

    VC_LOG_INFO("main", "loaded config: " + config_path);
    // 初始化流水线管理器，它将统一构建和控制采集、转换、编码以及网络推送
    visioncast::PipelineManager manager(config);
    manager.print_config();

    if (probe) {
        // 探测模式：探测系统上的音频设备(ALSA)和视频摄像头(V4L2)并输出报告
        return manager.run_probe(require_devices);
    }

    // 正常流媒体发布运行模式：启动并运行各组件流水线
    return manager.run_stream();
}
