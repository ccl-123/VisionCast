/**
 * @file video_pipeline.cpp
 * @brief VisionCast 视频流水线处理模块的实现
 * @details 实现了 VideoPipeline 类。主要流程包括：初始化及启动硬件加速编码器（MppEncoder）、
 *          基于 V4L2 开启采集工作线程、将采集到的原始帧送入缓冲队列。在工作线程循环中：
 *          利用 RgaProcessor 进行图像像素格式转换（转换为 NV12 供编码器使用），
 *          利用 MppEncoder 执行 H.264/H.265 视频硬件编码，按配置投递 NV12 图像到 DisplayRenderer 进行本地显示渲染，
 *          最后通过 AvTransport 发送编码后的视频数据。
 */

#include "pipeline/video_pipeline.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

#include "common/log.h"
#include "common/time_utils.h"

namespace visioncast {

// 构造函数：初始化各个子组件，包含基于 V4L2 的采集器、RGA 处理器、MPP 编码器、传输模块及本地显示渲染器
VideoPipeline::VideoPipeline(VisionCastConfig config,
                             std::shared_ptr<AvTransport> transport)
    : config_(std::move(config)),
      capture_(config_.video),
      processor_(config_.video.width, config_.video.height),
      encoder_(config_.video, config_.encoder),
      transport_(std::move(transport)),
      renderer_("VisionCast Local Preview") {}

VideoPipeline::~VideoPipeline() {
    stop(); // 析构时安全停止流水线
}

bool VideoPipeline::start(std::string& error) {
    if (running_) {
        return true; // 已在运行，直接返回
    }
    
    // 1. 初始化并打开 Rockchip MPP 硬件编码器
    if (!encoder_.open(error)) {
        return false;
    }
    running_ = true;
    
    // 2. 启动图像后处理、编码和网络推送的工作线程
    worker_ = std::thread(&VideoPipeline::worker_loop, this);
    
    // 3. 启动基于 V4L2 的视频采集，注册帧回调
    if (!capture_.start([this](VideoFrame&& frame) {
        // 将摄像头采集的原始图像数据（如 YUYV 格式）送入缓冲阻塞队列
        // 队列长度限制为 2 以保证推流的超低延迟（若满了则丢弃最老的一帧）
        raw_queue_.push_drop_oldest(std::move(frame));
    })) {
        error = capture_.error(); // 采集模块启动失败，提取 V4L2 错误
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_ = error;
        }
        running_ = false;
        raw_queue_.close();
        if (worker_.joinable()) {
            worker_.join();
        }
        encoder_.close(); // 回滚：关闭 MPP 编码器
        return false;
    }
    
    // 4. 按配置开启本地预览渲染器；默认关闭以避免纯推流场景持有额外显示帧
    if (config_.debug.enable_preview) {
        renderer_.start();
    } else {
        VC_LOG_INFO("display", "desktop preview disabled by debug.enable_preview=false");
    }
    return true;
}

bool VideoPipeline::is_running() const {
    return running_.load() && capture_.is_running();
}

