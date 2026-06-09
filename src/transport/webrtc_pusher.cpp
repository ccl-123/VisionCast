/**
 * @file webrtc_pusher.cpp
 * @brief VisionCast WebRTC WHIP 推流客户端模块实现文件
 * 
 * 本文件使用项目内置 FFmpeg 8.x 共享库完成 WHIP/WebRTC 封装与传输。
 * 视频硬编码输出的 Annex-B H.264/H.265 码流与 Opus 音频包，直接写入 FFmpeg whip muxer，
 * 由其自动完成 HTTP SDP 协商、STUN 绑定、DTLS 握手和 SRTP 加密传输。
 */

#include "transport/webrtc_pusher.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include "common/log.h"

#if defined(VISIONCAST_ENABLE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
}
#endif

namespace visioncast {

struct WhipNaluSpan {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

#if defined(VISIONCAST_ENABLE_FFMPEG)
struct CallbackOpaque {
    std::chrono::steady_clock::time_point start_time;
    int timeout_ms = 0;
    bool interrupted = false;
    bool disabled = false;
};

static int interrupt_callback(void* ctx) {
    if (!ctx) return 0;
    auto* cb = static_cast<CallbackOpaque*>(ctx);
    if (cb->disabled) {
        return 0;
    }
    if (cb->interrupted) {
        return 1;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - cb->start_time).count();
    if (elapsed > cb->timeout_ms) {
        cb->interrupted = true;
        return 1;
    }
    return 0;
}
#endif

struct WebRtcPusher::Impl {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    AVFormatContext* format = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    std::vector<WhipNaluSpan> video_nalus;
    std::vector<std::uint8_t> video_extradata;
    std::uint64_t first_video_pts_us = 0;
    std::uint64_t first_audio_pts_us = 0;
    bool has_first_video_pts = false;
    bool has_first_audio_pts = false;
    bool header_written = false;
    CallbackOpaque cb_opaque;
#endif
    bool connected = false;
};

namespace {

#if defined(VISIONCAST_ENABLE_FFMPEG)
std::string ffmpeg_error(const char* call, int ret) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(ret, text, sizeof(text));
    return std::string(call) + ": " + text;
}

std::size_t start_code_size(const std::uint8_t* data,
                            std::size_t size,
                            std::size_t pos) {
    if (pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 1) {
        return 3;
    }
    if (pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 &&
        data[pos + 2] == 0 && data[pos + 3] == 1) {
        return 4;
    }
    return 0;
}

bool find_nalus(const std::uint8_t* data,
                std::size_t size,
                std::vector<WhipNaluSpan>& nalus,
                std::string& error) {
    nalus.clear();
    std::size_t pos = 0;
    while (pos < size) {
        const std::size_t code = start_code_size(data, size, pos);
        if (code == 0) {
            ++pos;
            continue;
        }
        const std::size_t start = pos + code;
        pos = start;
        while (pos < size && start_code_size(data, size, pos) == 0) {
            ++pos;
        }
        if (pos > start) {
            nalus.push_back(WhipNaluSpan{data + start, pos - start});
        }
    }
    if (nalus.empty()) {
        error = "video frame does not contain Annex-B start-code NAL units";
        return false;
    }
    return true;
}

std::uint8_t h264_nalu_type(const WhipNaluSpan& nalu) {
    return nalu.size > 0 ? static_cast<std::uint8_t>(nalu.data[0] & 0x1FU) : 0;
}

std::uint8_t h265_nalu_type(const WhipNaluSpan& nalu) {
    return nalu.size >= 2
               ? static_cast<std::uint8_t>((nalu.data[0] >> 1U) & 0x3FU)
               : 0;
}

bool is_h264_keyframe(const std::vector<WhipNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        return h264_nalu_type(nalu) == 5;
    });
}

bool is_h265_keyframe(const std::vector<WhipNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        const std::uint8_t type = h265_nalu_type(nalu);
        return type >= 16 && type <= 21;
    });
}

void append_annexb_nalu(std::vector<std::uint8_t>& out, const WhipNaluSpan& nalu) {
    static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
    out.insert(out.end(), nalu.data, nalu.data + nalu.size);
}

bool build_annexb_extradata(VideoCodec codec,
                             const std::vector<WhipNaluSpan>& nalus,
                             std::vector<std::uint8_t>& extradata,
                             std::string& error) {
    extradata.clear();
    bool has_sps = false;
    bool has_pps = false;
    bool has_vps = codec == VideoCodec::H264;
    for (const auto& nalu : nalus) {
        if (nalu.data == nullptr || nalu.size == 0) {
            continue;
        }
        if (codec == VideoCodec::H265) {
            const std::uint8_t type = h265_nalu_type(nalu);
            if (type == 32 || type == 33 || type == 34) {
                append_annexb_nalu(extradata, nalu);
            }
            has_vps = has_vps || type == 32;
            has_sps = has_sps || type == 33;
            has_pps = has_pps || type == 34;
        } else {
            const std::uint8_t type = h264_nalu_type(nalu);
            if (type == 7 || type == 8) {
                append_annexb_nalu(extradata, nalu);
            }
            has_sps = has_sps || type == 7;
            has_pps = has_pps || type == 8;
        }
    }
    if (!has_vps || !has_sps || !has_pps || extradata.empty()) {
        error = codec == VideoCodec::H265
                    ? "first WHIP H265 frame does not contain valid VPS/SPS/PPS"
                    : "first WHIP H264 frame does not contain valid SPS/PPS";
        return false;
    }
    return true;
}

