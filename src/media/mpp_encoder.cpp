/**
 * @file mpp_encoder.cpp
 * @brief VisionCast MPP 视频硬编码实现文件
 * @details 基于瑞芯微 MPP (Media Process Platform) 框架，实现了将 NV12 (YUV420sp)
 *          原始像素视频帧通过硬件 H.264 编码器压缩为 H.264 (AVC) Annex-B 码流。
 *          支持 CBR 码率控制、GOP 配置、SPS/PPS 头部自动插入，并实现了 NALU 帧类型解析（探测 I 帧/关键帧）。
 */

#include "media/mpp_encoder.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include "media/media_clock.h"

#if defined(VISIONCAST_ENABLE_MPP)
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_meta.h"
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
    MppBuffer packet_buffer = nullptr;
    std::size_t packet_buffer_capacity = 0;
#endif
    bool opened = false;
    bool last_input_dma = false;
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

// 辅助函数：扫描 H.264 码流数据包，检测是否存在指定类型的 NAL 单元（如 expected_type=5 表示 IDR/I帧）。
// 支持匹配 3 字节 (00 00 01) 与 4 字节 (00 00 00 01) 的 Annex-B 起始码前缀。
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

std::size_t nv12_storage_size(const VideoFrame& frame) {
    const int stride = frame.stride > 0 ? frame.stride : frame.width;
    const int vertical_stride =
        frame.vertical_stride > 0 ? frame.vertical_stride : frame.height;
    if (stride <= 0 || vertical_stride <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(stride) *
           static_cast<std::size_t>(vertical_stride) * 3U / 2U;
}

}  // namespace

MppEncoder::MppEncoder(VideoConfig video, EncoderConfig encoder)
    : impl_(std::make_unique<Impl>()),
      video_(std::move(video)),
      encoder_(std::move(encoder)) {}

MppEncoder::~MppEncoder() {
    close();
}

// 打开并配置 MPP 硬件编码器
bool MppEncoder::open(std::string& error) {
#if defined(VISIONCAST_ENABLE_MPP)
    if (impl_->opened) {
        return true;
    }

    // 1. 创建 MPP 上下文和 API 接口指针
    MPP_RET ret = mpp_create(&impl_->ctx, &impl_->mpi);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_create", ret);
        return false;
    }

    // 2. 初始化为 H.264 (AVC) 视频硬编码器
    ret = mpp_init(impl_->ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_init(H264 encoder)", ret);
        close();
        return false;
    }

    // 3. 配置编码器基础属性，获取空配置句柄
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

    // 4. 设置视频源图像属性、码率模式（CBR 恒定码率）、帧率、GOP、等级和格式
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

    // 5. 应用配置参数
    ret = impl_->mpi->control(impl_->ctx, MPP_ENC_SET_CFG, impl_->cfg);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_ENC_SET_CFG", ret);
        close();
        return false;
    }

    // 6. 配置头部模式：在每个 IDR 帧前均插入 SPS/PPS 元数据，方便网络流随时接入解码
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    ret = impl_->mpi->control(impl_->ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_ENC_SET_HEADER_MODE", ret);
        close();
        return false;
    }

    // 7. 使用 DRM 物理映射缓冲区以获得高吞吐量
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

