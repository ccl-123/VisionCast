/**
 * @file mjpeg_decoder.cpp
 * @brief VisionCast MJPEG 视频解码实现文件
 * @details 提供硬件与软件双模 MJPEG 解码支持：
 *          1. 硬件解码：基于瑞芯微 MPP (Media Process Platform) 框架，执行硬解，并直接输出 NV12 帧以节省 CPU 消耗；
 *          2. 软件解码：基于 libjpeg (-turbo) 进行 RGB 解码，并在 CPU 上运行色彩转换算法将 RGB 转换并降采样为 NV12 (YUV420sp)。
 */

#include "media/mjpeg_decoder.h"

#include <unistd.h>

#include <algorithm>
#include "common/log.h"
#include <csetjmp>
#include <cstring>
#include <mutex>
#include <sstream>

#include <jpeglib.h>

#if defined(VISIONCAST_ENABLE_MPP)
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_task.h"
#include "rk_mpi.h"
#include "rk_type.h"
#endif

namespace visioncast {

#if defined(VISIONCAST_ENABLE_MPP)
namespace {
struct MjpegOutputBufferPool;
}
#endif

struct MjpegDecoder::Impl {
#if defined(VISIONCAST_ENABLE_MPP)
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup group = nullptr;
    MppBuffer input_buffer = nullptr;
    std::shared_ptr<MjpegOutputBufferPool> output_pool;
    std::size_t input_capacity = 0;
    int configured_width = 0;
    int configured_height = 0;
    bool mpp_init_attempted = false;
    bool mpp_ready = false;
#endif
    bool last_frame_hardware = false;
};

namespace {

// 计算 NV12 (YUV420sp) 图像格式所需的字节缓冲区大小 (width * height * 1.5)
std::size_t nv12_size(int stride, int vertical_stride) {
    return static_cast<std::size_t>(stride) * static_cast<std::size_t>(vertical_stride) * 3U / 2U;
}

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::max(0, std::min(255, value)));
}

// libjpeg 错误管理结构体，用于 longjmp 错误流控制
struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
    char message[JMSG_LENGTH_MAX]{};
};

// libjpeg 错误发生时的退出回调
void jpeg_error_exit(j_common_ptr info) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(info->err);
    manager->base.format_message(info, manager->message);
    std::longjmp(manager->jump, 1);
}

// 利用 libjpeg 执行软件 MJPEG 到 NV12 转换
bool decode_with_libjpeg(const VideoFrame& input, VideoFrame& output, std::string& error) {
    if (input.data.empty()) {
        error = "libjpeg MJPEG decode requires CPU-visible input data";
        return false;
    }

    jpeg_decompress_struct decoder{};
    JpegErrorManager jpeg_error{};
    decoder.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_error_exit;
    // 异常流程跳转点
    if (setjmp(jpeg_error.jump) != 0) {
        jpeg_destroy_decompress(&decoder);
        error = std::string("libjpeg MJPEG decode failed: ") + jpeg_error.message;
        return false;
    }

    jpeg_create_decompress(&decoder);
    // 绑定内存数据源
    jpeg_mem_src(&decoder,
                 const_cast<unsigned char*>(input.data.data()),
                 static_cast<unsigned long>(input.data.size()));
    jpeg_read_header(&decoder, TRUE);
    // 设定输出格式为 RGB
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);

    const int width = static_cast<int>(decoder.output_width);
    const int height = static_cast<int>(decoder.output_height);
    const int rgb_stride = width * 3;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(rgb_stride) * height);
    // 逐行解压
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

    // 1. 提取 Y 平面 (BT.601 YUV 转换公式)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto* pixel =
                rgb.data() + static_cast<std::size_t>(y) * rgb_stride + x * 3;
            y_plane[static_cast<std::size_t>(y) * width + x] =
                clamp_byte(((66 * pixel[0] + 129 * pixel[1] + 25 * pixel[2] + 128) >> 8) +
                           16);
        }
    }

    // 2. 提取并下采样交织的 UV (CbCr) 面 (4:2:0 格式)
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
// 构造 MPP 调用出错日志
std::string mpp_error(const char* call, MPP_RET ret) {
    std::ostringstream out;
    out << call << " failed ret=" << static_cast<int>(ret);
    return out.str();
}

