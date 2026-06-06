#include "media/rga_processor.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#if defined(VISIONCAST_ENABLE_RGA)
#include "im2d.h"
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

    for (int y = 0; y < out_h; ++y) {
        const int sy = y * in_h / out_h;
        for (int x = 0; x < out_w; ++x) {
            const int sx = x * in_w / out_w;
            dst_y[static_cast<std::size_t>(y) * out_w + x] =
                src_y[static_cast<std::size_t>(sy) * in_stride + sx];
        }
    }

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

void yuyv_to_nv12_scaled(const VideoFrame& input, VideoFrame& output) {
    const int in_w = input.width;
    const int in_h = input.height;
    const int in_stride = yuyv_stride_bytes(input);
    const int out_w = output.width;
    const int out_h = output.height;
    std::uint8_t* dst_y = output.data.data();
    std::uint8_t* dst_uv = output.data.data() + static_cast<std::size_t>(out_w) * out_h;

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
    rga_buffer_t src = wrapbuffer_virtualaddr_t(const_cast<std::uint8_t*>(input.data.data()),
                                                input.width,
                                                input.height,
                                                input_wstride,
                                                in_format == "NV12" ? nv12_vertical_stride(input)
                                                                    : input.height,
                                                src_fmt);
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

bool rga_process(const VideoFrame& input, VideoFrame& output, std::string& error) {
    const std::string in_format = normalize_format(input.format);
    if (in_format == "NV12" || (input.width == output.width && input.height == output.height)) {
        return rga_convert_same_size(input, output, error);
    }

    // RGA imcvtcolor does not reliably combine color conversion and scaling on all
    // BSP versions, so YUYV resize is handled as convert-to-temp then resize.
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

bool RgaProcessor::process(const VideoFrame& input, VideoFrame& output, std::string& error) {
    if (input.width <= 0 || input.height <= 0 || input.data.empty()) {
        error = "invalid input video frame";
        return false;
    }
    if (output_width_ <= 0 || output_height_ <= 0) {
        error = "invalid RGA output size";
        return false;
    }

    output = {};
    output.pts_us = input.pts_us;
    output.width = output_width_;
    output.height = output_height_;
    output.stride = output_width_;
    output.vertical_stride = output_height_;
    output.sequence = input.sequence;
    output.format = "NV12";
    output.data.assign(nv12_size(output_width_, output_height_), 0);

    const std::string format = normalize_format(input.format);
    VideoFrame decoded;
    const VideoFrame* source = &input;
    if (format == "MJPEG" || format == "MJPG") {
        if (!mjpeg_decoder_.decode(input, decoded, error)) {
            return false;
        }
        source = &decoded;
    }
#if defined(VISIONCAST_ENABLE_RGA)
    if (rga_process(*source, output, error)) {
        return true;
    }
    if (!error.empty()) {
        return false;
    }
#endif
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

}  // namespace visioncast
