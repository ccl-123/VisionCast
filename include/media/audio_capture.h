/**
 * @file audio_capture.h
 * @brief VisionCast 音频采集模块头文件
 * @details 负责从系统音频设备（如 ALSA 设备）捕获原始 PCM 音频数据，并将其封装为 AudioFrame 回调给订阅者。
 *          主要功能包括设备探测、参数配置、采集线程管理以及生命周期同步。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/types.h"
#include "media/audio_frame.h"

namespace visioncast {

/**
 * @class AudioCapture
 * @brief 音频采集类，封装了 ALSA 等底层音频捕获接口
 */
class AudioCapture {
public:
    // 音频帧回调函数定义
    using FrameCallback = std::function<void(AudioFrame&&)>;

    // 构造函数，传入音频配置参数（如通道数、采样率、采样格式、设备名等）
    explicit AudioCapture(AudioConfig config);
    // 析构函数，负责停止采集线程并释放相关资源
    ~AudioCapture();

    // 禁用拷贝构造和赋值运算符，防止资源重复释放或冲突
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // 探测当前配置的音频设备是否可用
    AudioProbeResult probe() const;
    // 静态方法：探测指定名称的音频设备是否可用（如 "default"、"hw:0,0" 等）
    static AudioProbeResult probe_device(const std::string& device);

    // 启动音频捕获线程，并注册数据回调接口
    bool start(FrameCallback callback);
    // 停止音频捕获线程，阻塞等待线程安全退出
    void stop();

    // 检查音频捕获是否正在运行
    bool is_running() const;
    // 获取最新的错误信息描述
    std::string error() const;

private:
    // 音频采集主线程循环函数，包含 ALSA 设备的打开、参数设置与数据读取逻辑
    void capture_loop();
    // 辅助函数：通知并唤醒等待初始化完成的调用线程
    void finish_initialization(bool success, const std::string& error = {});

    AudioConfig config_;         // 音频配置参数
    FrameCallback callback_;     // 数据帧到达时的回调函数
    std::thread thread_;         // 音频采集线程对象
    std::atomic<bool> running_{false}; // 标识采集线程是否处于运行状态的原子变量

    mutable std::mutex state_mutex_;    // 保护初始化及运行状态的互斥锁
    std::condition_variable state_cv_;  // 用于主线程与采集线程同步初始化状态的条件变量
    bool initialization_done_ = false;   // 标识采集线程是否已完成初始化
    bool initialization_success_ = false;// 标识采集线程是否成功初始化设备
    std::string error_;                 // 保存最近一次的错误信息
};

}  // namespace visioncast