struct MjpegOutputBufferPool {
    ~MjpegOutputBufferPool() {
        shutdown();
    }

    bool acquire(MppBufferGroup group,
                 std::size_t required_size,
                 MppBuffer& buffer,
                 std::string& error) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = free_buffers.begin(); it != free_buffers.end(); ++it) {
                if (mpp_buffer_get_size(*it) >= required_size) {
                    buffer = *it;
                    free_buffers.erase(it);
                    return true;
                }
            }
        }

        const MPP_RET ret = mpp_buffer_get(group, &buffer, required_size);
        if (ret != MPP_OK || buffer == nullptr) {
            buffer = nullptr;
            error = mpp_error("mpp_buffer_get(MJPEG output)", ret);
            return false;
        }
        return true;
    }

    void release(MppBuffer buffer) {
        if (buffer == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (active && free_buffers.size() < max_free_buffers) {
            free_buffers.push_back(buffer);
            return;
        }
        mpp_buffer_put(buffer);
    }

    void shutdown() {
        std::vector<MppBuffer> buffers;
        {
            std::lock_guard<std::mutex> lock(mutex);
            active = false;
            buffers.swap(free_buffers);
        }
        for (MppBuffer buffer : buffers) {
            if (buffer != nullptr) {
                mpp_buffer_put(buffer);
            }
        }
    }

    std::mutex mutex;
    std::vector<MppBuffer> free_buffers;
    bool active = true;
    static constexpr std::size_t max_free_buffers = 4;
};

void release_output_buffer(const std::shared_ptr<MjpegOutputBufferPool>& pool,
                           MppBuffer& buffer) {
    if (buffer == nullptr) {
        return;
    }
    if (pool != nullptr) {
        pool->release(buffer);
    } else {
        mpp_buffer_put(buffer);
    }
    buffer = nullptr;
}

std::string mpp_frame_status(const MppFrame frame) {
    const RK_U32 info_change = mpp_frame_get_info_change(frame);
    const RK_U32 errinfo = mpp_frame_get_errinfo(frame);
    const RK_U32 discard = mpp_frame_get_discard(frame);
    const RK_U32 eos = mpp_frame_get_eos(frame);
    const RK_S32 width = mpp_frame_get_width(frame);
    const RK_S32 height = mpp_frame_get_height(frame);
    const RK_S32 hor_stride = mpp_frame_get_hor_stride(frame);
    const RK_S32 ver_stride = mpp_frame_get_ver_stride(frame);
    const MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    MppBuffer buffer = mpp_frame_get_buffer(frame);

    std::ostringstream out;
    out << "MPP MJPEG frame status: info_change=" << info_change
        << " errinfo=" << errinfo
        << " discard=" << discard
        << " eos=" << eos
        << " size=" << width << "x" << height
        << " stride=" << hor_stride << "x" << ver_stride
        << " fmt=" << static_cast<int>(fmt)
        << " buffer=" << static_cast<void*>(buffer)
        << " fd=" << (buffer != nullptr ? mpp_buffer_get_fd(buffer) : -1);
    return out.str();
}

