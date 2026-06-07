#include "common/config.h"

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace visioncast {
namespace {

bool read_file(const std::string& path, std::string& content, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "failed to open config file: " + path;
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    content = buffer.str();
    return true;
}

void skip_ws(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

std::string parse_string_literal(const std::string& text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') {
        throw std::runtime_error("expected string literal");
    }

    ++pos;
    std::string value;
    while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '"') {
            return value;
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (pos >= text.size()) {
            throw std::runtime_error("unterminated escape sequence");
        }
        char escaped = text[pos++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                value.push_back(escaped);
                break;
            case 'b':
                value.push_back('\b');
                break;
            case 'f':
                value.push_back('\f');
                break;
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                throw std::runtime_error("unsupported escape sequence");
        }
    }

    throw std::runtime_error("unterminated string literal");
}

void skip_value(const std::string& text, std::size_t& pos) {
    skip_ws(text, pos);
    if (pos >= text.size()) {
        return;
    }

    if (text[pos] == '"') {
        parse_string_literal(text, pos);
        return;
    }

    if (text[pos] == '{') {
        int depth = 0;
        while (pos < text.size()) {
            if (text[pos] == '"') {
                parse_string_literal(text, pos);
                continue;
            }
            if (text[pos] == '{') {
                ++depth;
            } else if (text[pos] == '}') {
                --depth;
                ++pos;
                if (depth == 0) {
                    return;
                }
                continue;
            }
            ++pos;
        }
        throw std::runtime_error("unterminated object value");
    }

    if (text[pos] == '[') {
        int depth = 0;
        while (pos < text.size()) {
            if (text[pos] == '"') {
                parse_string_literal(text, pos);
                continue;
            }
            if (text[pos] == '[') {
                ++depth;
            } else if (text[pos] == ']') {
                --depth;
                ++pos;
                if (depth == 0) {
                    return;
                }
                continue;
            }
            ++pos;
        }
        throw std::runtime_error("unterminated array value");
    }

    while (pos < text.size() && text[pos] != ',' && text[pos] != '}') {
        ++pos;
    }
}

std::optional<std::size_t> find_key_value_pos(const std::string& object,
                                               const std::string& key) {
    std::size_t pos = 0;
    while (pos < object.size()) {
        skip_ws(object, pos);
        if (pos >= object.size()) {
            return std::nullopt;
        }
        if (object[pos] != '"') {
            ++pos;
            continue;
        }

        std::size_t key_start = pos;
        std::string candidate = parse_string_literal(object, pos);
        skip_ws(object, pos);
        if (pos >= object.size() || object[pos] != ':') {
            pos = key_start + 1;
            continue;
        }
        ++pos;
        skip_ws(object, pos);
        if (candidate == key) {
            return pos;
        }
        skip_value(object, pos);
    }

    return std::nullopt;
}

std::optional<std::string> find_object(const std::string& document, const std::string& key) {
    auto value_pos = find_key_value_pos(document, key);
    if (!value_pos || *value_pos >= document.size() || document[*value_pos] != '{') {
        return std::nullopt;
    }

    std::size_t start = *value_pos;
    std::size_t pos = start;
    int depth = 0;
    while (pos < document.size()) {
        char ch = document[pos];
        if (ch == '"') {
            parse_string_literal(document, pos);
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return document.substr(start, pos - start + 1);
            }
        }
        ++pos;
    }

    throw std::runtime_error("unterminated object for key: " + key);
}

std::optional<std::string> string_value(const std::string& object, const std::string& key) {
    auto pos = find_key_value_pos(object, key);
    if (!pos || *pos >= object.size() || object[*pos] != '"') {
        return std::nullopt;
    }
    std::size_t parse_pos = *pos;
    return parse_string_literal(object, parse_pos);
}

std::optional<int> int_value(const std::string& object, const std::string& key) {
    auto pos = find_key_value_pos(object, key);
    if (!pos) {
        return std::nullopt;
    }
    std::size_t parse_pos = *pos;
    if (parse_pos < object.size() && object[parse_pos] == '-') {
        ++parse_pos;
    }
    if (parse_pos >= object.size() || std::isdigit(static_cast<unsigned char>(object[parse_pos])) == 0) {
        return std::nullopt;
    }

    std::size_t end_pos = parse_pos;
    while (end_pos < object.size() && std::isdigit(static_cast<unsigned char>(object[end_pos])) != 0) {
        ++end_pos;
    }

    return std::stoi(object.substr(*pos, end_pos - *pos));
}

