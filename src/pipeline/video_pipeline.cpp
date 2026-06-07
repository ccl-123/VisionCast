/**
 * @file video_pipeline.cpp
 * @brief VisionCast 视频流水线处理模块的实现
 * @details 实现了 VideoPipeline 类。主要流程包括：初始化及启动硬件加速编码器（MppEncoder）、
 *          基于 V4L2 开启采集工作线程、将采集到的原始帧送入缓冲队列。在工作线程循环中：
 *          利用 RgaProcessor 进行图像像素格式转换（转换为 NV12 供编码器使用），
 *          利用 MppEncoder 执行 H.264 视频硬件编码，投递 NV12 图像到 DisplayRenderer 进行本地显示渲染，
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
    
    // 1. 初始化并打开 Rockchip MPP H.264 硬件编码器
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
    
    // 4. 开启本地预览渲染器
    renderer_.start();
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
    std::uint64_t last_log = monotonic_now_us(); // 记录性能日志周期的时间戳

    while (running_) {
        VideoFrame raw;
        // 从阻塞队列中取出捕获的原始图像，若队列已关闭则退出
        if (!raw_queue_.pop(raw)) {
            break;
        }

        const std::uint64_t t_process_start = monotonic_now_us();
        VideoFrame nv12;
        std::string error;
        
        // A. 使用 Rockchip RGA 硬件加速器进行图像转换
        //    将原始图像（如 YUYV422 或 MJPEG）转换为 MPP 编码器所需的 NV12 (YUV420SP) 格式，并进行缩放或裁剪
        if (!processor_.process(raw, nv12, error)) {
            VC_LOG_WARN("rga", error);
            continue;
        }
        const std::uint64_t t_process_end = monotonic_now_us();
        
        EncodedPacket encoded;
        const std::uint64_t t_encode_start = monotonic_now_us();
        
        // B. 使用 Rockchip MPP 硬件编码器进行 H.264 压缩编码
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
        
        // C. 将 NV12 图像提交给本地视频渲染器，在当前屏幕上进行预览显示
        renderer_.submit(std::move(nv12));
        
        // 若当前帧编码后未产生输出（例如 MPP 内部缓存或属于非 I/P 关键帧输出），则继续下一帧
        if (encoded.data.empty()) {
            continue;
        }

        // D. 通过传输模块（RTP, RTMP 或 WebRTC WHIP）发送编码完成的视频封包
        if (!transport_->send_video(encoded, error)) {
            VC_LOG_ERROR("network", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error; // 记录网络传输错误
            }
            running_ = false;
            break;
        }
        sent_bytes += encoded.data.size();
        ++processed;

        // 周期性（每秒）统计并输出视频流水线性能指标：包括实际 FPS、RGA 耗时（标注硬件 HW 还是软件 SW 兜底）、MPP 编码耗时、发送码率等
        const std::uint64_t now = monotonic_now_us();
        if (config_.debug.enable_perf_log && now - last_log >= 1000000ULL) {
            const double seconds = static_cast<double>(now - last_log) / 1000000.0;
            const double fps = static_cast<double>(processed) / seconds;
            const double kbps = static_cast<double>(sent_bytes * 8U) / seconds / 1000.0;
            const double rga_ms = static_cast<double>(t_process_end - t_process_start) / 1000.0;
            const double mpp_ms = static_cast<double>(t_encode_end - t_encode_start) / 1000.0;
            const std::string rga_mode = processor_.is_hardware_accelerated() ? "HW" : "SW";
            const double avg_pack_bytes = processed > 0 ? (static_cast<double>(sent_bytes) / processed) : 0.0;

            std::ostringstream log_str;
            log_str << std::fixed << std::setprecision(2)
                    << "fps=" << fps
                    << " rga_ms=" << rga_ms << " (" << rga_mode << ")"
                    << " mpp_ms=" << mpp_ms << " (HW)"
                    << " bitrate_kbps=" << kbps
                    << " avg_bytes=" << avg_pack_bytes
                    << " drop_frames=" << raw_queue_.dropped()
                    << " queue_raw=" << raw_queue_.size();
            VC_LOG_INFO("VideoPipeline", log_str.str());

            processed = 0;
            sent_bytes = 0;
            last_log = now;
        }
    }
}

}  // namespace visioncast
