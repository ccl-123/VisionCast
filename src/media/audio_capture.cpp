#include "media/audio_capture.h"

#include <fstream>
#include <chrono>
#include <dlfcn.h>
#include <regex>
#include <sstream>
#include <utility>

#include "common/log.h"
#include "media/media_clock.h"

namespace visioncast {
namespace {

struct snd_pcm_t;
using snd_pcm_stream_t = int;
using snd_pcm_format_t = int;
using snd_pcm_access_t = int;
using snd_pcm_uframes_t = unsigned long;
using snd_pcm_sframes_t = long;

constexpr snd_pcm_stream_t kSndPcmStreamCapture = 1;
constexpr snd_pcm_format_t kSndPcmFormatS16Le = 2;
constexpr snd_pcm_access_t kSndPcmAccessRwInterleaved = 3;
constexpr int kEpipe = 32;

class AlsaRuntime {
public:
    using PcmOpen = int (*)(snd_pcm_t**, const char*, snd_pcm_stream_t, int);
    using PcmClose = int (*)(snd_pcm_t*);
    using PcmSetParams = int (*)(snd_pcm_t*,
                                 snd_pcm_format_t,
                                 snd_pcm_access_t,
                                 unsigned int,
                                 unsigned int,
                                 int,
                                 unsigned int);
    using PcmReadi = snd_pcm_sframes_t (*)(snd_pcm_t*, void*, snd_pcm_uframes_t);
    using PcmRecover = int (*)(snd_pcm_t*, int, int);
    using PcmPrepare = int (*)(snd_pcm_t*);
    using PcmDrop = int (*)(snd_pcm_t*);
    using StrError = const char* (*)(int);

    ~AlsaRuntime() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    bool load(std::string& error) {
        if (handle_ != nullptr) {
            return true;
        }
        handle_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            error = std::string("dlopen libasound.so.2: ") + dlerror();
            return false;
        }

        if (!load_symbol(open, "snd_pcm_open", error) ||
            !load_symbol(close, "snd_pcm_close", error) ||
            !load_symbol(set_params, "snd_pcm_set_params", error) ||
            !load_symbol(readi, "snd_pcm_readi", error) ||
            !load_symbol(recover, "snd_pcm_recover", error) ||
            !load_symbol(prepare, "snd_pcm_prepare", error) ||
            !load_symbol(drop, "snd_pcm_drop", error) ||
            !load_symbol(strerror, "snd_strerror", error)) {
            dlclose(handle_);
            handle_ = nullptr;
            return false;
        }
        return true;
    }

    PcmOpen open = nullptr;
    PcmClose close = nullptr;
    PcmSetParams set_params = nullptr;
    PcmReadi readi = nullptr;
    PcmRecover recover = nullptr;
    PcmPrepare prepare = nullptr;
    PcmDrop drop = nullptr;
    StrError strerror = nullptr;

private:
    template <typename Fn>
    bool load_symbol(Fn& fn, const char* name, std::string& error) {
        dlerror();
        void* symbol = dlsym(handle_, name);
        const char* dl_error = dlerror();
        if (dl_error != nullptr || symbol == nullptr) {
            error = std::string("dlsym ") + name + ": " + (dl_error != nullptr ? dl_error : "missing symbol");
            return false;
        }
        fn = reinterpret_cast<Fn>(symbol);
        return true;
    }

    void* handle_ = nullptr;
};

bool read_file(const std::string& path, std::string& content) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    content = buffer.str();
    return true;
}

bool parse_hw_device(const std::string& device, int& card, int& pcm_device) {
    static const std::regex pattern(R"(^hw:([0-9]+),([0-9]+)$)");
    std::smatch match;
    if (!std::regex_match(device, match, pattern)) {
        return false;
    }

    card = std::stoi(match[1].str());
    pcm_device = std::stoi(match[2].str());
    return true;
}

std::string pcm_prefix(int card, int pcm_device) {
    std::ostringstream out;
    if (card < 10) {
        out << '0';
    }
    out << card << '-';
    if (pcm_device < 10) {
        out << '0';
    }
    out << pcm_device << ':';
    return out.str();
}

std::string find_pcm_entry(const std::string& pcm_text, int card, int pcm_device) {
    std::istringstream input(pcm_text);
    std::string line;
    const std::string prefix = pcm_prefix(card, pcm_device);
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return line;
        }
    }
    return {};
}

std::string find_card_name(const std::string& cards_text, int card) {
    std::istringstream input(cards_text);
    std::string line;
    const std::string prefix = std::to_string(card) + " [";
    const std::string alt_prefix = " " + prefix;

    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) != 0 && line.rfind(alt_prefix, 0) != 0) {
            continue;
        }

        auto left = line.find('[');
        auto right = line.find(']');
        if (left != std::string::npos && right != std::string::npos && right > left + 1) {
            return line.substr(left + 1, right - left - 1);
        }
        return line;
    }

    return {};
}

std::string alsa_error(const AlsaRuntime& alsa, const std::string& prefix, int err) {
    return prefix + ": " + alsa.strerror(err);
}

}  // namespace

AudioCapture::AudioCapture(AudioConfig config)
    : config_(std::move(config)) {}

AudioCapture::~AudioCapture() {
    stop();
}

AudioProbeResult AudioCapture::probe() const {
    return probe_device(config_.device);
}

