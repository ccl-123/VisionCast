/**
 * @file rtmp_pusher.cpp
 * @brief VisionCast RTMP 推流客户端模块实现文件
 *
 * 本文件使用项目内置 FFmpeg 8.x 共享库完成 RTMP/FLV 封装。视频编码包保持 MPP
 * 输出的 Annex-B H.264/H.265 码流，由 FFmpeg flv muxer 写出传统 AVC 或 Enhanced
 * RTMP/HEVC tag；音频路径将原始 PCM 重采样编码为 AAC 后写入同一个 RTMP 会话。
 */

#include "transport/rtmp/rtmp_pusher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#if defined(VISIONCAST_ENABLE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace visioncast {

struct RtmpNaluSpan {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

struct RtmpPusher::Impl {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    AVFormatContext* format = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    AVCodecContext* audio_encoder = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* audio_frame = nullptr;
    std::vector<std::int16_t> pcm_samples;
    std::size_t pcm_sample_offset = 0;
    std::vector<RtmpNaluSpan> video_nalus;
    std::vector<std::uint8_t> video_extradata;
    std::int64_t audio_samples_written = 0;
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
                std::vector<RtmpNaluSpan>& nalus,
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
            nalus.push_back(RtmpNaluSpan{data + start, pos - start});
        }
    }
    if (nalus.empty()) {
        error = "video frame does not contain Annex-B start-code NAL units";
        return false;
    }
    return true;
}

std::uint8_t h264_nalu_type(const RtmpNaluSpan& nalu) {
    return nalu.size > 0 ? static_cast<std::uint8_t>(nalu.data[0] & 0x1FU) : 0;
}

std::uint8_t h265_nalu_type(const RtmpNaluSpan& nalu) {
    return nalu.size >= 2
               ? static_cast<std::uint8_t>((nalu.data[0] >> 1U) & 0x3FU)
               : 0;
}

bool is_h264_keyframe(const std::vector<RtmpNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        return h264_nalu_type(nalu) == 5;
    });
}

bool is_h265_keyframe(const std::vector<RtmpNaluSpan>& nalus) {
    return std::any_of(nalus.begin(), nalus.end(), [](const auto& nalu) {
        const std::uint8_t type = h265_nalu_type(nalu);
        return type >= 16 && type <= 21;
    });
}

void append_annexb_nalu(std::vector<std::uint8_t>& out, const RtmpNaluSpan& nalu) {
    static constexpr std::uint8_t kStartCode[] = {0, 0, 0, 1};
    out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
    out.insert(out.end(), nalu.data, nalu.data + nalu.size);
}

bool build_annexb_extradata(VideoCodec codec,
                            const std::vector<RtmpNaluSpan>& nalus,
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
                    ? "first RTMP H265 frame does not contain valid VPS/SPS/PPS"
                    : "first RTMP H264 frame does not contain valid SPS/PPS";
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
        error = "failed to allocate RTMP video extradata";
        return false;
    }
    std::memcpy(parameters->extradata, extradata.data(), extradata.size());
    parameters->extradata_size = static_cast<int>(extradata.size());
    return true;
}

