/**
 * @file audio_capture.cpp
 * @brief VisionCast 音频采集实现文件
 * @details 实现了基于 ALSA (Advanced Linux Sound Architecture) 的音频数据捕获。
 *          利用运行时动态链接（dlopen/dlsym）方式加载 `libasound.so.2`，规避编译期硬链接依赖。
 *          支持独立配置采集声道与输出声道，并按需执行降混或声道复制。
 */

#include "media/audio_capture.h"

#include <cstdint>
#include <fstream>
#include <chrono>
#include <dlfcn.h>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "media/media_clock.h"

namespace visioncast {
namespace {

// ALSA 库中的不透明 PCM 句柄结构体前置声明
struct snd_pcm_t;
using snd_pcm_stream_t = int;
using snd_pcm_format_t = int;
using snd_pcm_access_t = int;
using snd_pcm_uframes_t = unsigned long;
using snd_pcm_sframes_t = long;

// ALSA 常量定义
constexpr snd_pcm_stream_t kSndPcmStreamCapture = 1;      // 录音/捕获流
constexpr snd_pcm_format_t kSndPcmFormatS16Le = 2;        // 16位有符号小端格式
constexpr snd_pcm_access_t kSndPcmAccessRwInterleaved = 3;// 交织读写模式
constexpr int kEpipe = 32;                                // 缓冲区越界错误号 (EPIPE / Overrun)

/**
 * @class AlsaRuntime
 * @brief 动态加载 ALSA 运行库的包装类，规避直接强链接 libasound
 */
class AlsaRuntime {
public:
    // ALSA 函数指针类型定义
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

    // 析构函数，关闭动态库句柄
    ~AlsaRuntime() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
    }

    // 动态加载共享库并绑定相关符号接口
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

    // 导出的 ALSA 接口函数指针成员
    PcmOpen open = nullptr;
    PcmClose close = nullptr;
    PcmSetParams set_params = nullptr;
    PcmReadi readi = nullptr;
    PcmRecover recover = nullptr;
    PcmPrepare prepare = nullptr;
    PcmDrop drop = nullptr;
    StrError strerror = nullptr;

private:
    // 模板函数，用于动态提取动态库符号并类型转换
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

    void* handle_ = nullptr; // dlopen 获取的动态库句柄
};

// 辅助读取文本文件
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

// 解析 "hw:X,Y" 格式的设备名称，提取 card 声卡号与 pcm_device 设备号
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

// 生成形如 "01-00:" 的 PCM 匹配前缀
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

// 从 proc 文本中匹配指定声卡设备的 PCM 条目
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

// 从 proc 文本中寻找指定声卡的真实显示名称
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

// 组合 ALSA 出错信息
std::string alsa_error(const AlsaRuntime& alsa, const std::string& prefix, int err) {
    return prefix + ": " + alsa.strerror(err);
}

// 以小端格式读取16位 PCM 样本
std::int16_t read_s16le(const std::uint8_t* data) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8));
}