bool assign_extradata(AVCodecParameters* parameters,
                      const std::vector<std::uint8_t>& extradata,
                      std::string& error) {
    parameters->extradata = static_cast<std::uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata == nullptr) {
        error = "failed to allocate WHIP video extradata";
        return false;
    }
    std::memcpy(parameters->extradata, extradata.data(), extradata.size());
    parameters->extradata_size = static_cast<int>(extradata.size());
    return true;
}

bool setup_video_stream(WebRtcPusher::Impl& impl,
                        const VideoConfig& video,
                        const EncoderConfig& encoder,
                        VideoCodec codec,
                        const std::vector<WhipNaluSpan>& nalus,
                        std::string& error) {
    impl.video_stream = avformat_new_stream(impl.format, nullptr);
    if (impl.video_stream == nullptr) {
        error = "avformat_new_stream(video) failed";
        return false;
    }

    impl.video_stream->time_base = AVRational{1, 1000000}; // 微秒基准
    AVCodecParameters* parameters = impl.video_stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id =
        codec == VideoCodec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    parameters->width = video.width;
    parameters->height = video.height;
    parameters->bit_rate = encoder.bitrate;

    if (!build_annexb_extradata(codec, nalus, impl.video_extradata, error)) {
        return false;
    }
    return assign_extradata(parameters, impl.video_extradata, error);
}

bool setup_audio_stream(WebRtcPusher::Impl& impl,
                        const AudioConfig& audio,
                        std::string& error) {
    impl.audio_stream = avformat_new_stream(impl.format, nullptr);
    if (impl.audio_stream == nullptr) {
        error = "avformat_new_stream(audio) failed";
        return false;
    }

    impl.audio_stream->time_base = AVRational{1, 1000000}; // 微秒基准
    AVCodecParameters* parameters = impl.audio_stream->codecpar;
    parameters->codec_type = AVMEDIA_TYPE_AUDIO;
    parameters->codec_id = AV_CODEC_ID_OPUS;
    parameters->sample_rate = audio.sample_rate;
    parameters->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    parameters->ch_layout.nb_channels = audio.channels;
    return true;
}
#endif

}  // namespace

WebRtcPusher::WebRtcPusher(std::string whip_url,
                           VideoConfig video,
                           AudioConfig audio,
                           EncoderConfig encoder)
    : impl_(std::make_unique<Impl>()),
      whip_url_(std::move(whip_url)),
      video_(std::move(video)),
      audio_(std::move(audio)),
      encoder_(std::move(encoder)) {}

WebRtcPusher::~WebRtcPusher() {
    disconnect();
}

bool WebRtcPusher::connect(std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->connected) {
        return true;
    }

    int ret = avformat_alloc_output_context2(
        &impl_->format, nullptr, "whip", whip_url_.c_str());
    if (ret < 0 || impl_->format == nullptr) {
        error = ret < 0 ? ffmpeg_error("avformat_alloc_output_context2(whip)", ret)
                        : "avformat_alloc_output_context2(whip) returned null";
        disconnect();
        return false;
    }

    // 设置 DTLS 主动握手标志以适应 MediaMTX/Pion 信道要求，消除超时风险
    ret = av_opt_set(impl_->format->priv_data, "whip_flags", "dtls_active", 0);
    if (ret < 0) {
        VC_LOG_WARN("webrtc", "Failed to set whip_flags to dtls_active: " + ffmpeg_error("av_opt_set", ret));
    }

    impl_->connected = true;
    VC_LOG_INFO("webrtc", "WebRTC/WHIP format context allocated for " + whip_url_);
    return true;
#else
    error = "FFmpeg support is not enabled in this build";
    return false;
#endif
}

void WebRtcPusher::disconnect() {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->format != nullptr && impl_->header_written) {
        av_write_trailer(impl_->format);
    }
    if (impl_->format != nullptr) {
        avformat_free_context(impl_->format);
        impl_->format = nullptr;
    }
    impl_->video_stream = nullptr;
    impl_->audio_stream = nullptr;
    impl_->video_nalus.clear();
    impl_->video_extradata.clear();
    impl_->first_video_pts_us = 0;
    impl_->first_audio_pts_us = 0;
    impl_->has_first_video_pts = false;
    impl_->has_first_audio_pts = false;
    impl_->header_written = false;