bool setup_video_stream(RtmpPusher::Impl& impl,
                        const VideoConfig& video,
                        const EncoderConfig& encoder,
                        VideoCodec codec,
                        const std::vector<RtmpNaluSpan>& nalus,
                        std::string& error) {
    impl.video_stream = avformat_new_stream(impl.format, nullptr);
    if (impl.video_stream == nullptr) {
        error = "avformat_new_stream(video) failed";
        return false;
    }

    impl.video_stream->time_base = AVRational{1, 1000};
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

bool write_encoded_audio(RtmpPusher::Impl& impl, std::string& error) {
    AVPacket packet{};
    for (;;) {
        const int ret = avcodec_receive_packet(impl.audio_encoder, &packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            error = ffmpeg_error("avcodec_receive_packet(AAC)", ret);
            return false;
        }
        av_packet_rescale_ts(
            &packet, impl.audio_encoder->time_base, impl.audio_stream->time_base);
        packet.stream_index = impl.audio_stream->index;
        const int write_ret = av_interleaved_write_frame(impl.format, &packet);
        av_packet_unref(&packet);
        if (write_ret < 0) {
            error = ffmpeg_error("av_interleaved_write_frame(AAC)", write_ret);
            return false;
        }
    }
}

bool encode_pending_audio(RtmpPusher::Impl& impl,
                          int channels,
                          std::string& error) {
    const int frame_samples = impl.audio_encoder->frame_size;
    const auto frame_values = static_cast<std::size_t>(frame_samples) *
                              static_cast<std::size_t>(channels);
    while (impl.pcm_samples.size() >= impl.pcm_sample_offset + frame_values) {
        int ret = av_frame_make_writable(impl.audio_frame);
        if (ret < 0) {
            error = ffmpeg_error("av_frame_make_writable(AAC)", ret);
            return false;
        }
        const std::uint8_t* input_data =
            reinterpret_cast<const std::uint8_t*>(impl.pcm_samples.data() +
                                                  impl.pcm_sample_offset);
        ret = swr_convert(impl.resampler,
                          impl.audio_frame->data,
                          frame_samples,
                          &input_data,
                          frame_samples);
        if (ret < 0) {
            error = ffmpeg_error("swr_convert(AAC)", ret);
            return false;
        }
        impl.audio_frame->pts = impl.audio_samples_written;
        impl.audio_samples_written += ret;
        impl.pcm_sample_offset += frame_values;

        ret = avcodec_send_frame(impl.audio_encoder, impl.audio_frame);
        if (ret < 0) {
            error = ffmpeg_error("avcodec_send_frame(AAC)", ret);
            return false;
        }
        if (!write_encoded_audio(impl, error)) {
            return false;
        }
    }
    if (impl.pcm_sample_offset > 0 &&
        (impl.pcm_sample_offset == impl.pcm_samples.size() ||
         impl.pcm_sample_offset >= impl.pcm_samples.size() / 2U)) {
        impl.pcm_samples.erase(impl.pcm_samples.begin(),
                               impl.pcm_samples.begin() +
                                   static_cast<std::ptrdiff_t>(impl.pcm_sample_offset));
        impl.pcm_sample_offset = 0;
    }
    return true;
}
#endif

}  // namespace

RtmpPusher::RtmpPusher(std::string rtmp_url,
                       VideoConfig video,
                       AudioConfig audio,
                       EncoderConfig encoder)
    : impl_(std::make_unique<Impl>()),
      rtmp_url_(std::move(rtmp_url)),
      video_(std::move(video)),
      audio_(std::move(audio)),
      encoder_(std::move(encoder)) {}

RtmpPusher::~RtmpPusher() {
    disconnect();
}

