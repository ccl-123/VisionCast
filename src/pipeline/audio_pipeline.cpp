/**
 * @file audio_pipeline.cpp
 * @brief VisionCast 音频流水线处理模块的实现
 * @details 实现了 AudioPipeline 类，提供音频捕获的回调注册、缓存队列管理、
 *          工作线程的启停以及向网络传输模块（AvTransport）的音频数据推送。
 */

#include "pipeline/audio_pipeline.h"

#include <iomanip>
#include <sstream>
#include <utility>

#include "common/log.h"
#include "common/time_utils.h"

namespace visioncast {

// 构造函数：初始化配置，根据音频配置初始化 capture_，并绑定网络传输层
AudioPipeline::AudioPipeline(VisionCastConfig config,
                             std::shared_ptr<AvTransport> transport)
    : config_(std::move(config)),
      capture_(config_.audio),
      transport_(std::move(transport)) {}

AudioPipeline::~AudioPipeline() {
    stop(); // 析构时自动停止流水线，释放线程资源
}

bool AudioPipeline::start(std::string& error) {
    if (running_) {
        return true; // 已在运行中，直接返回
    }
    running_ = true;
    
    // 启动音频数据包的网络推送工作线程
    worker_ = std::thread(&AudioPipeline::worker_loop, this);
    
    // 启动基于 ALSA 库的音频数据采集，并注册数据回调 Lambda 表达式
    if (!capture_.start([this](AudioFrame&& frame) {
        // 当 ALSA 采集到新的原始 PCM 音频帧后，将其推入阻塞缓冲队列
        // 若队列已满，则自动丢弃最老的一帧数据，以维持流水线低延迟
        raw_queue_.push_drop_oldest(std::move(frame));
    })) {
        // 音频采集启动失败，获取底层 ALSA 错误信息并同步保存
        error = capture_.error();
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_ = error;
        }
        running_ = false;
        raw_queue_.close();
        if (worker_.joinable()) {
            worker_.join();
        }
        return false;
    }
    return true;
}

bool AudioPipeline::is_running() const {
    // 流水线状态需要同时满足控制标志 running_ 和底层 capture_ 都在运行
    return running_.load() && capture_.is_running();
}

std::string AudioPipeline::error() const {
    // 优先返回底层采集模块的错误，若采集已停止且有错
    const std::string capture_error = capture_.error();
    if (!capture_.is_running() && !capture_error.empty()) {
        return capture_error;
    }
    // 线程安全地读取 pipeline 自身的网络发送错误或启动错误
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void AudioPipeline::stop() {
    running_ = false;       // 触发工作线程退出
    capture_.stop();        // 停止 ALSA 音频采集
    raw_queue_.close();     // 唤醒并关闭阻塞队列
    if (worker_.joinable()) {
        worker_.join();     // 等待推送工作线程退出
    }
}

std::size_t AudioPipeline::dropped_frames() const {
    return raw_queue_.dropped(); // 获取被阻塞队列主动丢弃的帧数计数
}

void AudioPipeline::worker_loop() {
    std::size_t frames = 0;
    std::size_t sent_bytes = 0;
    std::uint64_t total_queue_us = 0;
    std::uint64_t total_send_us = 0;
    std::uint64_t total_latency_us = 0;
    std::size_t queue_samples = 0;
    std::size_t latency_samples = 0;
    std::uint64_t last_log = monotonic_now_us(); // 记录上一次打印日志的单调时间戳

    while (running_) {
        AudioFrame raw;
        // 从阻塞队列中拉取采集到的原始音频帧，若队列已关闭则退出循环
        if (!raw_queue_.pop(raw)) {
            break;
        }

        std::string error;
        const std::size_t input_bytes = raw.pcm.size();
        const std::uint64_t t_send_start = monotonic_now_us();
        std::uint64_t frame_queue_us = 0;
        bool frame_has_queue_sample = false;
        if (raw.pts_us > 0 && t_send_start >= raw.pts_us) {
            frame_queue_us = t_send_start - raw.pts_us;
            frame_has_queue_sample = true;
        }
        
        // 通过传输层（如 RTP/RTSP 封装）发送音频数据帧
        if (!transport_->send_audio(raw, error)) {
            VC_LOG_ERROR("audio-network", error);
            {
                // 记录发送失败的错误，标志流水线发生不可恢复异常
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error;
            }
            running_ = false;
            break;
        }
        const std::uint64_t send_done_us = monotonic_now_us();
        const std::uint64_t frame_send_us = send_done_us - t_send_start;
        sent_bytes += input_bytes;
        if (frame_has_queue_sample) {
            total_queue_us += frame_queue_us;
            ++queue_samples;
        }
        total_send_us += frame_send_us;
        if (raw.pts_us > 0 && send_done_us >= raw.pts_us) {
            total_latency_us += send_done_us - raw.pts_us;
            ++latency_samples;
        }
        ++frames;

        // 周期性（每秒）计算音频码率、帧率、丢包队列等统计指标
        const std::uint64_t now = send_done_us;
        if (config_.debug.enable_perf_log && now - last_log >= 1000000ULL) {
            const double seconds = static_cast<double>(now - last_log) / 1000000.0;
            const double kbps = static_cast<double>(sent_bytes * 8U) / seconds / 1000.0;
            const double avg_pack_kb =
                frames > 0 ? (static_cast<double>(sent_bytes) / frames / 1024.0) : 0.0;
            const double queue_ms =
                queue_samples > 0
                    ? (static_cast<double>(total_queue_us) / queue_samples / 1000.0)
                    : 0.0;
            const double send_ms =
                frames > 0 ? (static_cast<double>(total_send_us) / frames / 1000.0) : 0.0;
            const double avg_latency_ms =
                latency_samples > 0
                    ? (static_cast<double>(total_latency_us) / latency_samples / 1000.0)
                    : 0.0;

            std::ostringstream log_str;
            log_str << std::fixed << std::setprecision(2)
                    << "\n  概览: 帧数=" << frames
                    << " 码率=" << kbps << "kbps"
                    << " 平均=" << avg_pack_kb << "KB"
                    << "\n  耗时: 排队=" << queue_ms << "ms"
                    << " 发送=" << send_ms << "ms"
                    << " 总=" << avg_latency_ms << "ms"
                    << "\n  状态: 丢帧=" << raw_queue_.dropped()
                    << " 原始队列=" << raw_queue_.size();
            VC_LOG_INFO("音频流水线", log_str.str());

            frames = 0;
            sent_bytes = 0;
            total_queue_us = 0;
            total_send_us = 0;
            total_latency_us = 0;
            queue_samples = 0;
            latency_samples = 0;
            last_log = now;
        }
    }
}

}  // namespace visioncast
