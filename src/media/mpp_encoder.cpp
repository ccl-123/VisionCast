#include "media/mpp_encoder.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include "media/media_clock.h"

#if defined(VISIONCAST_ENABLE_MPP)
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "rk_mpi.h"
#include "rk_type.h"
#include "rk_venc_cfg.h"
#endif

namespace visioncast {

struct MppEncoder::Impl {
#if defined(VISIONCAST_ENABLE_MPP)
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup group = nullptr;
    MppEncCfg cfg = nullptr;
#endif
    bool opened = false;
};

namespace {

#if defined(VISIONCAST_ENABLE_MPP)
std::string mpp_error(const char* call, int ret) {
    std::ostringstream out;
    out << call << " failed ret=" << ret;
    return out.str();
}

bool set_s32(MppEncCfg cfg, const char* key, int value, std::string& error) {
    if (cfg == nullptr) {
        error = std::string("MPP config is null before setting ") + key;
        return false;
    }
    const MPP_RET ret = mpp_enc_cfg_set_s32(cfg, key, value);
    if (ret != MPP_OK) {
        error = mpp_error(key, ret);
        return false;
    }
    return true;
}

bool contains_h264_nalu_type(const std::uint8_t* data,
                             std::size_t size,
                             std::uint8_t expected_type) {
    for (std::size_t i = 0; i + 4 < size; ++i) {
        std::size_t prefix = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            prefix = 3;
        } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            prefix = 4;
        }
        if (prefix != 0 && i + prefix < size &&
            (data[i + prefix] & 0x1FU) == expected_type) {
            return true;
        }
    }
    return false;
}
#endif

}  // namespace

MppEncoder::MppEncoder(VideoConfig video, EncoderConfig encoder)
    : impl_(std::make_unique<Impl>()),
      video_(std::move(video)),
      encoder_(std::move(encoder)) {}

MppEncoder::~MppEncoder() {
    close();
}

bool MppEncoder::open(std::string& error) {
#if defined(VISIONCAST_ENABLE_MPP)
    if (impl_->opened) {
        return true;
    }

    MPP_RET ret = mpp_create(&impl_->ctx, &impl_->mpi);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_create", ret);
        return false;
    }

    ret = mpp_init(impl_->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_init(H264 encoder)", ret);
        close();
        return false;
    }

    ret = mpp_enc_cfg_init(&impl_->cfg);
    if (ret != MPP_OK || impl_->cfg == nullptr) {
        error = ret == MPP_OK ? "mpp_enc_cfg_init returned null config"
                              : mpp_error("mpp_enc_cfg_init", ret);
        close();
        return false;
    }

    ret = impl_->mpi->control(impl_->ctx, MPP_ENC_GET_CFG, impl_->cfg);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_ENC_GET_CFG", ret);
        close();
        return false;
    }

    const int fps = std::max(1, video_.fps);
    const int bps = std::max(1, encoder_.bitrate);
    const int gop = std::max(1, encoder_.gop);
    if (!set_s32(impl_->cfg, "prep:width", video_.width, error) ||
        !set_s32(impl_->cfg, "prep:height", video_.height, error) ||
        !set_s32(impl_->cfg, "prep:hor_stride", video_.width, error) ||
        !set_s32(impl_->cfg, "prep:ver_stride", video_.height, error) ||
        !set_s32(impl_->cfg, "prep:format", MPP_FMT_YUV420SP, error) ||
        !set_s32(impl_->cfg, "rc:mode", MPP_ENC_RC_MODE_CBR, error) ||
        !set_s32(impl_->cfg, "rc:bps_target", bps, error) ||
        !set_s32(impl_->cfg, "rc:bps_max", bps, error) ||
        !set_s32(impl_->cfg, "rc:bps_min", bps, error) ||
        !set_s32(impl_->cfg, "rc:fps_in_flex", 0, error) ||
        !set_s32(impl_->cfg, "rc:fps_in_num", fps, error) ||
        !set_s32(impl_->cfg, "rc:fps_in_denorm", 1, error) ||
        !set_s32(impl_->cfg, "rc:fps_out_flex", 0, error) ||
        !set_s32(impl_->cfg, "rc:fps_out_num", fps, error) ||
        !set_s32(impl_->cfg, "rc:fps_out_denorm", 1, error) ||
        !set_s32(impl_->cfg, "rc:gop", gop, error) ||
        !set_s32(impl_->cfg, "h264:stream_type", 0, error) ||
        !set_s32(impl_->cfg, "h264:profile", 66, error) ||
        !set_s32(impl_->cfg, "h264:level", 31, error)) {
        close();
        return false;
    }

    ret = impl_->mpi->control(impl_->ctx, MPP_ENC_SET_CFG, impl_->cfg);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_ENC_SET_CFG", ret);
        close();
        return false;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = impl_->mpi->control(impl_->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_ENC_SET_HEADER_MODE", ret);
        close();
        return false;
    }

    ret = mpp_buffer_group_get(&impl_->group,
                               MPP_BUFFER_TYPE_DRM,
                               MPP_BUFFER_INTERNAL,
                               "visioncast",
                               "MppEncoder::open");
    if (ret != MPP_OK) {
        error = mpp_error("mpp_buffer_group_get_internal", ret);
        close();
        return false;
    }

    impl_->opened = true;
    return true;