bool RtmpPusher::connect(std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->connected) {
        return true;
    }
    if (rtmp_url_.rfind("rtmp://", 0) != 0) {
        error = "invalid RTMP URL: " + rtmp_url_;
        return false;
    }

    int ret = avformat_alloc_output_context2(
        &impl_->format, nullptr, "flv", rtmp_url_.c_str());
    if (ret < 0 || impl_->format == nullptr) {
        error = ret < 0 ? ffmpeg_error("avformat_alloc_output_context2", ret)
                        : "avformat_alloc_output_context2 returned null";
        disconnect();
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        error = "FFmpeg AAC encoder is unavailable";
        disconnect();
        return false;
    }
    impl_->audio_encoder = avcodec_alloc_context3(codec);
    if (impl_->audio_encoder == nullptr) {
        error = "avcodec_alloc_context3(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_encoder->sample_rate = audio_.sample_rate;
    av_channel_layout_default(&impl_->audio_encoder->ch_layout, audio_.channels);
    impl_->audio_encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    impl_->audio_encoder->bit_rate = 128000;
    impl_->audio_encoder->time_base = AVRational{1, audio_.sample_rate};
    impl_->audio_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = avcodec_open2(impl_->audio_encoder, codec, nullptr);
    if (ret < 0) {
        error = ffmpeg_error("avcodec_open2(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->audio_stream = avformat_new_stream(impl_->format, nullptr);
    if (impl_->audio_stream == nullptr) {
        error = "avformat_new_stream(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_stream->time_base = impl_->audio_encoder->time_base;
    ret = avcodec_parameters_from_context(
        impl_->audio_stream->codecpar, impl_->audio_encoder);
    if (ret < 0) {
        error = ffmpeg_error("avcodec_parameters_from_context(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->audio_frame = av_frame_alloc();
    if (impl_->audio_frame == nullptr) {
        error = "av_frame_alloc(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_frame->nb_samples = impl_->audio_encoder->frame_size;
    impl_->audio_frame->format = impl_->audio_encoder->sample_fmt;
    ret = av_channel_layout_copy(&impl_->audio_frame->ch_layout,
                                 &impl_->audio_encoder->ch_layout);
    if (ret < 0) {
        error = ffmpeg_error("av_channel_layout_copy(AAC)", ret);
        disconnect();
        return false;
    }
    impl_->audio_frame->sample_rate = impl_->audio_encoder->sample_rate;
    ret = av_frame_get_buffer(impl_->audio_frame, 0);
    if (ret < 0) {
        error = ffmpeg_error("av_frame_get_buffer(AAC)", ret);
        disconnect();
        return false;
    }

    ret = swr_alloc_set_opts2(&impl_->resampler,
                              &impl_->audio_encoder->ch_layout,
                              impl_->audio_encoder->sample_fmt,
                              impl_->audio_encoder->sample_rate,
                              &impl_->audio_encoder->ch_layout,
                              AV_SAMPLE_FMT_S16,
                              audio_.sample_rate,
                              0,
                              nullptr);
    if (ret < 0 || impl_->resampler == nullptr) {
        error = ret < 0 ? ffmpeg_error("swr_alloc_set_opts2(AAC)", ret)
                        : "swr_alloc_set_opts2(AAC) returned null";
        disconnect();
        return false;
    }
    ret = swr_init(impl_->resampler);
    if (ret < 0) {
        error = ffmpeg_error("swr_init(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->connected = true;
    return true;
#else
    error = "RTMP support is not enabled in this build";
    return false;
#endif
}

void RtmpPusher::disconnect() {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->audio_encoder != nullptr && impl_->header_written) {
        std::string ignored;
        avcodec_send_frame(impl_->audio_encoder, nullptr);
        write_encoded_audio(*impl_, ignored);
        av_write_trailer(impl_->format);
    }
    if (impl_->format != nullptr && impl_->format->pb != nullptr) {
        avio_closep(&impl_->format->pb);
    }
    if (impl_->audio_frame != nullptr) {
        av_frame_free(&impl_->audio_frame);
    }
    if (impl_->resampler != nullptr) {
        swr_free(&impl_->resampler);
    }
    if (impl_->audio_encoder != nullptr) {
        avcodec_free_context(&impl_->audio_encoder);
    }
    if (impl_->format != nullptr) {
        avformat_free_context(impl_->format);
        impl_->format = nullptr;
    }
    impl_->video_stream = nullptr;
    impl_->audio_stream = nullptr;
    impl_->pcm_samples.clear();
    impl_->pcm_sample_offset = 0;
    impl_->video_nalus.clear();
    impl_->video_extradata.clear();
    impl_->audio_samples_written = 0;
    impl_->first_video_pts_us = 0;
    impl_->header_written = false;
#endif
    impl_->connected = false;
}

bool RtmpPusher::push_video(const EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || packet.data.empty()) {
        error = "RTMP pusher is not connected or video frame is empty";
        return false;
    }
    if (packet.data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "RTMP video packet is too large";
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
        int ret = avio_open(&impl_->format->pb, rtmp_url_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            error = ffmpeg_error("avio_open(RTMP)", ret);
            return false;
        }
        ret = avformat_write_header(impl_->format, nullptr);
        if (ret < 0) {
            error = ffmpeg_error("avformat_write_header(FLV)", ret);
            return false;
        }
        impl_->first_video_pts_us = packet.pts_us;
        impl_->header_written = true;
    }

    AVPacket av_packet{};
    av_packet.data = const_cast<std::uint8_t*>(packet.data.data());
    av_packet.size = static_cast<int>(packet.data.size());
    av_packet.stream_index = impl_->video_stream->index;
    const std::uint64_t elapsed_us =
        packet.pts_us >= impl_->first_video_pts_us
            ? packet.pts_us - impl_->first_video_pts_us
            : 0;
    av_packet.pts = av_packet.dts = static_cast<std::int64_t>(elapsed_us / 1000U);
    av_packet.duration = std::max(1, 1000 / std::max(1, video_.fps));
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
    return encode_pending_audio(*impl_, audio_.channels, error);
#else
    (void)packet;
    error = "RTMP support is not enabled in this build";
    return false;
#endif
}

bool RtmpPusher::push_audio(const std::uint8_t* data,
                            std::size_t size,
                            std::uint64_t pts_us,
                            std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    (void)pts_us;
    if (!impl_->connected || data == nullptr || size < sizeof(std::int16_t)) {
        error = "RTMP pusher is not connected or PCM frame is empty";
        return false;
    }
    const std::size_t sample_count = size / sizeof(std::int16_t);
    const auto* samples = reinterpret_cast<const std::int16_t*>(data);
    impl_->pcm_samples.insert(
        impl_->pcm_samples.end(), samples, samples + sample_count);
    if (!impl_->header_written) {
        return true;
    }
    return encode_pending_audio(*impl_, audio_.channels, error);
#else
    (void)data;
    (void)size;
    (void)pts_us;
    error = "RTMP support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
