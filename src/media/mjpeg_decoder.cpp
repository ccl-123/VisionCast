#include "media/mjpeg_decoder.h"

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <sstream>

#include <jpeglib.h>

#if defined(VISIONCAST_ENABLE_MPP)
#include "mpp_buffer.h"
#include "mpp_dec_cfg.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "rk_mpi.h"
#include "rk_type.h"
#endif

namespace visioncast {

struct MjpegDecoder::Impl {
#if defined(VISIONCAST_ENABLE_MPP)
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup group = nullptr;
    MppBuffer input_buffer = nullptr;
    std::size_t input_capacity = 0;
    MppBuffer output_buffer = nullptr;
    MppFrame output_frame = nullptr;
    int configured_width = 0;
    int configured_height = 0;
    bool mpp_init_attempted = false;
    bool mpp_ready = false;
#endif
};

namespace {

std::size_t nv12_size(int stride, int vertical_stride) {
    return static_cast<std::size_t>(stride) * static_cast<std::size_t>(vertical_stride) * 3U / 2U;
}

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr info) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
    manager->base.format_message(info, manager->message);
    std::longjmp(manager->jump, 1);
}

bool decode_with_libjpeg(const VideoFrame& input, VideoFrame& output, std::string& error) {
    jpeg_decompress_struct decoder{};
    JpegErrorManager jpeg_error{};
    decoder.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_error_exit;
    if (setjmp(jpeg_error.jump) != 0) {
        jpeg_destroy_decompress(&decoder);
        error = std::string("libjpeg MJPEG decode failed: ") + jpeg_error.message;
        return false;
    }

    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder,
                 const_cast<unsigned char*>(input.data.data()),
                 static_cast<unsigned long>(input.data.size()));
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);

    const int width = static_cast<int>(decoder.output_width);
    const int height = static_cast<int>(decoder.output_height);
    const int rgb_stride = width * 3;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(rgb_stride) * height);
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row =
            rgb.data() + static_cast<std::size_t>(decoder.output_scanline) * rgb_stride;
        jpeg_read_scanlines(&decoder, &row, 1);
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);

    output = {};
    output.width = width;
    output.height = height;
    output.stride = width;
    output.vertical_stride = height;
    output.format = "NV12";
    output.pts_us = input.pts_us;
    output.sequence = input.sequence;
    output.data.resize(nv12_size(width, height));
    std::uint8_t* y_plane = output.data.data();
    std::uint8_t* uv_plane =
        output.data.data() + static_cast<std::size_t>(width) * height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto* pixel =
                rgb.data() + static_cast<std::size_t>(y) * rgb_stride + x * 3;
            y_plane[static_cast<std::size_t>(y) * width + x] =
                clamp_byte(((66 * pixel[0] + 129 * pixel[1] + 25 * pixel[2] + 128) >> 8) +
                           16);
        }
    }

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sum_u = 0;
            int sum_v = 0;
            int samples = 0;
            for (int dy = 0; dy < 2 && y + dy < height; ++dy) {
                for (int dx = 0; dx < 2 && x + dx < width; ++dx) {
                    const auto* pixel =
                        rgb.data() + static_cast<std::size_t>(y + dy) * rgb_stride +
                        (x + dx) * 3;
                    sum_u += ((-38 * pixel[0] - 74 * pixel[1] + 112 * pixel[2] + 128) >> 8) +
                             128;
                    sum_v += ((112 * pixel[0] - 94 * pixel[1] - 18 * pixel[2] + 128) >> 8) +
                             128;
                    ++samples;
                }
            }
            const std::size_t offset = static_cast<std::size_t>(y / 2) * width + x;
            uv_plane[offset] = clamp_byte(sum_u / samples);
            if (x + 1 < width) {
                uv_plane[offset + 1] = clamp_byte(sum_v / samples);
            }
        }
    }
    return true;
}

#if defined(VISIONCAST_ENABLE_MPP)
std::string mpp_error(const char* call, MPP_RET ret) {
    std::ostringstream out;
    out << call << " failed ret=" << static_cast<int>(ret);
    return out.str();
}

void close_mpp(MjpegDecoder::Impl& impl) {
    if (impl.ctx != nullptr) {
        mpp_destroy(impl.ctx);
        impl.ctx = nullptr;
        impl.mpi = nullptr;
    }
    if (impl.output_frame != nullptr) {
        mpp_frame_deinit(&impl.output_frame);
    }
    if (impl.output_buffer != nullptr) {
        mpp_buffer_put(impl.output_buffer);
        impl.output_buffer = nullptr;
    }
    if (impl.input_buffer != nullptr) {
        mpp_buffer_put(impl.input_buffer);
        impl.input_buffer = nullptr;
    }
    if (impl.group != nullptr) {
        mpp_buffer_group_put(impl.group);
        impl.group = nullptr;
    }
    impl.input_capacity = 0;
    impl.mpp_ready = false;
}