// 关闭硬件编码器并释放相关缓冲区
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
    if (impl_->packet_buffer != nullptr) {
        mpp_buffer_put(impl_->packet_buffer);
        impl_->packet_buffer = nullptr;
        impl_->packet_buffer_capacity = 0;
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

bool MppEncoder::last_input_dma() const {
    return impl_->last_input_dma;
}

// 编码单帧 NV12 为 H.264 Packet
bool MppEncoder::encode(const VideoFrame& frame, EncodedPacket& packet, std::string& error) {
#if defined(VISIONCAST_ENABLE_MPP)
    impl_->last_input_dma = false;
    if (!impl_->opened && !open(error)) {
        return false;
    }
    if (frame.format != "NV12") {
        error = "MPP encoder expects NV12 frame";
        return false;
    }

    // 1. 准备 MPP 输入缓冲区：优先导入 DMA-BUF，失败或无 DMA-BUF 时走旧拷贝路径。
    MppBuffer buffer = nullptr;
    const std::size_t input_size =
        !frame.data.empty() ? frame.data.size()
                            : std::max(nv12_storage_size(frame), frame.dma_bytesused());
    if (input_size == 0) {
        error = "invalid NV12 frame size";
        return false;
    }
    if (frame.has_single_plane_dma()) {
        const auto& plane = frame.dma->planes.front();
        if (plane.fd >= 0 && plane.length > 0) {
            MppBufferInfo info{};
            info.type = MPP_BUFFER_TYPE_EXT_DMA;
            info.size = plane.length;
            info.fd = plane.fd;
            info.index = static_cast<int>(frame.dma->buffer_index);
            MPP_RET import_ret = mpp_buffer_import(&buffer, &info);
            if (import_ret == MPP_OK && plane.offset > 0) {
                import_ret = mpp_buffer_set_offset(buffer, plane.offset);
            }
            if (import_ret != MPP_OK) {
                if (buffer != nullptr) {
                    mpp_buffer_put(buffer);
                    buffer = nullptr;
                }
                if (frame.data.empty()) {
                    error = mpp_error("mpp_buffer_import(NV12 dma)", import_ret);
                    return false;
                }
            } else {
                impl_->last_input_dma = true;
            }
        }
    }
    MppPacket output_packet = nullptr;

    if (buffer == nullptr) {
        if (frame.data.empty()) {
            error = "MPP encoder has no CPU-visible NV12 data for fallback";
            return false;
        }
        const MPP_RET buffer_ret = mpp_buffer_get(impl_->group, &buffer, frame.data.size());
        if (buffer_ret != MPP_OK) {
            error = mpp_error("mpp_buffer_get", buffer_ret);
            return false;
        }

        // 拷贝图像源数据至物理内存区
        std::memcpy(mpp_buffer_get_ptr(buffer), frame.data.data(), frame.data.size());
        mpp_buffer_sync_end(buffer);
    }

    // 2. 构造 MPP 帧元数据，描述物理缓冲区属性
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

    // 3. 为输出的 H.264 码流包预先分配物理内存
    const std::size_t packet_capacity =
        std::max<std::size_t>(input_size, static_cast<std::size_t>(encoder_.bitrate / 8));
    if (impl_->packet_buffer == nullptr ||
        impl_->packet_buffer_capacity < packet_capacity) {
        if (impl_->packet_buffer != nullptr) {
            mpp_buffer_put(impl_->packet_buffer);
            impl_->packet_buffer = nullptr;
            impl_->packet_buffer_capacity = 0;
        }
        ret = mpp_buffer_get(impl_->group, &impl_->packet_buffer, packet_capacity);
        if (ret != MPP_OK) {
            mpp_frame_deinit(&mpp_frame);
            mpp_buffer_put(buffer);
            error = mpp_error("mpp_buffer_get(packet)", ret);
            return false;
        }
        impl_->packet_buffer_capacity = packet_capacity;
    }
    ret = mpp_packet_init_with_buffer(&output_packet, impl_->packet_buffer);
    if (ret != MPP_OK) {
        mpp_frame_deinit(&mpp_frame);
        mpp_buffer_put(buffer);
        error = mpp_error("mpp_packet_init_with_buffer", ret);
        return false;
    }
    mpp_packet_set_length(output_packet, 0);
    mpp_meta_set_packet(mpp_frame_get_meta(mpp_frame), KEY_OUTPUT_PACKET, output_packet);

    // 4. 将 YUV 帧送入硬编码器，完成后释放帧描述
    ret = impl_->mpi->encode_put_frame(impl_->ctx, mpp_frame);
    mpp_frame_deinit(&mpp_frame);
    if (ret != MPP_OK) {
        mpp_buffer_put(buffer);
        mpp_packet_deinit(&output_packet);
        error = mpp_error("mpi->encode_put_frame", ret);
        return false;
    }

    // 5. 从硬编码器获取编码完成的 H.264 码流包
    MppPacket out = nullptr;
    ret = impl_->mpi->encode_get_packet(impl_->ctx, &out);
    if (ret != MPP_OK) {
        mpp_buffer_put(buffer);
        mpp_packet_deinit(&output_packet);
        error = mpp_error("mpi->encode_get_packet", ret);
        return false;
    }
    // 处理 MPP 空帧输出（表示在等待帧组合或没有可用包）
    if (out == nullptr) {
        mpp_buffer_put(buffer);
        mpp_packet_deinit(&output_packet);
        packet = {};
        packet.media_type = MediaType::Video;
        packet.pts_us = frame.pts_us;
        packet.rtp_timestamp = MediaClock::video_rtp_timestamp(frame.pts_us);
        return true;
    }

    // 6. 解析数据包，检测 H.264 NALU 类型是否包含关键帧 (IDR = 5)
    const auto* pos = static_cast<const std::uint8_t*>(mpp_packet_get_pos(out));
    const std::size_t length = mpp_packet_get_length(out);
    packet = {};
    packet.media_type = MediaType::Video;
    packet.pts_us = frame.pts_us;
    packet.rtp_timestamp = MediaClock::video_rtp_timestamp(frame.pts_us);
    if (pos == nullptr || length == 0) {
        const bool same_packet = out == output_packet;
        mpp_packet_deinit(&out);
        if (!same_packet) {
            mpp_packet_deinit(&output_packet);
        }
        mpp_buffer_put(buffer);
        return true;
    }
    packet.key_frame = contains_h264_nalu_type(pos, length, 5);
    packet.data.assign(pos, pos + length);

    // 7. 资源回收
    const bool same_packet = out == output_packet;
    mpp_packet_deinit(&out);
    if (!same_packet) {
        mpp_packet_deinit(&output_packet);
    }
    mpp_buffer_put(buffer);
    return true;
#else
    (void)frame;
    (void)packet;
    error = "MPP support is not enabled in this build";
    return false;
#endif
}

}  // namespace visioncast
