/**
 * @file rtmp_pusher.cpp
 * @brief VisionCast RTMP 推流客户端模块实现文件
 * 
 * 本文件利用 FFmpeg 动态库加载技术（通过 dlopen/dlsym 加载 libavformat/libavcodec/libswresample/libavutil），
 * 实现了将 H.264 视频流（Annex-B 格式）转换为 RTMP 兼容的 AVCC 格式，
 * 并将原始 PCM 音频重采样编码为 AAC 格式后推送到 RTMP 服务器。
 */

#include "transport/rtmp_pusher.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
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
#include <libavutil/opt.h>
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
    std::vector<std::uint8_t> video_payload;
    std::int64_t audio_samples_written = 0;
    std::uint64_t first_video_pts_us = 0;
    bool header_written = false;
#endif
    bool connected = false;
};

namespace {

#if defined(VISIONCAST_ENABLE_FFMPEG)
struct FfmpegApi {
    void* avutil = nullptr;
    void* swresample = nullptr;
    void* avcodec = nullptr;
    void* avformat = nullptr;
    bool loaded = false;

    decltype(&::av_strerror) av_strerror = nullptr;
    decltype(&::av_mallocz) av_mallocz = nullptr;
    decltype(&::av_packet_rescale_ts) av_packet_rescale_ts = nullptr;
    decltype(&::av_packet_unref) av_packet_unref = nullptr;
    decltype(&::av_frame_make_writable) av_frame_make_writable = nullptr;
    decltype(&::av_frame_alloc) av_frame_alloc = nullptr;
    decltype(&::av_frame_get_buffer) av_frame_get_buffer = nullptr;
    decltype(&::av_frame_free) av_frame_free = nullptr;
    decltype(&::avcodec_receive_packet) avcodec_receive_packet = nullptr;
    decltype(&::avcodec_send_frame) avcodec_send_frame = nullptr;
    decltype(&::avcodec_find_encoder) avcodec_find_encoder = nullptr;
    decltype(&::avcodec_alloc_context3) avcodec_alloc_context3 = nullptr;
    decltype(&::avcodec_open2) avcodec_open2 = nullptr;
    decltype(&::avcodec_parameters_from_context) avcodec_parameters_from_context = nullptr;
    decltype(&::avcodec_free_context) avcodec_free_context = nullptr;
    decltype(&::avformat_alloc_output_context2) avformat_alloc_output_context2 = nullptr;
    decltype(&::avformat_new_stream) avformat_new_stream = nullptr;
    decltype(&::avformat_write_header) avformat_write_header = nullptr;
    decltype(&::avformat_free_context) avformat_free_context = nullptr;
    decltype(&::av_interleaved_write_frame) av_interleaved_write_frame = nullptr;
    decltype(&::av_write_trailer) av_write_trailer = nullptr;
    decltype(&::avio_open) avio_open = nullptr;
    decltype(&::avio_closep) avio_closep = nullptr;
    decltype(&::swr_alloc_set_opts) swr_alloc_set_opts = nullptr;
    decltype(&::swr_init) swr_init = nullptr;
    decltype(&::swr_convert) swr_convert = nullptr;
    decltype(&::swr_free) swr_free = nullptr;

    ~FfmpegApi() {
        close_handles();
    }

    void close_handles() {
        if (avformat != nullptr) dlclose(avformat);
        if (avcodec != nullptr) dlclose(avcodec);
        if (swresample != nullptr) dlclose(swresample);
        if (avutil != nullptr) dlclose(avutil);
        avformat = nullptr;
        avcodec = nullptr;
        swresample = nullptr;
        avutil = nullptr;
        loaded = false;
    }

    template <typename Fn>
    bool symbol(Fn& function, const char* name, std::string& error) {
        dlerror();
        function = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
        const char* symbol_error = dlerror();
        if (function == nullptr || symbol_error != nullptr) {
            error = std::string("FFmpeg symbol ") + name + ": " +
                    (symbol_error != nullptr ? symbol_error : "not found");
            return false;
        }
        return true;
    }