std::optional<bool> bool_value(const std::string& object, const std::string& key) {
    auto pos = find_key_value_pos(object, key);
    if (!pos) {
        return std::nullopt;
    }

    if (object.compare(*pos, 4, "true") == 0) {
        return true;
    }
    if (object.compare(*pos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

void apply_video_config(const std::string& object, VideoConfig& video) {
    if (auto value = string_value(object, "source")) video.source = *value;
    if (auto value = string_value(object, "fallback_source")) video.fallback_source = *value;
    if (auto value = string_value(object, "device")) video.device = *value;
    if (auto value = string_value(object, "fallback_device")) video.fallback_device = *value;
    if (auto value = string_value(object, "fallback_format")) video.fallback_format = *value;
    if (auto value = int_value(object, "width")) video.width = *value;
    if (auto value = int_value(object, "height")) video.height = *value;
    if (auto value = int_value(object, "fps")) video.fps = *value;
    if (auto value = string_value(object, "format")) video.format = *value;
}

void apply_audio_config(const std::string& object, AudioConfig& audio) {
    if (auto value = string_value(object, "device")) audio.device = *value;
    if (auto value = int_value(object, "sample_rate")) audio.sample_rate = *value;
    if (auto value = int_value(object, "channels")) audio.channels = *value;
    if (auto value = string_value(object, "format")) audio.format = *value;
    if (auto value = int_value(object, "frame_ms")) audio.frame_ms = *value;
}

void apply_encoder_config(const std::string& object, EncoderConfig& encoder) {
    if (auto value = string_value(object, "video_codec")) encoder.video_codec = *value;
    if (auto value = int_value(object, "bitrate")) encoder.bitrate = *value;
    if (auto value = int_value(object, "gop")) encoder.gop = *value;
    if (auto value = bool_value(object, "low_latency")) encoder.low_latency = *value;
    if (auto value = int_value(object, "b_frames")) encoder.b_frames = *value;
}

void apply_stream_config(const std::string& object, StreamConfig& stream) {
    if (auto value = string_value(object, "protocol")) stream.protocol = *value;
    if (auto value = string_value(object, "rtmp_url")) stream.rtmp_url = *value;
    if (auto value = string_value(object, "webrtc_url")) stream.webrtc_url = *value;
    if (auto value = string_value(object, "server_ip")) stream.server_ip = *value;
    if (auto value = int_value(object, "video_port")) stream.video_port = *value;
    if (auto value = int_value(object, "audio_port")) stream.audio_port = *value;
    if (auto value = string_value(object, "sdp_path")) stream.sdp_path = *value;
}

void apply_debug_config(const std::string& object, DebugConfig& debug) {
    if (auto value = bool_value(object, "enable_perf_log")) debug.enable_perf_log = *value;
    if (auto value = bool_value(object, "enable_dump_frame")) debug.enable_dump_frame = *value;
}

std::string bool_text(bool value) {
    return value ? "true" : "false";
}

}  // namespace

void replace_all(std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return;
    }
    std::size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

VisionCastConfig default_config() {
    return VisionCastConfig{};
}

bool load_config_file(const std::string& path, VisionCastConfig& config, std::string& error) {
    config = default_config();

    std::string document;
    if (!read_file(path, document, error)) {
        return false;
    }

    try {
        if (auto object = find_object(document, "video")) apply_video_config(*object, config.video);
        if (auto object = find_object(document, "audio")) apply_audio_config(*object, config.audio);
        if (auto object = find_object(document, "encoder")) apply_encoder_config(*object, config.encoder);
        if (auto object = find_object(document, "stream")) apply_stream_config(*object, config.stream);
        if (auto object = find_object(document, "debug")) apply_debug_config(*object, config.debug);
    } catch (const std::exception& ex) {
        error = std::string("invalid config: ") + ex.what();
        return false;
    }

    return true;
}

std::string summarize_config(const VisionCastConfig& config) {
    std::ostringstream out;
    out << "Video: source=" << config.video.source
        << ", device=" << config.video.device
        << ", fallback=" << config.video.fallback_device
        << ", fallback_format=" << config.video.fallback_format
        << ", " << config.video.width << "x" << config.video.height
        << "@" << config.video.fps
        << ", format=" << config.video.format << '\n';
    out << "Audio: device=" << config.audio.device
        << ", sample_rate=" << config.audio.sample_rate
        << ", channels=" << config.audio.channels
        << ", format=" << config.audio.format
        << ", frame_ms=" << config.audio.frame_ms << '\n';
    out << "Encoder: codec=" << config.encoder.video_codec
        << ", bitrate=" << config.encoder.bitrate
        << ", gop=" << config.encoder.gop
        << ", low_latency=" << bool_text(config.encoder.low_latency)
        << ", b_frames=" << config.encoder.b_frames << '\n';
    out << "Stream: protocol=" << config.stream.protocol
        << ", rtmp_url=" << config.stream.rtmp_url
        << ", webrtc_url=" << config.stream.webrtc_url
        << ", server_ip=" << config.stream.server_ip
        << ", video_port=" << config.stream.video_port
        << ", audio_port=" << config.stream.audio_port
        << ", sdp_path=" << config.stream.sdp_path << '\n';
    out << "Debug: perf_log=" << bool_text(config.debug.enable_perf_log)
        << ", dump_frame=" << bool_text(config.debug.enable_dump_frame);
    return out.str();
}

}  // namespace visioncast