// 将16位采样值以小端格式写入缓冲区
void write_s16le(std::int16_t sample, std::uint8_t* data) {
    const auto value = static_cast<std::uint16_t>(sample);
    data[0] = static_cast<std::uint8_t>(value & 0xFFU);
    data[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

// 进行多通道数据处理，包括降混（如 Stereo 转 Mono）或映射
std::vector<std::uint8_t> convert_interleaved_s16(const std::uint8_t* input,
                                                  std::size_t frames,
                                                  unsigned int capture_channels,
                                                  unsigned int output_channels) {
    if (capture_channels == output_channels) {
        return std::vector<std::uint8_t>(
            input, input + frames * capture_channels * sizeof(std::int16_t));
    }

    std::vector<std::uint8_t> output(
        frames * output_channels * sizeof(std::int16_t));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        // 双声道降混单声道算法：将多通道样本求和取均值
        if (output_channels == 1 && capture_channels > 1) {
            std::int32_t sum = 0;
            for (unsigned int channel = 0; channel < capture_channels; ++channel) {
                const std::size_t offset =
                    (frame * capture_channels + channel) * sizeof(std::int16_t);
                sum += read_s16le(input + offset);
            }
            const auto mixed =
                static_cast<std::int16_t>(sum / static_cast<std::int32_t>(capture_channels));
            write_s16le(mixed, output.data() + frame * sizeof(std::int16_t));
            continue;
        }

        // 通道扩增或复制逻辑
        for (unsigned int channel = 0; channel < output_channels; ++channel) {
            const unsigned int source_channel =
                channel < capture_channels ? channel : capture_channels - 1;
            const std::size_t input_offset =
                (frame * capture_channels + source_channel) * sizeof(std::int16_t);
            const std::size_t output_offset =
                (frame * output_channels + channel) * sizeof(std::int16_t);
            write_s16le(read_s16le(input + input_offset), output.data() + output_offset);
        }
    }
    return output;
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

// 探测声卡设备，检测 /proc/asound 下的节点信息
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

// 启动音频采集，创建采集线程并同步等待初始化状态
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

    // 等待后台采集线程中的 ALSA 初始化结果，最长等待 8 秒
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

// 停止采集，安全回收线程
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

// 辅助更新初始化状态，唤醒等待在 start() 中的主线程
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

// 音频采集主线程循环
void AudioCapture::capture_loop() {
    AlsaRuntime alsa;
    std::string init_error;
    // 1. 动态加载 libasound
    if (!alsa.load(init_error)) {
        finish_initialization(false, init_error);
        VC_LOG_ERROR("audio-capture", init_error);
        running_ = false;
        return;
    }

    // 2. 打开 PCM 录音设备
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
    const unsigned int output_channels =
        static_cast<unsigned int>(config_.channels > 0 ? config_.channels : 1);
    unsigned int capture_channels =
        static_cast<unsigned int>(config_.capture_channels > 0
                                      ? config_.capture_channels
                                      : config_.channels);
    if (capture_channels == 0) {
        capture_channels = output_channels;
    }
    snd_pcm_uframes_t period_frames =
        static_cast<snd_pcm_uframes_t>((sample_rate * static_cast<unsigned int>(config_.frame_ms)) / 1000U);
    if (period_frames == 0) {
        period_frames = 960;
    }

    // 3. 配置音频参数：采样格式为 S16_LE，交织读写模式
    ret = alsa.set_params(handle,
                          kSndPcmFormatS16Le,
                          kSndPcmAccessRwInterleaved,
                          capture_channels,
                          sample_rate,
                          1,
                          static_cast<unsigned int>(config_.frame_ms * 1000));
    // 如果请求 mono 被声卡拒绝且输出需要 mono，尝试以 stereo 启动捕获并在软件层降混
    if (ret < 0 && output_channels == 1) {
        capture_channels = 2;
        ret = alsa.set_params(handle,
                              kSndPcmFormatS16Le,
                              kSndPcmAccessRwInterleaved,
                              capture_channels,
                              sample_rate,
                              1,
                              static_cast<unsigned int>(config_.frame_ms * 1000));
        if (ret >= 0) {
            VC_LOG_WARN("audio-capture",
                        "ALSA device rejected mono capture; capturing stereo and downmixing to mono");
        }
    }
    if (ret < 0) {
        init_error = alsa_error(alsa, "snd_pcm_set_params", ret);
        finish_initialization(false, init_error);
        VC_LOG_ERROR("audio-capture", init_error);
        alsa.close(handle);
        running_ = false;
        return;
    }

    const std::size_t bytes_per_frame = static_cast<std::size_t>(capture_channels) * sizeof(std::int16_t);
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(period_frames) * bytes_per_frame);
    std::uint64_t sequence = 0;
    VC_LOG_INFO("audio-capture",
                "capturing from " + config_.device +
                    " capture_channels=" + std::to_string(capture_channels) +
                    " output_channels=" + std::to_string(output_channels));
    finish_initialization(true);

    // 4. 数据轮询抓取
    while (running_) {
        // 读取交织音频帧
        snd_pcm_sframes_t frames = alsa.readi(handle, buffer.data(), period_frames);
        const std::uint64_t capture_pts_us = MediaClock::now_us();
        
        // 缓冲区溢出 (overrun) 异常处理
        if (frames == -kEpipe) {
            VC_LOG_WARN("audio-capture", "ALSA overrun, preparing device");
            alsa.prepare(handle);
            continue;
        }
        // 底层读取出错恢复
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

        // 5. 封装为 AudioFrame 并调用外部回调
        AudioFrame frame;
        frame.pts_us = capture_pts_us;
        frame.sample_rate = config_.sample_rate;
        frame.channels = static_cast<int>(output_channels);
        frame.sequence = sequence++;
        frame.pcm = convert_interleaved_s16(buffer.data(),
                                            static_cast<std::size_t>(frames),
                                            capture_channels,
                                            output_channels);
        if (callback_) {
            callback_(std::move(frame));
        }
    }

    // 6. 停止并关闭 ALSA 设备
    alsa.drop(handle);
    alsa.close(handle);
    running_ = false;
}

}  // namespace visioncast
