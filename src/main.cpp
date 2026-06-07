#include <iostream>
#include <exception>
#include <optional>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "pipeline/pipeline_manager.h"

namespace {

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

bool parse_int(const std::string& text, int& value) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/visioncast_config.json";
    bool probe = false;
    bool require_devices = false;
    std::optional<std::string> protocol_override;
    std::optional<std::string> rtmp_url_override;
    std::optional<std::string> webrtc_url_override;
    std::optional<std::string> server_ip_override;
    std::optional<int> video_port_override;
    std::optional<int> audio_port_override;
    std::optional<std::string> sdp_path_override;

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
            visioncast::set_log_level(visioncast::LogLevel::Debug);
            continue;
        }

        std::cerr << "unknown option: " << arg << "\n\n";
        print_help(argv[0]);
        return 1;
    }

    visioncast::VisionCastConfig config;
    std::string error;
    if (!visioncast::load_config_file(config_path, config, error)) {
        VC_LOG_ERROR("main", error);
        return 1;
    }

    if (protocol_override) config.stream.protocol = *protocol_override;
    if (rtmp_url_override) config.stream.rtmp_url = *rtmp_url_override;
    if (webrtc_url_override) config.stream.webrtc_url = *webrtc_url_override;
    if (server_ip_override) config.stream.server_ip = *server_ip_override;
    if (video_port_override) config.stream.video_port = *video_port_override;
    if (audio_port_override) config.stream.audio_port = *audio_port_override;
    if (sdp_path_override) config.stream.sdp_path = *sdp_path_override;

    VC_LOG_INFO("main", "loaded config: " + config_path);
    visioncast::PipelineManager manager(config);
    manager.print_config();

    if (probe) {
        return manager.run_probe(require_devices);
    }

    return manager.run_stream();
}