bool open_mpp(MjpegDecoder::Impl& impl, int width, int height, std::string& error) {
    impl.mpp_init_attempted = true;
    MPP_RET ret = mpp_create(&impl.ctx, &impl.mpi);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_create(MJPEG decoder)", ret);
        close_mpp(impl);
        return false;
    }
    ret = mpp_init(impl.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_init(MJPEG decoder)", ret);
        close_mpp(impl);
        return false;
    }

    MppDecCfg cfg = nullptr;
    ret = mpp_dec_cfg_init(&cfg);
    if (ret == MPP_OK) {
        ret = impl.mpi->control(impl.ctx, MPP_DEC_GET_CFG, cfg);
    }
    if (ret == MPP_OK) {
        ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
    }
    if (ret == MPP_OK) {
        ret = impl.mpi->control(impl.ctx, MPP_DEC_SET_CFG, cfg);
    }
    if (cfg != nullptr) {
        mpp_dec_cfg_deinit(cfg);
    }
    if (ret != MPP_OK) {
        error = mpp_error("configure MPP MJPEG decoder", ret);
        close_mpp(impl);
        return false;
    }

    MppFrameFormat output_format = MPP_FMT_YUV420SP;
    ret = impl.mpi->control(impl.ctx, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
    if (ret == MPP_OK) {
        ret = mpp_frame_init(&impl.output_frame);
    }
    if (ret == MPP_OK) {
        ret = mpp_buffer_group_get_internal(&impl.group, MPP_BUFFER_TYPE_DRM);
    }
    const std::size_t output_capacity =
        static_cast<std::size_t>((width + 15) & ~15) *
        static_cast<std::size_t>((height + 15) & ~15) * 4U;
    if (ret == MPP_OK) {
        ret = mpp_buffer_get(impl.group, &impl.output_buffer, output_capacity);
    }
    if (ret == MPP_OK) {
        mpp_frame_set_buffer(impl.output_frame, impl.output_buffer);
    }
    if (ret != MPP_OK) {
        error = mpp_error("allocate MPP MJPEG output", ret);
        close_mpp(impl);
        return false;
    }

    impl.configured_width = width;
    impl.configured_height = height;
    impl.mpp_ready = true;
    return true;
}