#else
    error = "MPP support is not enabled in this build";
    return false;
#endif
}

void MppEncoder::close() {
#if defined(VISIONCAST_ENABLE_MPP)
    if (impl_->ctx != nullptr) {
        mpp_destroy(impl_->ctx);
        impl_->ctx = nullptr;
        impl_->mpi = nullptr;
    }
    if (impl_->cfg != nullptr) {
        mpp_enc_cfg_deinit(impl_->cfg);
        impl_->cfg = nullptr;
    }
    if (impl_->group != nullptr) {
        mpp_buffer_group_put(impl_->group);
        impl_->group = nullptr;
    }
#endif
    impl_->opened = false;
}

std::string MppEncoder::backend_version() {
#if defined(VISIONCAST_ENABLE_MPP)
    return "Rockchip MPP enabled";
#else
    return "Rockchip MPP disabled";
#endif
}

bool MppEncoder::encode(const VideoFrame& frame, EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_MPP)
    if (!impl_->opened && !open(error)) {
        return false;
    }
    if (frame.format != "NV12") {
        error = "MPP encoder expects NV12 frame";
        return false;
    }

    MppBuffer buffer = nullptr;
    const MPP_RET buffer_ret = mpp_buffer_get(impl_->group, &buffer, frame.data.size());
    if (buffer_ret != MPP_OK) {
        error = mpp_error("mpp_buffer_get", buffer_ret);
        return false;
    }

    std::memcpy(mpp_buffer_get_ptr(buffer), frame.data.data(), frame.data.size());
    mpp_buffer_sync_end(buffer);

    MppFrame mpp_frame = nullptr;
    MPP_RET ret = mpp_frame_init(&mpp_frame);
    if (ret != MPP_OK) {
        mpp_buffer_put(buffer);
        error = mpp_error("mpp_frame_init", ret);
        return false;
    }

    mpp_frame_set_width(mpp_frame, frame.width);
    mpp_frame_set_height(mpp_frame, frame.height);
    mpp_frame_set_hor_stride(mpp_frame, frame.stride > 0 ? frame.stride : frame.width);
    mpp_frame_set_ver_stride(
        mpp_frame, frame.vertical_stride > 0 ? frame.vertical_stride : frame.height);
    mpp_frame_set_fmt(mpp_frame, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(mpp_frame, static_cast<RK_S64>(frame.pts_us));
    mpp_frame_set_buffer(mpp_frame, buffer);

    MppPacket out = nullptr;
    ret = impl_->mpi->encode(impl_->ctx, mpp_frame, &out);
    mpp_frame_deinit(&mpp_frame);
    mpp_buffer_put(buffer);
    if (ret != MPP_OK) {
        error = mpp_error("mpi->encode", ret);
        return false;
    }
    if (out == nullptr) {
        error = "MPP returned no packet";
        return false;
    }

    const auto* pos = static_cast<const std::uint8_t*>(mpp_packet_get_pos(out));
    const std::size_t length = mpp_packet_get_length(out);
    packet = {};
    packet.media_type = MediaType::Video;
    packet.pts_us = frame.pts_us;
    packet.key_frame = contains_h264_nalu_type(pos, length, 5);
    packet.rtp_timestamp = MediaClock::video_rtp_timestamp(frame.pts_us);
    packet.data.assign(pos, pos + length);
    mpp_packet_deinit(&out);
    return true;
#else
    (void)frame;
    (void)packet;
    error = "MPP support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
