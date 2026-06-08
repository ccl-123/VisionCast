/**
 * @file rga_processor.cpp
 * @brief VisionCast RGA 图像处理及格式缩放实现文件
 * @details 实现了基于瑞芯微 RGA 硬件加速芯片的视频图像缩放与格式转换。
 *          支持 YUYV/RGB24/NV12 格式的高效转化。
 *          若输入格式为 MJPEG，会先经过 MJPEG 解码器解出 NV12，再做后续处理。
 *          在 RGA 不可用或被禁用的情况下，提供了 CPU 软件缩放和 YUYV->NV12 色彩空间转换的双向 Fallback 实现。
 */

#include "media/rga_processor.h"

#include <unistd.h>

#include <algorithm>

#include "common/log.h"
#include "common/time_utils.h"
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#if defined(VISIONCAST_ENABLE_RGA)
#include "im2d.h"
#endif

#if defined(VISIONCAST_ENABLE_RGA) && defined(VISIONCAST_ENABLE_MPP)
#include "mpp_buffer.h"
#endif

namespace visioncast {
namespace {

std::size_t nv12_size(int width, int height) {
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U / 2U;
}

int nv12_stride(const VideoFrame& frame) {
    return frame.stride > 0 ? frame.stride : frame.width;
}

int nv12_vertical_stride(const VideoFrame& frame) {
    return frame.vertical_stride > 0 ? frame.vertical_stride : frame.height;
}

int yuyv_stride_bytes(const VideoFrame& frame) {
    return frame.stride > 0 ? frame.stride : frame.width * 2;
}

// CPU 软件下采样与缩放算法：使用最邻近/线性对 NV12 (Y 平面与 UV 平面) 图像进行等比/拉伸缩放。
void copy_nv12_scaled(const VideoFrame& input, VideoFrame& output) {
    const int in_w = input.width;
    const int in_h = input.height;
    const int in_stride = nv12_stride(input);
    const int out_w = output.width;
    const int out_h = output.height;
    const std::uint8_t* src_y = input.data.data();
    const std::uint8_t* src_uv =
        input.data.data() + static_cast<std::size_t>(in_stride) * nv12_vertical_stride(input);
    std::uint8_t* dst_y = output.data.data();
    std::uint8_t* dst_uv = output.data.data() + static_cast<std::size_t>(out_w) * out_h;

    // 1. 缩放 Y 平面
    for (int y = 0; y < out_h; ++y) {
        const int sy = y * in_h / out_h;
        for (int x = 0; x < out_w; ++x) {
            const int sx = x * in_w / out_w;
            dst_y[static_cast<std::size_t>(y) * out_w + x] =
                src_y[static_cast<std::size_t>(sy) * in_stride + sx];
        }
    }

    // 2. 缩放 UV 平面
    for (int y = 0; y < out_h / 2; ++y) {
        const int sy = (y * in_h / out_h) & ~1;
        for (int x = 0; x < out_w; x += 2) {
            const int sx = (x * in_w / out_w) & ~1;
            const std::size_t src = static_cast<std::size_t>(sy / 2) * in_stride + sx;
            const std::size_t dst = static_cast<std::size_t>(y) * out_w + x;
            dst_uv[dst] = src_uv[src];
            dst_uv[dst + 1] = src_uv[src + 1];
        }
    }
}

// CPU 软件 YUYV 到 NV12 转换加缩放算法
// YUYV 每 4 字节表示 2 个像素（Y0 U0 Y1 V0），提取其中的 Y0/Y1 并做缩放，将交织的 U0 和 V0 合并并下采样成 4:2:0 格式的 UV 交织平面。
void yuyv_to_nv12_scaled(const VideoFrame& input, VideoFrame& output) {
    const int in_w = input.width;
    const int in_h = input.height;
    const int in_stride = yuyv_stride_bytes(input);
    const int out_w = output.width;
    const int out_h = output.height;
    std::uint8_t* dst_y = output.data.data();
    std::uint8_t* dst_uv = output.data.data() + static_cast<std::size_t>(out_w) * out_h;

    // 1. 提取并缩放 Y 分量
    for (int y = 0; y < out_h; ++y) {
        const int sy = y * in_h / out_h;
        for (int x = 0; x < out_w; ++x) {
            const int sx = x * in_w / out_w;
            const std::size_t pair_offset = static_cast<std::size_t>(sy) * in_stride +
                                             static_cast<std::size_t>(sx & ~1) * 2U;
            const bool second = (sx & 1) != 0;
            dst_y[static_cast<std::size_t>(y) * out_w + x] =
                input.data[pair_offset + (second ? 2U : 0U)];
        }
    }

    // 2. 提取、下采样并缩放 UV 交织分量
    for (int y = 0; y < out_h / 2; ++y) {
        const int sy = ((2 * y) * in_h / out_h) & ~1;
        for (int x = 0; x < out_w; x += 2) {
            const int sx = (x * in_w / out_w) & ~1;
            const std::size_t src = static_cast<std::size_t>(sy) * in_stride +
                                    static_cast<std::size_t>(sx) * 2U;
            const std::size_t dst = static_cast<std::size_t>(y) * out_w + x;
            dst_uv[dst] = input.data[src + 1U];
            dst_uv[dst + 1U] = input.data[src + 3U];
        }
    }
}

// 格式名称归一化
std::string normalize_format(std::string format) {
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return format;
}

#if defined(VISIONCAST_ENABLE_RGA)
bool rga_success(IM_STATUS status) {
    return status == IM_STATUS_SUCCESS || status == IM_STATUS_NOERROR;
}

// 映射字符串格式至 RGA 内部格式定义
int rga_format(const std::string& format) {
    if (format == "NV12") {
        return RK_FORMAT_YCbCr_420_SP;
    }
    if (format == "YUYV" || format == "YUY2") {
        return RK_FORMAT_YUYV_422;
    }
    if (format == "RGB24") {
        return RK_FORMAT_RGB_888;
    }
    return 0;
}

#if defined(VISIONCAST_ENABLE_MPP)
std::size_t nv12_storage_size(int stride, int vertical_stride) {
    return static_cast<std::size_t>(stride) *
           static_cast<std::size_t>(vertical_stride) * 3U / 2U;
}

MppBufferGroup& rga_dma_group() {
    static MppBufferGroup group = nullptr;
    return group;
}

bool ensure_rga_dma_group(std::string& error) {
    MppBufferGroup& group = rga_dma_group();
    if (group != nullptr) {
        return true;
    }
    const MPP_RET ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK || group == nullptr) {
        error = "allocate RGA DMA buffer group failed ret=" + std::to_string(ret);
        group = nullptr;
        return false;
    }
    return true;
}

bool allocate_rga_dma_output(VideoFrame& output, MppBuffer& output_buffer, std::string& error) {
    if (!ensure_rga_dma_group(error)) {
        return false;
    }
    const std::size_t output_size = nv12_storage_size(output.stride, output.vertical_stride);
    const MPP_RET ret = mpp_buffer_get(rga_dma_group(), &output_buffer, output_size);
    if (ret != MPP_OK || output_buffer == nullptr) {
        error = "allocate RGA DMA output failed ret=" + std::to_string(ret);
        output_buffer = nullptr;
        return false;
    }
    const int output_fd = mpp_buffer_get_fd(output_buffer);
    if (output_fd < 0) {
        mpp_buffer_put(output_buffer);
        output_buffer = nullptr;
        error = "RGA DMA output has no exportable fd";
        return false;
    }
    return true;
}

bool attach_rga_dma_output(VideoFrame& output, MppBuffer output_buffer, std::string& error) {
    const int output_fd = mpp_buffer_get_fd(output_buffer);
    const int frame_fd = dup(output_fd);
    if (frame_fd < 0) {
        mpp_buffer_put(output_buffer);
        error = "dup RGA DMA output fd failed";
        return false;
    }

    auto dma = std::make_shared<VideoDmaBuffer>();
    VideoDmaPlane plane;
    plane.fd = frame_fd;
    plane.offset = 0;
    plane.bytesused = nv12_storage_size(output.stride, output.vertical_stride);
    plane.length = mpp_buffer_get_size(output_buffer);
    dma->planes.push_back(plane);
    dma->release = [output_buffer] {
        mpp_buffer_put(output_buffer);
    };
    output.dma = std::move(dma);
    return true;
}

bool rga_process_dma_nv12(const VideoFrame& input,
                          int output_width,
                          int output_height,
                          VideoFrame& output,
                          std::string& error) {
    const std::string in_format = normalize_format(input.format);
    if (in_format != "NV12" || !input.has_single_plane_dma()) {
        return false;
    }
    const auto& in_plane = input.dma->planes.front();
    if (in_plane.fd < 0 || in_plane.offset != 0) {
        return false;
    }

    output = {};
    output.pts_us = input.pts_us;
    output.width = output_width;
    output.height = output_height;
    output.stride = output_width;
    output.vertical_stride = output_height;
    output.sequence = input.sequence;
    output.format = "NV12";

    MppBuffer output_buffer = nullptr;
    if (!allocate_rga_dma_output(output, output_buffer, error)) {
        return false;
    }

    rga_buffer_t src = wrapbuffer_fd_t(in_plane.fd,
                                      input.width,
                                      input.height,
                                      nv12_stride(input),
                                      nv12_vertical_stride(input),
                                      RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd_t(mpp_buffer_get_fd(output_buffer),
                                      output.width,
                                      output.height,
                                      output.stride,
                                      output.vertical_stride,
                                      RK_FORMAT_YCbCr_420_SP);
    const IM_STATUS status = imresize(src, dst);
    if (!rga_success(status)) {
        mpp_buffer_put(output_buffer);
        error = std::string("RGA DMA resize failed: ") + imStrError_t(status);
        return false;
    }
    return attach_rga_dma_output(output, output_buffer, error);
}
#endif

// 仅在 RGA 中执行同尺寸色彩空间转换
bool rga_convert_same_size(const VideoFrame& input, VideoFrame& output, std::string& error) {
    const std::string in_format = normalize_format(input.format);
    const int src_fmt = rga_format(in_format);
    if (src_fmt == 0) {
        return false;
    }

    const int input_wstride =
        in_format == "NV12" ? nv12_stride(input)
                            : (in_format == "RGB24" ? input.stride / 3
                                                   : yuyv_stride_bytes(input) / 2);
    // 封装输入 buffer 信息
    rga_buffer_t src = wrapbuffer_virtualaddr_t(const_cast<std::uint8_t*>(input.data.data()),
                                                input.width,
                                                input.height,
                                                input_wstride,
                                                in_format == "NV12" ? nv12_vertical_stride(input)
                                                                    : input.height,
                                                src_fmt);
    // 封装输出 buffer 信息
    rga_buffer_t dst = wrapbuffer_virtualaddr_t(output.data.data(),
                                                output.width,
                                                output.height,
                                                output.width,
                                                output.height,
                                                RK_FORMAT_YCbCr_420_SP);
    IM_STATUS status = IM_STATUS_FAILED;
    if (in_format == "NV12") {
        status = imresize(src, dst);
    } else {
        status = imcvtcolor(src, dst, src_fmt, RK_FORMAT_YCbCr_420_SP);
    }
    if (!rga_success(status)) {
        error = std::string("RGA conversion failed: ") + imStrError_t(status);
        return false;
    }
    return true;
}

// RGA 处理逻辑，支持跨格式及缩放
bool rga_process(const VideoFrame& input, VideoFrame& output, std::string& error) {
    const std::string in_format = normalize_format(input.format);
    if (in_format == "NV12" || (input.width == output.width && input.height == output.height)) {
        return rga_convert_same_size(input, output, error);
    }

    // RGA imcvtcolor does not reliably combine color conversion and scaling on all
    // BSP versions, so YUYV resize is handled as convert-to-temp then resize.
    // 某些旧版 BSP 固件上的 RGA 驱动无法可靠合并颜色转换和缩放，
    // 因此对于 YUYV/RGB24 伴随缩放的场景，采用两步法：先用 RGA 硬件转换为等尺寸 NV12 临时帧，再用 RGA 硬件执行缩放至目标 NV12。
    if (in_format == "YUYV" || in_format == "YUY2" || in_format == "RGB24") {
        VideoFrame temp;
        temp.width = input.width;
        temp.height = input.height;
        temp.stride = input.width;
        temp.vertical_stride = input.height;
        temp.format = "NV12";
        temp.pts_us = input.pts_us;
        temp.sequence = input.sequence;
        temp.data.assign(nv12_size(input.width, input.height), 0);
        if (!rga_convert_same_size(input, temp, error)) {
            return false;
        }
        return rga_convert_same_size(temp, output, error);
    }
    return false;
}
#endif

}  // namespace

RgaProcessor::RgaProcessor(int output_width, int output_height)
    : output_width_(output_width), output_height_(output_height) {}

// 图像处理及缩放核心函数
bool RgaProcessor::process(const VideoFrame& input, VideoFrame& output, std::string& error) {
    last_frame_hardware_ = false;
    last_mode_ = "CPU";
    last_mjpeg_input_ = false;
    last_mjpeg_hardware_ = false;
    last_mjpeg_dma_output_ = false;
    last_mjpeg_decode_us_ = 0;

    if (input.width <= 0 || input.height <= 0 ||
        (input.data.empty() && !input.has_dma())) {
        error = "invalid input video frame";
        return false;
    }
    if (output_width_ <= 0 || output_height_ <= 0) {
        error = "invalid RGA output size";
        return false;
    }

    const std::string format = normalize_format(input.format);
    if (format == "NV12" && input.width == output_width_ && input.height == output_height_) {
        output = input;
        output.format = "NV12";
        output.stride = input.stride > 0 ? input.stride : input.width;
        output.vertical_stride =
            input.vertical_stride > 0 ? input.vertical_stride : input.height;
        last_frame_hardware_ = input.has_single_plane_dma();
        last_mode_ = input.has_single_plane_dma() ? "BYPASS-DMA" : "BYPASS";
        return true;
    }

    VideoFrame decoded;
    const VideoFrame* source = &input;
    // 1. 若输入为 MJPEG，则先通过软件或 MPP 解码成 NV12 格式
    if (format == "MJPEG" || format == "MJPG") {
        last_mjpeg_input_ = true;
#if defined(VISIONCAST_ENABLE_RGA) && defined(VISIONCAST_ENABLE_MPP)
        const bool allow_decoder_dma_output = true;
#else
        const bool allow_decoder_dma_output =
            input.width == output_width_ && input.height == output_height_;
#endif
        const std::uint64_t decode_start_us = monotonic_now_us();
        if (!mjpeg_decoder_.decode(input, decoded, error, allow_decoder_dma_output)) {
            last_mjpeg_decode_us_ = monotonic_now_us() - decode_start_us;
            return false;
        }
        last_mjpeg_decode_us_ = monotonic_now_us() - decode_start_us;
        last_mjpeg_hardware_ = mjpeg_decoder_.is_hardware_accelerated();
        last_mjpeg_dma_output_ = decoded.has_single_plane_dma();
        if (decoded.format == "NV12" && decoded.width == output_width_ &&
            decoded.height == output_height_) {
            output = std::move(decoded);
            output.format = "NV12";
            output.stride = output.stride > 0 ? output.stride : output.width;
            output.vertical_stride =
                output.vertical_stride > 0 ? output.vertical_stride : output.height;
            last_frame_hardware_ =
                output.has_single_plane_dma() || mjpeg_decoder_.is_hardware_accelerated();
            last_mode_ = output.has_single_plane_dma() ? "BYPASS-DMA" : "BYPASS";
            return true;
        }
        source = &decoded;
    }

#if defined(VISIONCAST_ENABLE_RGA) && defined(VISIONCAST_ENABLE_MPP)
    std::string dma_error;
    if (rga_process_dma_nv12(*source, output_width_, output_height_, output, dma_error)) {
        last_frame_hardware_ = true;
        last_mode_ = "RGA-FD";
        return true;
    }
    if (!dma_error.empty()) {
        if (source->data.empty()) {
            error = dma_error;
            return false;
        }
        VC_LOG_WARN("rga", dma_error + ", falling back to CPU-visible RGA path");
        error.clear();
    }
#endif

    if (source->data.empty()) {
        error = "DMA-BUF frame requires RGA fd path for resize/format conversion";
        return false;
    }

    // 初始化输出结构
    output = {};
    output.pts_us = input.pts_us;
    output.width = output_width_;
    output.height = output_height_;
    output.stride = output_width_;
    output.vertical_stride = output_height_;
    output.sequence = input.sequence;
    output.format = "NV12";
    output.data.assign(nv12_size(output_width_, output_height_), 0);
#if defined(VISIONCAST_ENABLE_RGA)
    // 2. 尝试使用瑞芯微 RGA 硬件加速接口进行缩放和颜色空间转换
    if (rga_process(*source, output, error)) {
        last_frame_hardware_ = true;
        last_mode_ = "RGA-VA";
        return true;
    }
    if (!error.empty()) {
        VC_LOG_WARN("rga", "RgaProcessor hardware processing failed: " + error + ", falling back to software CPU conversion");
    } else {
        static bool warned_once = false;
        if (!warned_once) {
            VC_LOG_WARN("rga", "RgaProcessor hardware processing failed, falling back to software CPU conversion");
            warned_once = true;
        }
    }
#else
    static bool warned_once = false;
    if (!warned_once) {
        VC_LOG_WARN("rga", "RGA hardware acceleration is not compiled/enabled, using software CPU conversion");
        warned_once = true;
    }
#endif

    last_frame_hardware_ = false;
    last_mode_ = "CPU";
    // 3. Fallback：硬件处理不可用时，使用 CPU 执行软件下采样和缩放
    const std::string source_format = normalize_format(source->format);
    if (source_format == "NV12") {
        const std::size_t expected = static_cast<std::size_t>(nv12_stride(*source)) *
                                     static_cast<std::size_t>(nv12_vertical_stride(*source)) *
                                     3U / 2U;
        if (source->data.size() < expected) {
            error = "NV12 frame is smaller than expected";
            return false;
        }
        copy_nv12_scaled(*source, output);
        return true;
    }

    if (source_format == "YUYV" || source_format == "YUY2") {
        const std::size_t expected = static_cast<std::size_t>(yuyv_stride_bytes(*source)) *
                                     static_cast<std::size_t>(source->height);
        if (source->data.size() < expected) {
            error = "YUYV frame is smaller than expected";
            return false;
        }
        yuyv_to_nv12_scaled(*source, output);
        return true;
    }

    error = "unsupported video format for NV12 conversion: " + source->format;
    return false;
}

bool RgaProcessor::is_hardware_accelerated() const {
    return last_frame_hardware_;
}

std::string RgaProcessor::last_mode() const {
    return last_mode_;
}

bool RgaProcessor::last_mjpeg_input() const {
    return last_mjpeg_input_;
}

bool RgaProcessor::last_mjpeg_hardware() const {
    return last_mjpeg_hardware_;
}

bool RgaProcessor::last_mjpeg_dma_output() const {
    return last_mjpeg_dma_output_;
}

std::uint64_t RgaProcessor::last_mjpeg_decode_us() const {
    return last_mjpeg_decode_us_;
}

}  // namespace visioncast
