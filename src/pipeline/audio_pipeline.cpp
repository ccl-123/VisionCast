#include "pipeline/audio_pipeline.h"

#include <utility>

#include "common/log.h"
#include "common/time_utils.h"

namespace visioncast {

AudioPipeline::AudioPipeline(VisionCastConfig config,
                             std::shared_ptr<AvTransport> transport)
    : config_(std::move(config)),
      capture_(config_.audio),
      transport_(std::move(transport)) {}

AudioPipeline::~AudioPipeline() {
    stop();
}

bool AudioPipeline::start(std::string& error) {
    if (running_) {
        return true;
    }
    running_ = true;
    worker_ = std::thread(&AudioPipeline::worker_loop, this);
    if (!capture_.start([this](AudioFrame&& frame) {
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
        return false;
    }
    return true;
}

bool AudioPipeline::is_running() const {
    return running_.load() && capture_.is_running();
}

std::string AudioPipeline::error() const {
    const std::string capture_error = capture_.error();
    if (!capture_.is_running() && !capture_error.empty()) {
        return capture_error;
    }
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void AudioPipeline::stop() {
    running_ = false;
    capture_.stop();
    raw_queue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::size_t AudioPipeline::dropped_frames() const {
    return raw_queue_.dropped();
}

void AudioPipeline::worker_loop() {
    std::size_t frames = 0;
    std::size_t sent_bytes = 0;
    std::uint64_t last_log = monotonic_now_us();

    while (running_) {
        AudioFrame raw;
        if (!raw_queue_.pop(raw)) {
            break;
        }

        std::string error;
        const std::size_t input_bytes = raw.pcm.size();
        if (!transport_->send_audio(raw, error)) {
            VC_LOG_ERROR("audio-network", error);
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                error_ = error;
            }
            running_ = false;
            break;
        }
        sent_bytes += input_bytes;
        ++frames;

        const std::uint64_t now = monotonic_now_us();
        if (config_.debug.enable_perf_log && now - last_log >= 1000000ULL) {
            const double seconds = static_cast<double>(now - last_log) / 1000000.0;
            const double kbps = static_cast<double>(sent_bytes * 8U) / seconds / 1000.0;
            VC_LOG_INFO("AudioPipeline",
                        "frames=" + std::to_string(frames) +
                            " bitrate_kbps=" + std::to_string(kbps) +
                            " drop_frames=" + std::to_string(raw_queue_.dropped()) +
                            " queue_raw=" + std::to_string(raw_queue_.size()));
            frames = 0;
            sent_bytes = 0;
            last_log = now;
        }
    }
}

}  // namespace visioncast