// 释放及销毁 MPP 相关资源
void close_mpp(MjpegDecoder::Impl& impl) {
    if (impl.output_pool != nullptr) {
        impl.output_pool->shutdown();
        impl.output_pool.reset();
    }
    if (impl.ctx != nullptr) {
        mpp_destroy(impl.ctx);
        impl.ctx = nullptr;
        impl.mpi = nullptr;
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

// 创建并初始化 MPP MJPEG 硬件解码器
bool open_mpp(MjpegDecoder::Impl& impl, int width, int height, std::string& error) {
    impl.mpp_init_attempted = true;
    MPP_RET ret = mpp_create(&impl.ctx, &impl.mpi);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_create(MJPEG decoder)", ret);
        close_mpp(impl);
        return false;
    }
    RK_U32 need_split = 0;
    ret = impl.mpi->control(impl.ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &need_split);
    if (ret != MPP_OK) {
        error = mpp_error("MPP_DEC_SET_PARSER_SPLIT_MODE(MJPEG)", ret);
        close_mpp(impl);
        return false;
    }
    // 指定为解码器，并解码 MJPEG 格式
    ret = mpp_init(impl.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
        error = mpp_error("mpp_init(MJPEG decoder)", ret);
        close_mpp(impl);
        return false;
    }

    // 设置目标输出格式为 YUV420SP (NV12)
    MppFrameFormat output_format = MPP_FMT_YUV420SP;
    ret = impl.mpi->control(impl.ctx, MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
    // 使用内建 DRM 内存组
    if (ret == MPP_OK) {
        ret = mpp_buffer_group_get_internal(&impl.group, MPP_BUFFER_TYPE_DRM);
    }
    if (ret != MPP_OK) {
        error = mpp_error("configure MPP MJPEG output", ret);
        close_mpp(impl);
        return false;
    }

    impl.configured_width = width;
    impl.configured_height = height;
    impl.output_pool = std::make_shared<MjpegOutputBufferPool>();
    impl.mpp_ready = true;
    return true;
}

// 利用 MPP 硬件加速对 MJPEG 进行单帧解码
bool decode_with_mpp(MjpegDecoder::Impl& impl,
                     const VideoFrame& input,
                     VideoFrame& output,
                     std::string& error,
                     bool allow_dma_output) {
    MPP_RET ret = MPP_OK;
    MppPacket packet = nullptr;
    MppBuffer packet_buffer = nullptr;
    MppFrame output_frame = nullptr;
    MppBuffer output_buffer = nullptr;
    const auto output_pool = impl.output_pool;
    std::size_t packet_size = input.data.size();

    if (input.data.empty()) {
        error = "MPP MJPEG decoder requires CPU-visible packet input";
        return false;
    }

    // 按需扩容输入缓冲区，USB 摄像头 DQBUF 已经是一张完整 JPEG 帧。
    if (input.data.size() > impl.input_capacity) {
        if (impl.input_buffer != nullptr) {
            mpp_buffer_put(impl.input_buffer);
            impl.input_buffer = nullptr;
            impl.input_capacity = 0;
        }
        impl.input_capacity =
            (input.data.size() + 4095U) & ~static_cast<std::size_t>(4095U);
        ret = mpp_buffer_get(impl.group, &impl.input_buffer, impl.input_capacity);
        if (ret != MPP_OK) {
            error = mpp_error("mpp_buffer_get(MJPEG input)", ret);
            impl.input_capacity = 0;
            return false;
        }
    }

    // 将压缩 JPEG 载荷拷贝到 MPP 输入缓冲区。C270 在当前 BSP 上压缩 DMA-BUF
    // EXT_DMA 输入会产生 errinfo=256，保留解码输出 DMA-BUF 作为主优化点。
    ret = mpp_buffer_write(impl.input_buffer,
                           0,
                           const_cast<std::uint8_t*>(input.data.data()),
                           input.data.size());
    if (ret == MPP_OK) {
        mpp_buffer_sync_end(impl.input_buffer);
        packet_buffer = impl.input_buffer;
        packet_size = input.data.size();
        ret = mpp_packet_init_with_buffer(&packet, packet_buffer);
    }

    if (ret != MPP_OK) {
        if (packet != nullptr) {
            mpp_packet_deinit(&packet);
        }
        error = mpp_error("prepare MPP MJPEG packet", ret);
        return false;
    }
    void* packet_ptr = mpp_buffer_get_ptr(packet_buffer);
    if (packet_ptr != nullptr) {
        mpp_packet_set_data(packet, packet_ptr);
        mpp_packet_set_pos(packet, packet_ptr);
    }
    mpp_packet_set_size(packet, packet_size);
    mpp_packet_set_length(packet, packet_size);
    mpp_packet_set_pts(packet, static_cast<RK_S64>(input.pts_us));

    ret = mpp_frame_init(&output_frame);
    if (ret == MPP_OK) {
        const std::size_t output_capacity =
            static_cast<std::size_t>((input.width + 15) & ~15) *
            static_cast<std::size_t>((input.height + 15) & ~15) * 4U;
        if (output_pool == nullptr ||
            !output_pool->acquire(impl.group, output_capacity, output_buffer, error)) {
            ret = MPP_NOK;
        }
    }
    if (ret == MPP_OK && output_buffer != nullptr) {
        mpp_frame_set_buffer(output_frame, output_buffer);
    }
    if (ret != MPP_OK) {
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        if (packet != nullptr) {
            mpp_packet_deinit(&packet);
        }
        if (error.empty()) {
            error = mpp_error("allocate MPP MJPEG output frame", ret);
        }
        return false;
    }

    // 轮询输入接口并将数据包任务塞入 MPP 队列
    MppTask input_task = nullptr;
    ret = impl.mpi->poll(impl.ctx, MPP_PORT_INPUT, static_cast<MppPollType>(1000));
    if (ret == MPP_OK) {
        ret = impl.mpi->dequeue(impl.ctx, MPP_PORT_INPUT, &input_task);
    }
    if (ret == MPP_OK && input_task != nullptr) {
        ret = mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, packet);
    }
    if (ret == MPP_OK) {
        ret = mpp_task_meta_set_frame(input_task, KEY_OUTPUT_FRAME, output_frame);
    }
    if (ret == MPP_OK) {
        ret = impl.mpi->enqueue(impl.ctx, MPP_PORT_INPUT, input_task);
    }
    if (ret != MPP_OK) {
        if (packet != nullptr) {
            mpp_packet_deinit(&packet);
        }
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        error = mpp_error("submit MPP MJPEG task", ret);
        return false;
    }

    // 轮询输出接口，获取解码后的原始图像帧
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

    // 清理并回收输入任务中的 packet 结构体
    MppTask returned_input_task = nullptr;
    MppPacket returned_packet = nullptr;
    MPP_RET input_ret = impl.mpi->poll(impl.ctx,
                                       MPP_PORT_INPUT,
                                       static_cast<MppPollType>(1000));
    if (input_ret == MPP_OK) {
        input_ret = impl.mpi->dequeue(impl.ctx, MPP_PORT_INPUT, &returned_input_task);
    }
    if (input_ret == MPP_OK && returned_input_task != nullptr) {
        input_ret =
            mpp_task_meta_get_packet(returned_input_task, KEY_INPUT_PACKET, &returned_packet);
        if (returned_packet != nullptr) {
            MppPacket released_packet = returned_packet;
            mpp_packet_deinit(&returned_packet);
            if (released_packet == packet) {
                packet = nullptr;
            }
        }
        const MPP_RET enqueue_ret =
            impl.mpi->enqueue(impl.ctx, MPP_PORT_INPUT, returned_input_task);
        if (input_ret == MPP_OK) {
            input_ret = enqueue_ret;
        }
    }
    if (ret != MPP_OK || input_ret != MPP_OK || decoded_frame == nullptr) {
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        close_mpp(impl);
        impl.mpp_init_attempted = false;
        error = "MPP MJPEG task did not return a decoded frame";
        return false;
    }
    if (mpp_frame_get_info_change(decoded_frame) != 0) {
        const std::string status = mpp_frame_status(decoded_frame);
        impl.mpi->control(impl.ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        error = status;
        return false;
    }

    // 校验硬件解码器的错误状态标识
    if (mpp_frame_get_errinfo(decoded_frame) != 0 ||
        mpp_frame_get_discard(decoded_frame) != 0 ||
        mpp_frame_get_eos(decoded_frame) != 0) {
        const std::string status = mpp_frame_status(decoded_frame);
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        error = status;
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
    const int decoded_fd = mpp_buffer_get_fd(decoded_buffer);
    if (width <= 0 || height <= 0 || horizontal_stride < width ||
        vertical_stride < height ||
        (decoded_ptr == nullptr && (!allow_dma_output || decoded_fd < 0))) {
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        error = "MPP MJPEG decoder returned invalid frame geometry";
        return false;
    }

    // 封装并返回 VideoFrame
    output = {};
    output.width = width;
    output.height = height;
    output.stride = horizontal_stride;
    output.vertical_stride = vertical_stride;
    output.format = "NV12";
    output.pts_us = input.pts_us;
    output.sequence = input.sequence;
    if (allow_dma_output) {
        if (decoded_fd >= 0) {
            const int frame_fd = dup(decoded_fd);
            if (frame_fd >= 0) {
                auto dma = std::make_shared<VideoDmaBuffer>();
                VideoDmaPlane plane;
                plane.fd = frame_fd;
                plane.offset = 0;
                plane.bytesused = nv12_size(horizontal_stride, vertical_stride);
                plane.length = mpp_buffer_get_size(decoded_buffer);
                dma->planes.push_back(plane);
                if (output_buffer != nullptr) {
                    MppBuffer held_output = output_buffer;
                    dma->release = [output_pool, held_output] {
                        if (output_pool != nullptr) {
                            output_pool->release(held_output);
                        } else {
                            mpp_buffer_put(held_output);
                        }
                    };
                    output_buffer = nullptr;
                }
                output.dma = std::move(dma);
                if (output_frame != nullptr) {
                    mpp_frame_deinit(&output_frame);
                }
                return true;
            }
        }
    }
    if (decoded_ptr == nullptr) {
        if (output_frame != nullptr) {
            mpp_frame_deinit(&output_frame);
        }
        release_output_buffer(output_pool, output_buffer);
        error = "MPP MJPEG decoder output is not CPU-visible";
        return false;
    }
    output.data.assign(decoded_ptr,
                       decoded_ptr + nv12_size(horizontal_stride, vertical_stride));
    if (output_frame != nullptr) {
        mpp_frame_deinit(&output_frame);
    }
    release_output_buffer(output_pool, output_buffer);
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

// 解码入口函数
bool MjpegDecoder::decode(const VideoFrame& input,
                          VideoFrame& output,
                          std::string& error,
                          bool allow_dma_output) {
    if (input.data.empty() || input.width <= 0 || input.height <= 0) {
        error = "invalid MJPEG frame";
        return false;
    }

#if defined(VISIONCAST_ENABLE_MPP)
    // 优先尝试硬件加速解码
    if (!impl_->mpp_init_attempted) {
        std::string init_error;
        open_mpp(*impl_, input.width, input.height, init_error);
        if (!impl_->mpp_ready) {
            error = init_error;
            VC_LOG_WARN("mjpeg-decoder", "MPP hardware MJPEG decoder init failed: " + init_error + ", falling back to software libjpeg decoding");
        } else {
            VC_LOG_INFO("mjpeg-decoder", "MPP hardware MJPEG decoder initialized successfully");
        }
    }
    if (impl_->mpp_ready) {
        impl_->last_frame_hardware = true;
        bool success = decode_with_mpp(*impl_, input, output, error, allow_dma_output);
        if (!success) {
            VC_LOG_WARN("mjpeg-decoder", "MPP hardware decode failed: " + error + ", falling back to software libjpeg decoding");
            impl_->last_frame_hardware = false;
            error.clear();
            return decode_with_libjpeg(input, output, error);
        }
        return true;
    }
#else
    static bool warned_once = false;
    if (!warned_once) {
        VC_LOG_WARN("mjpeg-decoder", "MPP hardware decoding is not compiled/enabled, using software libjpeg decoding");
        warned_once = true;
    }
#endif

    // 若硬件不可用或初始化失败，则 fallback 至 CPU 软件解码
    impl_->last_frame_hardware = false;
    error.clear();
    return decode_with_libjpeg(input, output, error);
}

bool MjpegDecoder::is_hardware_accelerated() const {
    return impl_->last_frame_hardware;
}

}  // namespace visioncast