std::string VideoPipeline::error() const {
    // 优先返回底层的 V4L2 采集错误
    const std::string capture_error = capture_.error();
    if (!capture_.is_running() && !capture_error.empty()) {
        return capture_error;
    }
    // 线程安全地读取并返回流水线工作线程中的编码或发送错误
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void VideoPipeline::stop() {
    running_ = false;       // 终止工作线程循环
    capture_.stop();        // 停止 V4L2 视频采集
    raw_queue_.close();     // 关闭并唤醒阻塞队列
    if (worker_.joinable()) {
        worker_.join();     // 等待视频处理线程汇合
    }
    renderer_.stop();       // 停止本地显示渲染
    encoder_.close();       // 关闭 MPP 硬件编码器并释放上下文
}

std::size_t VideoPipeline::dropped_frames() const {
    return raw_queue_.dropped(); // 返回由于消费不及时而被主动丢弃的视频帧数
}

void VideoPipeline::worker_loop() {
    std::size_t processed = 0;
    std::size_t sent_bytes = 0;
    std::size_t mpp_dma_frames = 0;
    std::size_t mpp_copy_frames = 0;
    std::size_t rga_fd_frames = 0;
    std::size_t rga_bypass_dma_frames = 0;
    std::size_t rga_cpu_or_va_frames = 0;
    std::size_t mjpeg_frames = 0;
    std::size_t mjpeg_hw_frames = 0;
    std::size_t mjpeg_sw_frames = 0;
    std::size_t mjpeg_dma_output_frames = 0;
    std::size_t process_failures = 0;
    std::size_t encode_empty_frames = 0;
    std::uint64_t total_queue_us = 0;
    std::uint64_t total_process_us = 0;
    std::uint64_t total_mjpeg_decode_us = 0;
    std::uint64_t total_image_process_us = 0;
    std::uint64_t total_encode_us = 0;
    std::uint64_t total_transport_us = 0;
    std::uint64_t total_latency_us = 0;
    std::size_t queue_samples = 0;
    std::size_t latency_samples = 0;
    std::string last_process_error;
    std::uint64_t last_log = monotonic_now_us(); // 记录性能日志周期的时间戳

    while (running_) {
        VideoFrame raw;
        // 从阻塞队列中取出捕获的原始图像，若队列已关闭则退出
        if (!raw_queue_.pop(raw)) {
            break;
        }
        const std::uint64_t raw_pts_us = raw.pts_us;

        const std::uint64_t t_process_start = monotonic_now_us();
        std::uint64_t frame_queue_us = 0;
        bool frame_has_queue_sample = false;
        if (raw_pts_us > 0 && t_process_start >= raw_pts_us) {
            frame_queue_us = t_process_start - raw_pts_us;
            frame_has_queue_sample = true;
        }
        VideoFrame nv12;
        std::string error;
        
        // A. 使用 Rockchip RGA 硬件加速器进行图像转换
        //    将原始图像（如 YUYV422 或 MJPEG）转换为 MPP 编码器所需的 NV12 (YUV420SP) 格式，并进行缩放或裁剪
        if (!processor_.process(raw, nv12, error)) {
            ++process_failures;
            last_process_error = error;
            continue;
        }
        const std::uint64_t t_process_end = monotonic_now_us();
        const std::uint64_t frame_process_us = t_process_end - t_process_start;
        const std::uint64_t frame_mjpeg_decode_us = processor_.last_mjpeg_decode_us();
        const std::uint64_t frame_image_process_us =
            frame_process_us >= frame_mjpeg_decode_us
                ? frame_process_us - frame_mjpeg_decode_us
                : 0;
        const bool frame_mjpeg = processor_.last_mjpeg_input();
        const bool frame_mjpeg_hw = processor_.last_mjpeg_hardware();
        const bool frame_mjpeg_dma_output = processor_.last_mjpeg_dma_output();
        raw = {};
        
        EncodedPacket encoded;
        const std::uint64_t t_encode_start = monotonic_now_us();
        
        // B. 使用 Rockchip MPP 硬件编码器进行 H.264/H.265 压缩编码
        if (!encoder_.encode(nv12, encoded, error)) {
            VC_LOG_ERROR("mpp", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error; // 保存编码错误，并触发流水线停止
            }
            running_ = false;
            break;
        }
        const std::uint64_t t_encode_end = monotonic_now_us();
        const std::uint64_t frame_encode_us = t_encode_end - t_encode_start;
        const std::string frame_rga_mode = processor_.last_mode();
        const bool frame_mpp_dma = encoder_.last_input_dma();
        
        // C. 按配置提交本地预览。DisplayRenderer 支持 CPU 可见帧和 DMA-BUF fd 输入。
        if (config_.debug.enable_preview) {
            renderer_.submit(std::move(nv12));
        } else {
            nv12 = {};
        }
        
        // 若当前帧编码后未产生输出（例如 MPP 内部缓存或属于非 I/P 关键帧输出），则继续下一帧
        if (encoded.data.empty()) {
            ++encode_empty_frames;
            continue;
        }

        // D. 通过传输模块（RTP, RTMP 或 WebRTC WHIP）发送编码完成的视频封包
        const std::uint64_t t_send_start = monotonic_now_us();
        if (!transport_->send_video(encoded, error)) {
            VC_LOG_ERROR("network", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error; // 记录网络传输错误
            }
            running_ = false;
            break;
        }
        const std::uint64_t send_done_us = monotonic_now_us();
        const std::uint64_t frame_transport_us = send_done_us - t_send_start;
        sent_bytes += encoded.data.size();
        if (frame_has_queue_sample) {
            total_queue_us += frame_queue_us;
            ++queue_samples;
        }
        total_process_us += frame_process_us;
        total_mjpeg_decode_us += frame_mjpeg_decode_us;
        total_image_process_us += frame_image_process_us;
        total_encode_us += frame_encode_us;
        total_transport_us += frame_transport_us;
        if (frame_mpp_dma) {
            ++mpp_dma_frames;
        } else {
            ++mpp_copy_frames;
        }
        if (frame_mjpeg) {
            ++mjpeg_frames;
            if (frame_mjpeg_hw) {
                ++mjpeg_hw_frames;
            } else {
                ++mjpeg_sw_frames;
            }
            if (frame_mjpeg_dma_output) {
                ++mjpeg_dma_output_frames;
            }
        }
        if (frame_rga_mode == "RGA-FD") {
            ++rga_fd_frames;
        } else if (frame_rga_mode == "BYPASS-DMA") {
            ++rga_bypass_dma_frames;
        } else {
            ++rga_cpu_or_va_frames;
        }
        if (raw_pts_us > 0 && send_done_us >= raw_pts_us) {
            total_latency_us += send_done_us - raw_pts_us;
            ++latency_samples;
        }
        ++processed;

        // 周期性统计视频流水线性能：前处理、编码、发送码率和端到端处理延迟。
        const std::uint64_t now = send_done_us;
        if (config_.debug.enable_perf_log && now - last_log >= 1000000ULL) {
            const double seconds = static_cast<double>(now - last_log) / 1000000.0;
            const double fps = static_cast<double>(processed) / seconds;
            const double kbps = static_cast<double>(sent_bytes * 8U) / seconds / 1000.0;
            const double queue_ms =
                queue_samples > 0
                    ? (static_cast<double>(total_queue_us) / queue_samples / 1000.0)
                    : 0.0;
            const double process_ms =
                processed > 0 ? (static_cast<double>(total_process_us) / processed / 1000.0) : 0.0;
            const double mjpeg_decode_ms =
                mjpeg_frames > 0
                    ? (static_cast<double>(total_mjpeg_decode_us) / mjpeg_frames / 1000.0)
                    : 0.0;
            const double image_process_ms =
                processed > 0
                    ? (static_cast<double>(total_image_process_us) / processed / 1000.0)
                    : 0.0;
            const double encode_ms =
                processed > 0 ? (static_cast<double>(total_encode_us) / processed / 1000.0) : 0.0;
            const double transport_ms =
                processed > 0 ? (static_cast<double>(total_transport_us) / processed / 1000.0) : 0.0;
            const double avg_pack_kb =
                processed > 0 ? (static_cast<double>(sent_bytes) / processed / 1024.0) : 0.0;
            const double avg_latency_ms =
                latency_samples > 0
                    ? (static_cast<double>(total_latency_us) / latency_samples / 1000.0)
                    : 0.0;

            std::ostringstream log_str;
            log_str << std::fixed << std::setprecision(2)
                    << "\n  概览: 窗口帧=" << processed
                    << " 帧率=" << fps << "fps"
                    << " 码率=" << kbps << "kbps"
                    << " 平均=" << avg_pack_kb << "KB"
                    << "\n  路径: 前处理(fd/直通/拷贝)=" << rga_fd_frames
                    << "/" << rga_bypass_dma_frames
                    << "/" << rga_cpu_or_va_frames
                    << " MPP输入(DMA/COPY)=" << mpp_dma_frames
                    << "/" << mpp_copy_frames
                    << " MJPEG(hw/sw/dma输出)=" << mjpeg_hw_frames
                    << "/" << mjpeg_sw_frames
                    << "/" << mjpeg_dma_output_frames
                    << "\n  耗时: 排队=" << queue_ms << "ms"
                    << " 前处理=" << process_ms << "ms"
                    << " MJPEG=" << mjpeg_decode_ms << "ms"
                    << " 图像处理=" << image_process_ms << "ms"
                    << " 编码=" << encode_ms << "ms"
                    << " 传输=" << transport_ms << "ms"
                    << " 总=" << avg_latency_ms << "ms"
                    << "\n  状态: 处理失败=" << process_failures
                    << " 编码无输出=" << encode_empty_frames
                    << " 丢帧=" << raw_queue_.dropped()
                    << " 原始队列=" << raw_queue_.size();
            if (process_failures > 0 && !last_process_error.empty()) {
                log_str << "\n  最后错误: " << last_process_error;
            }
            VC_LOG_INFO("视频流水线", log_str.str());

            processed = 0;
            sent_bytes = 0;
            mpp_dma_frames = 0;
            mpp_copy_frames = 0;
            rga_fd_frames = 0;
            rga_bypass_dma_frames = 0;
            rga_cpu_or_va_frames = 0;
            mjpeg_frames = 0;
            mjpeg_hw_frames = 0;
            mjpeg_sw_frames = 0;
            mjpeg_dma_output_frames = 0;
            process_failures = 0;
            encode_empty_frames = 0;
            total_queue_us = 0;
            total_process_us = 0;
            total_mjpeg_decode_us = 0;
            total_image_process_us = 0;
            total_encode_us = 0;
            total_transport_us = 0;
            total_latency_us = 0;
            queue_samples = 0;
            latency_samples = 0;
            last_process_error.clear();
            last_log = now;
        }
    }
}

}  // namespace visioncast
