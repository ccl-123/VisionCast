/**
 * @file config.cpp
 * @brief VisionCast Configuration Management Implementation
 * 
 * 本文件实现了 VisionCast 系统配置的管理与解析。为了避免对第三方复杂 JSON 库的依赖，
 * 此处实现了一个基于文本扫描的轻量级 JSON 解析器。它支持提取嵌套对象、解析字符串字面量、
 * 整数及布尔值，并将解析到的字段应用到 VisionCastConfig 相应的子结构体中。
 */

#include "common/config.h"

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace visioncast {
namespace {

/**
 * @brief 从指定文件路径中读取全部文本内容。
 * 
 * @param path 文件的绝对/相对路径。
 * @param content 输出参数，若读取成功则将文件全部内容存入此变量。
 * @param error 输出参数，若读取失败，填充错误描述信息。
 * @return 成功返回 true，失败返回 false。
 */
bool read_file(const std::string& path, std::string& content, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "failed to open config file: " + path;
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf(); // 读取整个文件缓冲区
    content = buffer.str();
    return true;
}

/**
 * @brief 跳过当前的空白字符（包括空格、制表符、换行等）。
 * 
 * @param text 输入文本字符串。
 * @param pos 当前扫描的位置索引（输入输出参数）。
 */
void skip_ws(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

/**
 * @brief 解析 JSON 格式的字符串字面量（包含转义字符处理）。
 * 
 * @param text 输入文本字符串。
 * @param pos 当前扫描位置索引，必须指向开头的双引号 `"`。
 * @return 解析得到的原始字符串内容。
 * @throws std::runtime_error 若字面量格式非法或包含未终结的转义序列。
 */
std::string parse_string_literal(const std::string& text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') {
        throw std::runtime_error("expected string literal");
    }

    ++pos; // 跳过开头的双引号
    std::string value;
    while (pos < text.size()) {
        char ch = text[pos++];
        if (ch == '"') {
            return value; // 遇到配对的结束双引号，成功返回
        }
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        // 处理转义字符
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

/**
 * @brief 跳过当前的 JSON 值（包括字面量、嵌套的 {} 对象或 [] 数组）。
 * 
 * 用于在查找 Key 时跳过不匹配的 Value 块。
 * 
 * @param text 输入文本字符串。
 * @param pos 当前扫描位置索引。
 */
void skip_value(const std::string& text, std::size_t& pos) {
    skip_ws(text, pos);
    if (pos >= text.size()) {
        return;
    }

    // 若值是字符串字面量，直接解析并跳过
    if (text[pos] == '"') {
        parse_string_literal(text, pos);
        return;
    }

    // 若值是嵌套对象 {}，通过括号匹配跳过整个对象
    if (text[pos] == '{') {
        int depth = 0;
        while (pos < text.size()) {
            if (text[pos] == '"') {
                parse_string_literal(text, pos); // 跳过对象内部的字符串，防止混淆括号
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

    // 若值是嵌套数组 []，通过括号匹配跳过整个数组
    if (text[pos] == '[') {
        int depth = 0;
        while (pos < text.size()) {
            if (text[pos] == '"') {
                parse_string_literal(text, pos); // 跳过内部字符串
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

    // 其他常规标量值（数字、布尔、null），扫描直到逗号或对象结束标志 `}`
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}') {
        ++pos;
    }
}

/**
 * @brief 在给定的 JSON 对象字符串中定位指定 Key 对应的 Value 起始位置。
 * 
 * @param object JSON 对象的字符串内容（通常包裹在 `{}` 内）。
 * @param key 需要定位的字段名称。
 * @return 若找到该 Key，返回对应 Value 字段的第一个非空白字符位置；否则返回 std::nullopt。
 */
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
            pos = key_start + 1; // 格式不符，回溯扫描下一个位置
            continue;
        }
        ++pos; // 跳过冒号 `:`
        skip_ws(object, pos);
        if (candidate == key) {
            return pos; // 匹配成功，返回当前位置
        }
        skip_value(object, pos); // 不匹配，跳过当前键对应的值继续搜寻
    }

    return std::nullopt;
}

/**
 * @brief 在文档中查找嵌套的 JSON 子对象。
 * 
 * @param document 父 JSON 文档内容。
 * @param key 对象的字段名（如 "video", "audio"）。
 * @return 若找到，以 string 形式返回该子对象 `{ ... }`；否则返回 std::nullopt。
 */
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
            parse_string_literal(document, pos); // 跳过字符串以防括号误判
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return document.substr(start, pos - start + 1); // 提取完整的对象子串
            }
        }
        ++pos;
    }

    throw std::runtime_error("unterminated object for key: " + key);
}

/**
 * @brief 从对象中解析指定 Key 的字符串类型值。
 * 
 * @param object JSON 对象的字符串内容。
 * @param key 字段名称。
 * @return 若找到且为有效字符串，返回解析后的 std::string；否则返回 std::nullopt。
 */
std::optional<std::string> string_value(const std::string& object, const std::string& key) {
    auto pos = find_key_value_pos(object, key);
    if (!pos || *pos >= object.size() || object[*pos] != '"') {
        return std::nullopt;
    }
    std::size_t parse_pos = *pos;
    return parse_string_literal(object, parse_pos);
}

/**
 * @brief 从对象中解析指定 Key 的整型数值。
 * 
 * @param object JSON 对象的字符串内容。
 * @param key 字段名称。
 * @return 若找到且为有效数字，返回解析后的 int；否则返回 std::nullopt。
 */
std::optional<int> int_value(const std::string& object, const std::string& key) {
    auto pos = find_key_value_pos(object, key);
    if (!pos) {
        return std::nullopt;
    }
    std::size_t parse_pos = *pos;
    if (parse_pos < object.size() && object[parse_pos] == '-') {
        ++parse_pos; // 支持负数符号
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

/**
 * @brief 从对象中解析指定 Key 的布尔类型值。
 * 
 * 匹配 "true" 或 "false"。
 * 
 * @param object JSON 对象的字符串内容。
 * @param key 字段名称。
 * @return 若找到且为布尔文本，返回对应的布尔值；否则返回 std::nullopt。
 */
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

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string normalize_video_codec(std::string value) {
    value = lower_ascii(value);
    if (value == "h.264" || value == "avc") {
        return "h264";
    }
    if (value == "h.265" || value == "hevc") {
        return "h265";
    }
    return value;
}

/**
 * @brief 将 JSON 对象的字段填充到 VideoConfig 视频配置结构体中。
 */
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
    if (auto value = string_value(object, "sensor_subdev")) video.sensor_subdev = *value;
    if (auto value = int_value(object, "crop_left")) video.crop_left = *value;
    if (auto value = int_value(object, "crop_top")) video.crop_top = *value;
    if (auto value = int_value(object, "crop_width")) video.crop_width = *value;
    if (auto value = int_value(object, "crop_height")) video.crop_height = *value;
    if (auto value = int_value(object, "sensor_exposure")) video.sensor_exposure = *value;
    if (auto value = int_value(object, "sensor_vblank")) video.sensor_vblank = *value;
    if (auto value = int_value(object, "sensor_analogue_gain")) {
        video.sensor_analogue_gain = *value;
    }
}

/**
 * @brief 将 JSON 对象的字段填充到 AudioConfig 音频配置结构体中。
 */
void apply_audio_config(const std::string& object, AudioConfig& audio) {
    if (auto value = string_value(object, "device")) audio.device = *value;
    if (auto value = int_value(object, "sample_rate")) audio.sample_rate = *value;
    if (auto value = int_value(object, "channels")) audio.channels = *value;
    if (auto value = int_value(object, "capture_channels")) audio.capture_channels = *value;
    if (auto value = string_value(object, "format")) audio.format = *value;
    if (auto value = int_value(object, "frame_ms")) audio.frame_ms = *value;
}

/**
 * @brief 将 JSON 对象的字段填充到 EncoderConfig 编码配置结构体中。
 */
void apply_encoder_config(const std::string& object, EncoderConfig& encoder) {
    if (auto value = string_value(object, "video_codec")) {
        encoder.video_codec = normalize_video_codec(*value);
    }
    if (auto value = int_value(object, "bitrate")) encoder.bitrate = *value;
    if (auto value = int_value(object, "gop")) encoder.gop = *value;
    if (auto value = bool_value(object, "low_latency")) encoder.low_latency = *value;
    if (auto value = int_value(object, "b_frames")) encoder.b_frames = *value;
}

/**
 * @brief 将 JSON 对象的字段填充到 StreamConfig 传输流配置结构体中。
 */
void apply_stream_config(const std::string& object, StreamConfig& stream) {
    if (auto value = string_value(object, "protocol")) stream.protocol = *value;
    if (auto value = string_value(object, "rtmp_url")) stream.rtmp_url = *value;
    if (auto value = string_value(object, "webrtc_url")) stream.webrtc_url = *value;
    if (auto value = string_value(object, "server_ip")) stream.server_ip = *value;
    if (auto value = int_value(object, "video_port")) stream.video_port = *value;
    if (auto value = int_value(object, "audio_port")) stream.audio_port = *value;
    if (auto value = string_value(object, "sdp_path")) stream.sdp_path = *value;
}

/**
 * @brief 将 JSON 对象的字段填充到 DebugConfig 调试配置结构体中。
 */
void apply_debug_config(const std::string& object, DebugConfig& debug) {
    if (auto value = bool_value(object, "enable_perf_log")) debug.enable_perf_log = *value;
    if (auto value = bool_value(object, "enable_preview")) debug.enable_preview = *value;
    if (auto value = bool_value(object, "enable_dump_frame")) debug.enable_dump_frame = *value;
}

/**
 * @brief 格式化输出布尔文本的辅助函数。
 */
std::string bool_text(bool value) {
    return value ? "true" : "false";
}

bool is_rtp_opus_protocol(const std::string& protocol) {
    return protocol == "rtp" || protocol == "webrtc";
}

bool is_supported_opus_sample_rate(int sample_rate) {
    return sample_rate == 8000 ||
           sample_rate == 12000 ||
           sample_rate == 16000 ||
           sample_rate == 24000 ||
           sample_rate == 48000;
}

bool is_supported_opus_frame_ms(int frame_ms) {
    return frame_ms == 5 ||
           frame_ms == 10 ||
           frame_ms == 20 ||
           frame_ms == 40 ||
           frame_ms == 60;
}

bool is_supported_video_codec(const std::string& codec) {
    return codec == "h264" || codec == "h265";
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
    return VisionCastConfig{}; // 默认初始化值详见 types.h
}

bool load_config_file(const std::string& path, VisionCastConfig& config, std::string& error) {
    config = default_config();

    std::string document;
    if (!read_file(path, document, error)) {
        return false;
    }

    try {
        // 依次解析顶层 key：video, audio, encoder, stream, debug 并填充到配置中
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

bool validate_config(const VisionCastConfig& config, std::string& error) {
    if (!is_supported_video_codec(config.encoder.video_codec)) {
        error = "encoder.video_codec must be h264 or h265";
        return false;
    }

    if (config.audio.channels != 1 && config.audio.channels != 2) {
        error = "audio.channels must be 1 or 2";
        return false;
    }

    if (config.audio.capture_channels != 0 &&
        config.audio.capture_channels != 1 &&
        config.audio.capture_channels != 2) {
        error = "audio.capture_channels must be 0, 1 or 2";
        return false;
    }

    if (config.audio.frame_ms <= 0) {
        error = "audio.frame_ms must be positive";
        return false;
    }

    if (is_rtp_opus_protocol(config.stream.protocol)) {
        if (!is_supported_opus_sample_rate(config.audio.sample_rate)) {
            error = "RTP/WebRTC Opus audio.sample_rate must be one of 8000, 12000, 16000, 24000, 48000";
            return false;
        }
        if (!is_supported_opus_frame_ms(config.audio.frame_ms)) {
            error = "RTP/WebRTC Opus audio.frame_ms must be one of 5, 10, 20, 40, 60";
            return false;
        }
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
        << ", format=" << config.video.format;
    if (!config.video.sensor_subdev.empty()) {
        out << ", sensor_subdev=" << config.video.sensor_subdev
            << ", crop=" << config.video.crop_left
            << "," << config.video.crop_top
            << "," << config.video.crop_width
            << "x" << config.video.crop_height
            << ", sensor_exposure=" << config.video.sensor_exposure
            << ", sensor_vblank=" << config.video.sensor_vblank
            << ", sensor_analogue_gain=" << config.video.sensor_analogue_gain;
    }
    out << '\n';
    out << "Audio: device=" << config.audio.device
        << ", sample_rate=" << config.audio.sample_rate
        << ", capture_channels="
        << (config.audio.capture_channels > 0 ? config.audio.capture_channels : config.audio.channels)
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
        << ", preview=" << bool_text(config.debug.enable_preview)
        << ", dump_frame=" << bool_text(config.debug.enable_dump_frame);
    return out.str();
}

}  // namespace visioncast
