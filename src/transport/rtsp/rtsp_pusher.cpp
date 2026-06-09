/**
 * @file rtsp_pusher.cpp
 * @brief VisionCast RTSP 推流客户端模块实现文件
 *
 * 本文件使用项目内置 FFmpeg 8.x RTSP muxer 完成 RTSP ANNOUNCE/RECORD 发布。
 * 视频保持 MPP 输出的 Annex-B H.264/H.265 访问单元，音频复用项目现有 Opus 编码包。
 */

#include "transport/rtsp/rtsp_pusher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "common/log.h"

#if defined(VISIONCAST_ENABLE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}
#endif

namespace visioncast {

struct RtspNaluSpan {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct RtspPusher::Impl {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    AVFormatContext* format = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    std::vector<RtspNaluSpan> video_nalus;
    std::vector<std::uint8_t> video_extradata;
    std::uint64_t first_video_pts_us = 0;
    bool header_written = false;
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
                std::vector<RtspNaluSpan>& nalus,
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
            nalus.push_back(RtspNaluSpan{data + start, pos - start});
        }
    }
    if (nalus.empty()) {
        error = "video frame does not contain Annex-B start-code NAL units";
        return false;
    }
    return true;
}

std::uint8_t h264_nalu_type(const RtspNaluSpan& nalu) {
    return nalu.size > 0 ? static_cast<std::uint8_t>(nalu.data[0] & 0x1FU) : 0;
}

std::uint8_t h265_nalu_type(const RtspNaluSpan& nalu) {
    return nalu.size >= 2
               ? static_cast<std::uint8_t>((nalu.data[0] >> 1U) & 0x3FU)
               : 0;
}

bool is_h264_keyframe(const std::vector<RtspNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        return h264_nalu_type(nalu) == 5;
    });
}

bool is_h265_keyframe(const std::vector<RtspNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        const std::uint8_t type = h265_nalu_type(nalu);
        return type >= 16 && type <= 21;
    });
}

void append_annexb_nalu(std::vector<std::uint8_t>& out, const RtspNaluSpan& nalu) {
    static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
    out.insert(out.end(), nalu.data, nalu.data + nalu.size);
}

bool build_annexb_extradata(VideoCodec codec,
                            const std::vector<RtspNaluSpan>& nalus,
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
                    ? "first RTSP H265 frame does not contain valid VPS/SPS/PPS"
                    : "first RTSP H264 frame does not contain valid SPS/PPS";
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
        error = "failed to allocate RTSP video extradata";
        return false;
    }
    std::memcpy(parameters->extradata, extradata.data(), extradata.size());
    parameters->extradata_size = static_cast<int>(extradata.size());
    return true;
}

bool setup_video_stream(RtspPusher::Impl& impl,
                        const VideoConfig& video,
                        const EncoderConfig& encoder,
                        VideoCodec codec,
                        const std::vector<RtspNaluSpan>& nalus,
                        std::string& error) {
    impl.video_stream = avformat_new_stream(impl.format, nullptr);
    if (impl.video_stream == nullptr) {
        error = "avformat_new_stream(video) failed";
        return false;
    }

    impl.video_stream->time_base = AVRational{1, 1000000};
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

bool setup_audio_stream(RtspPusher::Impl& impl,
                        const AudioConfig& audio,
                        std::string& error) {
    impl.audio_stream = avformat_new_stream(impl.format, nullptr);
    if (impl.audio_stream == nullptr) {
        error = "avformat_new_stream(audio) failed";
        return false;
    }

    impl.audio_stream->time_base = AVRational{1, 1000000};
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

RtspPusher::RtspPusher(std::string rtsp_url,
                       std::string rtsp_transport,
                       VideoConfig video,
                       AudioConfig audio,
                       EncoderConfig encoder)
    : impl_(std::make_unique<Impl>()),
      rtsp_url_(std::move(rtsp_url)),
      rtsp_transport_(std::move(rtsp_transport)),
      video_(std::move(video)),
      audio_(std::move(audio)),
      encoder_(std::move(encoder)) {}

RtspPusher::~RtspPusher() {
    disconnect();
}

bool RtspPusher::connect(std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->connected) {
        return true;
    }
    if (rtsp_url_.rfind("rtsp://", 0) != 0 && rtsp_url_.rfind("rtsps://", 0) != 0) {
        error = "invalid RTSP URL: " + rtsp_url_;
        return false;
    }

    int ret = avformat_alloc_output_context2(
        &impl_->format, nullptr, "rtsp", rtsp_url_.c_str());
    if (ret < 0 || impl_->format == nullptr) {
        error = ret < 0 ? ffmpeg_error("avformat_alloc_output_context2(rtsp)", ret)
                        : "avformat_alloc_output_context2(rtsp) returned null";
        disconnect();
        return false;
    }

    ret = av_opt_set(impl_->format->priv_data, "rtsp_transport", rtsp_transport_.c_str(), 0);
    if (ret < 0) {
        VC_LOG_WARN("rtsp", "Failed to set rtsp_transport=" + rtsp_transport_ + ": " +
                                ffmpeg_error("av_opt_set", ret));
    }

    impl_->connected = true;
    VC_LOG_INFO("rtsp", "RTSP format context allocated for " + rtsp_url_ +
                            " transport=" + rtsp_transport_);
    return true;
#else
    error = "RTSP support is not enabled in this build";
    return false;
#endif
}

void RtspPusher::disconnect() {
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
    impl_->header_written = false;
#endif
    impl_->connected = false;
}

bool RtspPusher::push_video(const EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || packet.data.empty()) {
        error = "RTSP pusher is not connected or video frame is empty";
        return false;
    }
    if (packet.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "RTSP video packet is too large";
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

        const int ret = avformat_write_header(impl_->format, nullptr);
        if (ret < 0) {
            error = ffmpeg_error("avformat_write_header(rtsp)", ret);
            return false;
        }
        impl_->first_video_pts_us = packet.pts_us;
        impl_->header_written = true;
        VC_LOG_INFO("rtsp", "RTSP session announced and recording started.");
    }

    AVPacket av_packet{};
    av_packet.data = const_cast<std::uint8_t*>(packet.data.data());
    av_packet.size = static_cast<int>(packet.data.size());
    av_packet.stream_index = impl_->video_stream->index;

    const std::uint64_t elapsed_us =
        packet.pts_us >= impl_->first_video_pts_us
            ? packet.pts_us - impl_->first_video_pts_us
            : 0;
    av_packet.pts = av_packet.dts =
        av_rescale_q(elapsed_us, AVRational{1, 1000000}, impl_->video_stream->time_base);
    av_packet.duration = av_rescale_q(1000000 / std::max(1, video_.fps),
                                      AVRational{1, 1000000},
                                      impl_->video_stream->time_base);

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
    error = "RTSP support is not enabled in this build";
    return false;
#endif
}

bool RtspPusher::push_audio(const EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || packet.data.empty()) {
        error = "RTSP pusher is not connected or audio frame is empty";
        return false;
    }
    if (packet.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "RTSP audio packet is too large";
        return false;
    }
    if (!impl_->header_written) {
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
    av_packet.pts = av_packet.dts =
        av_rescale_q(elapsed_us, AVRational{1, 1000000}, impl_->audio_stream->time_base);

    const int ret = av_interleaved_write_frame(impl_->format, &av_packet);
    if (ret < 0) {
        error = ffmpeg_error("av_interleaved_write_frame(audio)", ret);
        return false;
    }
    return true;
#else
    (void)packet;
    error = "RTSP support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