    bool load(std::string& error) {
        if (loaded) {
            return true;
        }
        close_handles();
        // 动态加载并定位 FFmpeg 所需动态库 (SO) 以提供 RTMP 封装和 AAC 编码功能
        avutil = dlopen("libavutil.so.56", RTLD_NOW | RTLD_GLOBAL);
        if (avutil != nullptr) {
            swresample = dlopen("libswresample.so.3", RTLD_NOW | RTLD_GLOBAL);
        }
        if (swresample != nullptr) {
            avcodec = dlopen("libavcodec.so.58", RTLD_NOW | RTLD_GLOBAL);
        }
        if (avcodec != nullptr) {
            avformat = dlopen("libavformat.so.58", RTLD_NOW | RTLD_GLOBAL);
        }
        if (avformat == nullptr) {
            const char* load_error = dlerror();
            error = std::string("load FFmpeg RTMP backend: ") +
                    (load_error != nullptr ? load_error : "unknown error");
            close_handles();
            return false;
        }

        loaded =
            symbol(av_strerror, "av_strerror", error) &&
            symbol(av_mallocz, "av_mallocz", error) &&
            symbol(av_packet_rescale_ts, "av_packet_rescale_ts", error) &&
            symbol(av_packet_unref, "av_packet_unref", error) &&
            symbol(av_frame_make_writable, "av_frame_make_writable", error) &&
            symbol(av_frame_alloc, "av_frame_alloc", error) &&
            symbol(av_frame_get_buffer, "av_frame_get_buffer", error) &&
            symbol(av_frame_free, "av_frame_free", error) &&
            symbol(avcodec_receive_packet, "avcodec_receive_packet", error) &&
            symbol(avcodec_send_frame, "avcodec_send_frame", error) &&
            symbol(avcodec_find_encoder, "avcodec_find_encoder", error) &&
            symbol(avcodec_alloc_context3, "avcodec_alloc_context3", error) &&
            symbol(avcodec_open2, "avcodec_open2", error) &&
            symbol(avcodec_parameters_from_context,
                   "avcodec_parameters_from_context",
                   error) &&
            symbol(avcodec_free_context, "avcodec_free_context", error) &&
            symbol(avformat_alloc_output_context2,
                   "avformat_alloc_output_context2",
                   error) &&
            symbol(avformat_new_stream, "avformat_new_stream", error) &&
            symbol(avformat_write_header, "avformat_write_header", error) &&
            symbol(avformat_free_context, "avformat_free_context", error) &&
            symbol(av_interleaved_write_frame,
                   "av_interleaved_write_frame",
                   error) &&
            symbol(av_write_trailer, "av_write_trailer", error) &&
            symbol(avio_open, "avio_open", error) &&
            symbol(avio_closep, "avio_closep", error) &&
            symbol(swr_alloc_set_opts, "swr_alloc_set_opts", error) &&
            symbol(swr_init, "swr_init", error) &&
            symbol(swr_convert, "swr_convert", error) &&
            symbol(swr_free, "swr_free", error);
        if (!loaded) {
            close_handles();
        }
        return loaded;
    }
};

FfmpegApi& ffmpeg() {
    static FfmpegApi api;
    return api;
}

std::string ffmpeg_error(const char* call, int ret) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    ffmpeg().av_strerror(ret, text, sizeof(text));
    return std::string(call) + ": " + text;
}

// 检测 Annex-B 起始码的长度（3字节的 0x000001 或 4字节的 0x00000001）
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

// 解析 H.264 Annex-B 码流，记录 NALU 视图，避免为每个 NALU 生成临时 vector。
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
        error = "H264 frame does not contain Annex-B start-code NAL units";
        return false;
    }
    return true;
}