AudioProbeResult AudioCapture::probe_device(const std::string& device) {
    AudioProbeResult result;
    result.device = device;

    if (!parse_hw_device(device, result.card, result.pcm_device)) {
        result.error = "unsupported ALSA device syntax, expected hw:X,Y";
        return result;
    }

    std::string pcm_text;
    if (!read_file("/proc/asound/pcm", pcm_text)) {
        result.error = "failed to read /proc/asound/pcm";
        return result;
    }

    std::string cards_text;
    if (read_file("/proc/asound/cards", cards_text)) {
        result.card_name = find_card_name(cards_text, result.card);
    }

    result.pcm_entry = find_pcm_entry(pcm_text, result.card, result.pcm_device);
    if (result.pcm_entry.empty()) {
        result.error = "ALSA PCM entry not found in /proc/asound/pcm";
        return result;
    }

    if (result.pcm_entry.find("capture") == std::string::npos) {
        result.error = "ALSA PCM exists but does not advertise capture";
        return result;
    }

    result.available = true;
    return result;
}

bool AudioCapture::start(FrameCallback callback) {
    if (running_) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callback_ = std::move(callback);
        error_.clear();
        initialization_done_ = false;
        initialization_success_ = false;
    }
    running_ = true;
    thread_ = std::thread(&AudioCapture::capture_loop, this);

    std::unique_lock<std::mutex> lock(state_mutex_);
    const bool signaled = state_cv_.wait_for(lock, std::chrono::seconds(8), [this] {
        return initialization_done_;
    });
    const bool success = signaled && initialization_success_;
    if (!signaled) {
        error_ = "audio capture initialization timed out";
    }
    lock.unlock();
    if (!success) {
        stop();
    }
    return success;
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool AudioCapture::is_running() const {
    return running_.load();
}

std::string AudioCapture::error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return error_;
}

void AudioCapture::finish_initialization(bool success, const std::string& error) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (initialization_done_) {
        return;
    }
    initialization_done_ = true;
    initialization_success_ = success;
    if (!error.empty()) {
        error_ = error;
    }
    state_cv_.notify_all();
}

void AudioCapture::capture_loop() {
    AlsaRuntime alsa;
    std::string init_error;
    if (!alsa.load(init_error)) {
        finish_initialization(false, init_error);
        VC_LOG_ERROR("audio-capture", init_error);
        running_ = false;
        return;
    }

    snd_pcm_t* handle = nullptr;
    int ret = alsa.open(&handle, config_.device.c_str(), kSndPcmStreamCapture, 0);
    if (ret < 0) {
        init_error = alsa_error(alsa, "snd_pcm_open " + config_.device, ret);
        finish_initialization(false, init_error);
        VC_LOG_ERROR("audio-capture", init_error);
        running_ = false;
        return;
    }

    const unsigned int sample_rate = static_cast<unsigned int>(config_.sample_rate);
    const unsigned int channels = static_cast<unsigned int>(config_.channels);
    snd_pcm_uframes_t period_frames =
        static_cast<snd_pcm_uframes_t>((sample_rate * static_cast<unsigned int>(config_.frame_ms)) / 1000U);
    if (period_frames == 0) {
        period_frames = 960;
    }

    ret = alsa.set_params(handle,
                          kSndPcmFormatS16Le,
                          kSndPcmAccessRwInterleaved,
                          channels,
                          sample_rate,
                          1,
                          static_cast<unsigned int>(config_.frame_ms * 1000));
    if (ret < 0) {
        init_error = alsa_error(alsa, "snd_pcm_set_params", ret);
        finish_initialization(false, init_error);
        VC_LOG_ERROR("audio-capture", init_error);
        alsa.close(handle);
        running_ = false;
        return;
    }

    const std::size_t bytes_per_frame = static_cast<std::size_t>(channels) * sizeof(std::int16_t);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(period_frames) * bytes_per_frame);
    std::uint64_t sequence = 0;
    VC_LOG_INFO("audio-capture", "capturing from " + config_.device);
    finish_initialization(true);
    while (running_) {
        snd_pcm_sframes_t frames = alsa.readi(handle, buffer.data(), period_frames);
        const std::uint64_t capture_pts_us = MediaClock::now_us();
        if (frames == -kEpipe) {
            VC_LOG_WARN("audio-capture", "ALSA overrun, preparing device");
            alsa.prepare(handle);
            continue;
        }
        if (frames < 0) {
            ret = alsa.recover(handle, static_cast<int>(frames), 1);
            if (ret < 0) {
                const std::string read_error =
                    alsa_error(alsa, "snd_pcm_readi", static_cast<int>(frames));
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    error_ = read_error;
                }
                VC_LOG_ERROR("audio-capture", read_error);
                break;
            }
            continue;
        }
        if (frames == 0) {
            continue;
        }

        AudioFrame frame;
        frame.pts_us = capture_pts_us;
        frame.sample_rate = config_.sample_rate;
        frame.channels = config_.channels;
        frame.sequence = sequence++;
        const std::size_t bytes = static_cast<std::size_t>(frames) * bytes_per_frame;
        frame.pcm.assign(buffer.begin(), buffer.begin() + bytes);
        if (callback_) {
            callback_(std::move(frame));
        }
    }

    alsa.drop(handle);
    alsa.close(handle);
    running_ = false;
}

}  // namespace visioncast