bool decode_with_mpp(MjpegDecoder::Impl& impl,
                     const VideoFrame& input,
                     VideoFrame& output,
                     std::string& error) {
    if (input.data.size() > impl.input_capacity) {
        if (impl.input_buffer != nullptr) {
            mpp_buffer_put(impl.input_buffer);
            impl.input_buffer = nullptr;
            impl.input_capacity = 0;
        }
        impl.input_capacity = (input.data.size() + 4095U) & ~static_cast<std::size_t>(4095U);
        const MPP_RET ret =
            mpp_buffer_get(impl.group, &impl.input_buffer, impl.input_capacity);
        if (ret != MPP_OK) {
            error = mpp_error("mpp_buffer_get(MJPEG input)", ret);
            impl.input_capacity = 0;
            return false;
        }
    }

    MPP_RET ret = mpp_buffer_write(impl.input_buffer,
                                   0,
                                   const_cast<std::uint8_t*>(input.data.data()),
                                   input.data.size());
    MppPacket packet = nullptr;
    if (ret == MPP_OK) {
        ret = mpp_packet_init_with_buffer(&packet, impl.input_buffer);
    }
    if (ret != MPP_OK) {
        error = mpp_error("prepare MPP MJPEG packet", ret);
        return false;
    }
    mpp_packet_set_pos(packet, mpp_buffer_get_ptr(impl.input_buffer));
    mpp_packet_set_size(packet, input.data.size());
    mpp_packet_set_length(packet, input.data.size());
    mpp_packet_set_pts(packet, static_cast<RK_S64>(input.pts_us));

    MppTask input_task = nullptr;
    ret = impl.mpi->poll(impl.ctx, MPP_PORT_INPUT, static_cast<MppPollType>(1000));
    if (ret == MPP_OK) {
        ret = impl.mpi->dequeue(impl.ctx, MPP_PORT_INPUT, &input_task);
    }
    if (ret == MPP_OK && input_task != nullptr) {
        ret = mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, packet);
    }
    if (ret == MPP_OK) {
        ret = mpp_task_meta_set_frame(input_task, KEY_OUTPUT_FRAME, impl.output_frame);
    }
    if (ret == MPP_OK) {
        ret = impl.mpi->enqueue(impl.ctx, MPP_PORT_INPUT, input_task);
    }
    if (ret != MPP_OK) {
        mpp_packet_deinit(&packet);
        error = mpp_error("submit MPP MJPEG task", ret);
        return false;
    }

    MppTask output_task = nullptr;
    ret = impl.mpi->poll(impl.ctx, MPP_PORT_OUTPUT, static_cast<MppPollType>(1000));
    if (ret == MPP_OK) {
        ret = impl.mpi->dequeue(impl.ctx, MPP_PORT_OUTPUT, &output_task);
    }
    MppFrame decoded_frame = nullptr;
    if (ret == MPP_OK && output_task != nullptr) {
        ret = mpp_task_meta_get_frame(output_task, KEY_OUTPUT_FRAME, &decoded_frame);
    }
    if (output_task != nullptr) {
        const MPP_RET enqueue_ret =
            impl.mpi->enqueue(impl.ctx, MPP_PORT_OUTPUT, output_task);
        if (ret == MPP_OK) {
            ret = enqueue_ret;
        }
    }

    MppTask returned_input_task = nullptr;
    MppPacket returned_packet = nullptr;
    MPP_RET input_ret =
        impl.mpi->dequeue(impl.ctx, MPP_PORT_INPUT, &returned_input_task);
    if (input_ret == MPP_OK && returned_input_task != nullptr) {
        input_ret =
            mpp_task_meta_get_packet(returned_input_task, KEY_INPUT_PACKET, &returned_packet);
        if (returned_packet != nullptr) {
            mpp_packet_deinit(&returned_packet);
        }
        const MPP_RET enqueue_ret =
            impl.mpi->enqueue(impl.ctx, MPP_PORT_INPUT, returned_input_task);
        if (input_ret == MPP_OK) {
            input_ret = enqueue_ret;
        }
    }
    if (ret != MPP_OK || input_ret != MPP_OK || decoded_frame == nullptr) {
        error = "MPP MJPEG task did not return a decoded frame";
        return false;
    }
    if (mpp_frame_get_errinfo(decoded_frame) != 0 ||
        mpp_frame_get_discard(decoded_frame) != 0) {
        error = "MPP MJPEG decoder marked frame invalid";
        return false;
    }

    const int width = static_cast<int>(mpp_frame_get_width(decoded_frame));
    const int height = static_cast<int>(mpp_frame_get_height(decoded_frame));
    const int horizontal_stride =
        static_cast<int>(mpp_frame_get_hor_stride(decoded_frame));
    const int vertical_stride =
        static_cast<int>(mpp_frame_get_ver_stride(decoded_frame));
    MppBuffer decoded_buffer = mpp_frame_get_buffer(decoded_frame);
    const auto* decoded_ptr =
        static_cast<const std::uint8_t*>(mpp_buffer_get_ptr(decoded_buffer));
    if (width <= 0 || height <= 0 || horizontal_stride < width ||
        vertical_stride < height || decoded_ptr == nullptr) {
        error = "MPP MJPEG decoder returned invalid frame geometry";
        return false;
    }

    output = {};
    output.width = width;
    output.height = height;
    output.stride = horizontal_stride;
    output.vertical_stride = vertical_stride;
    output.format = "NV12";
    output.pts_us = input.pts_us;
    output.sequence = input.sequence;
    output.data.assign(decoded_ptr,
                       decoded_ptr + nv12_size(horizontal_stride, vertical_stride));
    return true;
}
#endif

}  // namespace

MjpegDecoder::MjpegDecoder()
    : impl_(std::make_unique<Impl>()) {}

MjpegDecoder::~MjpegDecoder() {
#if defined(VISIONCAST_ENABLE_MPP)
    close_mpp(*impl_);
#endif
}

bool MjpegDecoder::decode(const VideoFrame& input,
                          VideoFrame& output,
                          std::string& error) {
    if (input.data.empty() || input.width <= 0 || input.height <= 0) {
        error = "invalid MJPEG frame";
        return false;
    }

#if defined(VISIONCAST_ENABLE_MPP)
    if (!impl_->mpp_init_attempted) {
        std::string init_error;
        open_mpp(*impl_, input.width, input.height, init_error);
        if (!impl_->mpp_ready) {
            error = init_error;
        }
    }
    if (impl_->mpp_ready) {
        return decode_with_mpp(*impl_, input, output, error);
    }
#endif

    error.clear();
    return decode_with_libjpeg(input, output, error);
}

}  // namespace visioncast