// 从首个视频帧中提取 SPS 和 PPS，构建 FLV/RTMP 推流所需的 AVCC Extradata (avcC 头部配置包)
bool set_avc_extradata(AVCodecParameters* parameters,
                       const std::vector<RtmpNaluSpan>& nalus,
                       std::string& error) {
    RtmpNaluSpan sps;
    RtmpNaluSpan pps;
    for (const auto& nalu : nalus) {
        if (nalu.data == nullptr || nalu.size == 0) {
            continue;
        }
        const std::uint8_t type = nalu.data[0] & 0x1FU;
        if (type == 7 && sps.data == nullptr) {
            sps = nalu;
        } else if (type == 8 && pps.data == nullptr) {
            pps = nalu;
        }
    }
    if (sps.size < 4 || pps.size == 0 || sps.size > 65535U ||
        pps.size > 65535U) {
        error = "first RTMP H264 frame does not contain valid SPS/PPS";
        return false;
    }

    std::vector<std::uint8_t> avcc;
    avcc.reserve(11U + sps.size + pps.size);
    avcc.push_back(1);
    avcc.push_back(sps.data[1]);
    avcc.push_back(sps.data[2]);
    avcc.push_back(sps.data[3]);
    avcc.push_back(0xFF);
    avcc.push_back(0xE1);
    avcc.push_back(static_cast<std::uint8_t>(sps.size >> 8));
    avcc.push_back(static_cast<std::uint8_t>(sps.size));
    avcc.insert(avcc.end(), sps.data, sps.data + sps.size);
    avcc.push_back(1);
    avcc.push_back(static_cast<std::uint8_t>(pps.size >> 8));
    avcc.push_back(static_cast<std::uint8_t>(pps.size));
    avcc.insert(avcc.end(), pps.data, pps.data + pps.size);

    parameters->extradata = static_cast<std::uint8_t*>(
        ffmpeg().av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (parameters->extradata == nullptr) {
        error = "failed to allocate H264 AVC configuration";
        return false;
    }
    std::memcpy(parameters->extradata, avcc.data(), avcc.size());
    parameters->extradata_size = static_cast<int>(avcc.size());
    return true;
}

// 将 H.264 Annex-B 编码包（带起始码）转换为 FLV/RTMP 要求的 AVCC 格式数据负载（4字节大端长度前缀 + NALU 数据）
bool annexb_to_avcc_payload(const std::vector<RtmpNaluSpan>& nalus,
                            std::size_t source_size,
                            std::vector<std::uint8_t>& payload,
                            std::string& error) {
    if (nalus.empty()) {
        error = "H264 frame does not contain Annex-B start-code NAL units";
        return false;
    }

    payload.clear();
    payload.reserve(source_size + nalus.size());
    for (const auto& nalu : nalus) {
        if (nalu.data == nullptr || nalu.size == 0) {
            continue;
        }
        if (nalu.size > 0xFFFFFFFFULL) {
            error = "H264 NAL unit is too large for AVC length prefix";
            return false;
        }
        const auto nalu_size = static_cast<std::uint32_t>(nalu.size);
        // FLV/RTMP stores AVC samples as 4-byte big-endian length + NAL,
        // while MPP emits Annex-B start-code framed H.264.
        payload.push_back(static_cast<std::uint8_t>((nalu_size >> 24) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((nalu_size >> 16) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>((nalu_size >> 8) & 0xFFU));
        payload.push_back(static_cast<std::uint8_t>(nalu_size & 0xFFU));
        payload.insert(payload.end(), nalu.data, nalu.data + nalu.size);
    }
    if (payload.empty()) {
        error = "H264 frame did not contain non-empty NAL units";
        return false;
    }
    return true;
}

// 从 AAC 编码器中循环获取已编码的包，并写入 RTMP 输出容器中
bool write_encoded_audio(RtmpPusher::Impl& impl, std::string& error) {
    AVPacket packet{};
    for (;;) {
        const int ret = ffmpeg().avcodec_receive_packet(impl.audio_encoder, &packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return true;
        }
        if (ret < 0) {
            error = ffmpeg_error("avcodec_receive_packet(AAC)", ret);
            return false;
        }
        ffmpeg().av_packet_rescale_ts(
            &packet, impl.audio_encoder->time_base, impl.audio_stream->time_base);
        packet.stream_index = impl.audio_stream->index;
        const int write_ret =
            ffmpeg().av_interleaved_write_frame(impl.format, &packet);
        ffmpeg().av_packet_unref(&packet);
        if (write_ret < 0) {
            error = ffmpeg_error("av_interleaved_write_frame(AAC)", write_ret);
            return false;
        }
    }
}

// 将缓存的 PCM 音频样本进行重采样（SwrContext），送入 AAC 编码器中进行编码并输出
bool encode_pending_audio(RtmpPusher::Impl& impl, std::string& error) {
    const int frame_samples = impl.audio_encoder->frame_size;
    while (impl.pcm_samples.size() >=
           impl.pcm_sample_offset + static_cast<std::size_t>(frame_samples)) {
        int ret = ffmpeg().av_frame_make_writable(impl.audio_frame);
        if (ret < 0) {
            error = ffmpeg_error("av_frame_make_writable(AAC)", ret);
            return false;
        }
        const std::uint8_t* input_data =
            reinterpret_cast<const std::uint8_t*>(impl.pcm_samples.data() +
                                                  impl.pcm_sample_offset);
        ret = ffmpeg().swr_convert(impl.resampler,
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
        impl.pcm_sample_offset += static_cast<std::size_t>(frame_samples);

        ret = ffmpeg().avcodec_send_frame(impl.audio_encoder, impl.audio_frame);
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
                       AudioConfig audio)
    : impl_(std::make_unique<Impl>()),
      rtmp_url_(std::move(rtmp_url)),
      video_(std::move(video)),
      audio_(std::move(audio)) {}

RtmpPusher::~RtmpPusher() {
    disconnect();
}

bool RtmpPusher::connect(std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (impl_->connected) {
        return true;
    }
    // 确保 URL 格式符合 RTMP 规范
    if (rtmp_url_.rfind("rtmp://", 0) != 0) {
        error = "invalid RTMP URL: " + rtmp_url_;
        return false;
    }
    // 动态加载 FFmpeg 库
    if (!ffmpeg().load(error)) {
        return false;
    }

    // 分配 FLV 封装上下文，用于向指定 RTMP 地址推送音视频流
    int ret = ffmpeg().avformat_alloc_output_context2(
        &impl_->format, nullptr, "flv", rtmp_url_.c_str());
    if (ret < 0 || impl_->format == nullptr) {
        error = ret < 0 ? ffmpeg_error("avformat_alloc_output_context2", ret)
                        : "avformat_alloc_output_context2 returned null";
        disconnect();
        return false;
    }

    impl_->video_stream = ffmpeg().avformat_new_stream(impl_->format, nullptr);
    if (impl_->video_stream == nullptr) {
        error = "avformat_new_stream(H264) failed";
        disconnect();
        return false;
    }
    impl_->video_stream->time_base = AVRational{1, 1000};
    AVCodecParameters* video_parameters = impl_->video_stream->codecpar;
    video_parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    video_parameters->codec_id = AV_CODEC_ID_H264;
    video_parameters->width = video_.width;
    video_parameters->height = video_.height;
    video_parameters->bit_rate = 4000000;

    const AVCodec* codec = ffmpeg().avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (codec == nullptr) {
        error = "FFmpeg AAC encoder is unavailable";
        disconnect();
        return false;
    }
    impl_->audio_encoder = ffmpeg().avcodec_alloc_context3(codec);
    if (impl_->audio_encoder == nullptr) {
        error = "avcodec_alloc_context3(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_encoder->sample_rate = audio_.sample_rate;
    impl_->audio_encoder->channel_layout =
        audio_.channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    impl_->audio_encoder->channels = audio_.channels;
    impl_->audio_encoder->sample_fmt =
        codec->sample_fmts != nullptr ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    impl_->audio_encoder->bit_rate = 128000;
    impl_->audio_encoder->time_base = AVRational{1, audio_.sample_rate};
    impl_->audio_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ret = ffmpeg().avcodec_open2(impl_->audio_encoder, codec, nullptr);
    if (ret < 0) {
        error = ffmpeg_error("avcodec_open2(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->audio_stream = ffmpeg().avformat_new_stream(impl_->format, nullptr);
    if (impl_->audio_stream == nullptr) {
        error = "avformat_new_stream(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_stream->time_base = impl_->audio_encoder->time_base;
    ret = ffmpeg().avcodec_parameters_from_context(
        impl_->audio_stream->codecpar, impl_->audio_encoder);
    if (ret < 0) {
        error = ffmpeg_error("avcodec_parameters_from_context(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->audio_frame = ffmpeg().av_frame_alloc();
    if (impl_->audio_frame == nullptr) {
        error = "av_frame_alloc(AAC) failed";
        disconnect();
        return false;
    }
    impl_->audio_frame->nb_samples = impl_->audio_encoder->frame_size;
    impl_->audio_frame->format = impl_->audio_encoder->sample_fmt;
    impl_->audio_frame->channel_layout = impl_->audio_encoder->channel_layout;
    impl_->audio_frame->sample_rate = impl_->audio_encoder->sample_rate;
    ret = ffmpeg().av_frame_get_buffer(impl_->audio_frame, 0);
    if (ret < 0) {
        error = ffmpeg_error("av_frame_get_buffer(AAC)", ret);
        disconnect();
        return false;
    }

    impl_->resampler = ffmpeg().swr_alloc_set_opts(
        nullptr,
        static_cast<std::int64_t>(impl_->audio_encoder->channel_layout),
        impl_->audio_encoder->sample_fmt,
        impl_->audio_encoder->sample_rate,
        static_cast<std::int64_t>(impl_->audio_encoder->channel_layout),
        AV_SAMPLE_FMT_S16,
        audio_.sample_rate,
        0,
        nullptr);
    if (impl_->resampler == nullptr || ffmpeg().swr_init(impl_->resampler) < 0) {
        error = "failed to initialize AAC resampler";
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
        ffmpeg().avcodec_send_frame(impl_->audio_encoder, nullptr);
        write_encoded_audio(*impl_, ignored);
        ffmpeg().av_write_trailer(impl_->format);
    }
    if (impl_->format != nullptr && impl_->format->pb != nullptr) {
        ffmpeg().avio_closep(&impl_->format->pb);
    }
    if (impl_->audio_frame != nullptr) {
        ffmpeg().av_frame_free(&impl_->audio_frame);
    }
    if (impl_->resampler != nullptr) {
        ffmpeg().swr_free(&impl_->resampler);
    }
    if (impl_->audio_encoder != nullptr) {
        ffmpeg().avcodec_free_context(&impl_->audio_encoder);
    }
    if (impl_->format != nullptr) {
        ffmpeg().avformat_free_context(impl_->format);
        impl_->format = nullptr;
    }
    impl_->video_stream = nullptr;
    impl_->audio_stream = nullptr;
    impl_->pcm_samples.clear();
    impl_->pcm_sample_offset = 0;
    impl_->video_nalus.clear();
    impl_->video_payload.clear();
    impl_->audio_samples_written = 0;
    impl_->first_video_pts_us = 0;
    impl_->header_written = false;
#endif
    impl_->connected = false;
}

bool RtmpPusher::push_video(const std::uint8_t* data,
                            std::size_t size,
                            std::uint64_t pts_us,
                            std::string& error) {
#if defined(VISIONCAST_ENABLE_FFMPEG)
    if (!impl_->connected || data == nullptr || size == 0) {
        error = "RTMP pusher is not connected or video frame is empty";
        return false;
    }
    if (!find_nalus(data, size, impl_->video_nalus, error)) {
        return false;
    }
    // 若尚未写入 FLV 文件头，则先解析 SPS/PPS 提取 extradata，并建立网络连接写入文件头
    if (!impl_->header_written) {
        if (!set_avc_extradata(impl_->video_stream->codecpar, impl_->video_nalus, error)) {
            return false;
        }
        int ret = ffmpeg().avio_open(
            &impl_->format->pb, rtmp_url_.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            error = ffmpeg_error("avio_open(RTMP)", ret);
            return false;
        }
        ret = ffmpeg().avformat_write_header(impl_->format, nullptr);
        if (ret < 0) {
            error = ffmpeg_error("avformat_write_header(FLV)", ret);
            return false;
        }
        impl_->first_video_pts_us = pts_us;
        impl_->header_written = true;
    }

    if (!annexb_to_avcc_payload(impl_->video_nalus, size, impl_->video_payload, error)) {
        return false;
    }

    AVPacket packet{};
    packet.data = impl_->video_payload.data();
    packet.size = static_cast<int>(impl_->video_payload.size());
    packet.stream_index = impl_->video_stream->index;
    packet.pts = packet.dts =
        static_cast<std::int64_t>((pts_us - impl_->first_video_pts_us) / 1000U);
    packet.duration = std::max(1, 1000 / std::max(1, video_.fps));
    if (std::any_of(impl_->video_nalus.begin(), impl_->video_nalus.end(), [](const auto& nalu) {
            return nalu.data != nullptr && nalu.size > 0 && (nalu.data[0] & 0x1FU) == 5;
        })) {
        packet.flags |= AV_PKT_FLAG_KEY;
    }
    const int ret = ffmpeg().av_interleaved_write_frame(impl_->format, &packet);
    if (ret < 0) {
        error = ffmpeg_error("av_interleaved_write_frame(H264)", ret);
        return false;
    }
    return encode_pending_audio(*impl_, error);
#else
    (void)data;
    (void)size;
    (void)pts_us;
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
    // 缓存原始 PCM 采样，供之后打包为 AAC 编码帧时使用
    const std::size_t sample_count = size / sizeof(std::int16_t);
    const auto* samples = reinterpret_cast<const std::int16_t*>(data);
    impl_->pcm_samples.insert(
        impl_->pcm_samples.end(), samples, samples + sample_count);
    if (!impl_->header_written) {
        return true;
    }
    return encode_pending_audio(*impl_, error);
#else
    (void)data;
    (void)size;
    (void)pts_us;
    error = "RTMP support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
