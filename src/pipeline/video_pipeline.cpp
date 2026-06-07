#include "pipeline/video_pipeline.h"

#include <chrono>
#include <utility>

#include "common/log.h"
#include "common/time_utils.h"

namespace visioncast {

VideoPipeline::VideoPipeline(VisionCastConfig config,
                             std::shared_ptr<AvTransport> transport)
    : config_(std::move(config)),
      capture_(config_.video),
      processor_(config_.video.width, config_.video.height),
      encoder_(config_.video, config_.encoder),
      transport_(std::move(transport)),
      renderer_("VisionCast Local Preview") {}

VideoPipeline::~VideoPipeline() {
    stop();
}

bool VideoPipeline::start(std::string& error) {
    if (running_) {
        return true;
    }
    if (!encoder_.open(error)) {
        return false;
    }
    running_ = true;
    worker_ = std::thread(&VideoPipeline::worker_loop, this);
    if (!capture_.start([this](VideoFrame&& frame) {
        raw_queue_.push_drop_oldest(std::move(frame));
    })) {
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
        encoder_.close();
        return false;
    }
    renderer_.start();
    return true;
}

bool VideoPipeline::is_running() const {
    return running_.load() && capture_.is_running();
}

std::string VideoPipeline::error() const {
    const std::string capture_error = capture_.error();
    if (!capture_.is_running() && !capture_error.empty()) {
        return capture_error;
    }
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void VideoPipeline::stop() {
    running_ = false;
    capture_.stop();
    raw_queue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    renderer_.stop();
    encoder_.close();
}

std::size_t VideoPipeline::dropped_frames() const {
    return raw_queue_.dropped();
}

void VideoPipeline::worker_loop() {
    std::size_t processed = 0;
    std::size_t sent_bytes = 0;
    std::uint64_t last_log = monotonic_now_us();

    while (running_) {
        VideoFrame raw;
        if (!raw_queue_.pop(raw)) {
            break;
        }

        const std::uint64_t t_process_start = monotonic_now_us();
        VideoFrame nv12;
        std::string error;
        if (!processor_.process(raw, nv12, error)) {
            VC_LOG_WARN("rga", error);
            continue;
        }
        const std::uint64_t t_process_end = monotonic_now_us();
        EncodedPacket encoded;
        const std::uint64_t t_encode_start = monotonic_now_us();
        if (!encoder_.encode(nv12, encoded, error)) {
            VC_LOG_ERROR("mpp", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error;
            }
            running_ = false;
            break;
        }
        const std::uint64_t t_encode_end = monotonic_now_us();
        renderer_.submit(std::move(nv12));
        if (encoded.data.empty()) {
            continue;
        }

        if (!transport_->send_video(encoded, error)) {
            VC_LOG_ERROR("network", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error;
            }
            running_ = false;
            break;
        }
        sent_bytes += encoded.data.size();
        ++processed;

        const std::uint64_t now = monotonic_now_us();
        if (config_.debug.enable_perf_log && now - last_log >= 1000000ULL) {
            const double seconds = static_cast<double>(now - last_log) / 1000000.0;
            const double fps = static_cast<double>(processed) / seconds;
            const double kbps = static_cast<double>(sent_bytes * 8U) / seconds / 1000.0;
            VC_LOG_INFO("VideoPipeline",
                        "fps=" + std::to_string(fps) +
                            " rga_us=" + std::to_string(t_process_end - t_process_start) +
                            " mpp_us=" + std::to_string(t_encode_end - t_encode_start) +
                            " bitrate_kbps=" + std::to_string(kbps) +
                            " drop_frames=" + std::to_string(raw_queue_.dropped()) +
                            " queue_raw=" + std::to_string(raw_queue_.size()));
            processed = 0;
            sent_bytes = 0;
            last_log = now;
        }
    }
}

}  // namespace visioncast