#endif
    impl_->connected = false;
}

bool WebRtcPusher::push_video(const EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || packet.data.empty()) {
        error = "WebRTC pusher is not connected or video frame is empty";
        return false;
    }
    if (packet.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "WebRTC video packet is too large";
        return false;
    }
    if (!find_nalus(packet.data.data(), packet.data.size(), impl_->video_nalus, error)) {
        return false;
    }

    if (!impl_->header_written) {
        if (!setup_video_stream(*impl_,
                                video_,
                                encoder_,
                                packet.video_codec,
                                impl_->video_nalus,
                                error)) {
            return false;
        }
        if (!setup_audio_stream(*impl_, audio_, error)) {
            return false;
        }

        // 配置 5 秒超时机制，防止由于远端端口不可达或防火墙拦截导致连接线程无限期阻塞
        impl_->cb_opaque.start_time = std::chrono::steady_clock::now();
        impl_->cb_opaque.timeout_ms = 5000;
        impl_->cb_opaque.interrupted = false;
        impl_->cb_opaque.disabled = false;

        impl_->format->interrupt_callback.callback = interrupt_callback;
        impl_->format->interrupt_callback.opaque = &impl_->cb_opaque;

        // 调用 avformat_write_header 会在底层开启 HTTP/SDP 信令交互和 DTLS 握手建连
        int ret = avformat_write_header(impl_->format, nullptr);

        // 交互完成后禁用中断回调，防止后续数据传输（由底层 URLContext 缓存的 callback 副本触发）被错误断开
        impl_->cb_opaque.disabled = true;
        impl_->format->interrupt_callback.callback = nullptr;
        impl_->format->interrupt_callback.opaque = nullptr;

        if (ret < 0) {
            if (impl_->cb_opaque.interrupted) {
                error = "Connection timed out during WHIP signaling/handshake (5000ms limit)";
            } else {
                error = ffmpeg_error("avformat_write_header(whip)", ret);
            }
            return false;
        }
        impl_->first_video_pts_us = packet.pts_us;
        impl_->has_first_video_pts = true;
        impl_->header_written = true;
        VC_LOG_INFO("webrtc", "WebRTC/WHIP native signaling and handshake completed successfully.");
    }

    AVPacket av_packet{};
    av_packet.data = const_cast<std::uint8_t*>(packet.data.data());
    av_packet.size = static_cast<int>(packet.data.size());
    av_packet.stream_index = impl_->video_stream->index;

    const std::uint64_t elapsed_us =
        packet.pts_us >= impl_->first_video_pts_us
            ? packet.pts_us - impl_->first_video_pts_us
            : 0;

    // FFmpeg AVPacket pts/dts 必须基于 stream->time_base
    av_packet.pts = av_packet.dts = av_rescale_q(elapsed_us, AVRational{1, 1000000}, impl_->video_stream->time_base);
    av_packet.duration = av_rescale_q(1000000 / std::max(1, video_.fps), AVRational{1, 1000000}, impl_->video_stream->time_base);

    const bool key_frame =
        packet.video_codec == VideoCodec::H265 ? is_h265_keyframe(impl_->video_nalus)
                                               : is_h264_keyframe(impl_->video_nalus);
    if (packet.key_frame || key_frame) {
        av_packet.flags |= AV_PKT_FLAG_KEY;
    }

    const int ret = av_interleaved_write_frame(impl_->format, &av_packet);
    if (ret < 0) {
        error = ffmpeg_error("av_interleaved_write_frame(video)", ret);
        return false;
    }
    return true;
#else
    (void)packet;
    error = "FFmpeg support is not enabled in this build";
    return false;
#endif
}

bool WebRtcPusher::push_audio(const EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || packet.data.empty()) {
        error = "WebRTC pusher is not connected or audio frame is empty";
        return false;
    }
    if (!impl_->header_written) {
        // 等待首帧视频包写入头信息后再发送音频
        return true;
    }

    AVPacket av_packet{};
    av_packet.data = const_cast<std::uint8_t*>(packet.data.data());
    av_packet.size = static_cast<int>(packet.data.size());
    av_packet.stream_index = impl_->audio_stream->index;

    const std::uint64_t elapsed_us =
        packet.pts_us >= impl_->first_video_pts_us
            ? packet.pts_us - impl_->first_video_pts_us
            : 0;

    av_packet.pts = av_packet.dts = av_rescale_q(elapsed_us, AVRational{1, 1000000}, impl_->audio_stream->time_base);

    const int ret = av_interleaved_write_frame(impl_->format, &av_packet);
    if (ret < 0) {
        error = ffmpeg_error("av_interleaved_write_frame(audio)", ret);
        return false;
    }
    return true;
#else
    (void)packet;
    error = "FFmpeg support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
