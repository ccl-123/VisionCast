/**
 * @file audio_pipeline.h
 * @brief VisionCast 音频流水线处理模块
 * @details 负责音频数据的采集（通过 ALSA 接口）、缓冲队列管理以及音频数据的传输。
 *          主要内容包含 AudioPipeline 类的声明，以及相关的缓冲队列和成员变量。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <memory>
#include <mutex>

#include "common/blocking_queue.h"
#include "common/types.h"
#include "media/audio_capture.h"
#include "media/audio_frame.h"
#include "transport/av_transport.h"

namespace visioncast {

class AudioPipeline {
public:
    /**
     * @brief 构造函数，初始化音频流水线
     * @param config VisionCast 全局配置
     * @param transport 音视频传输接口指针，用于发送编码或打包后的音频数据
     */
    AudioPipeline(VisionCastConfig config, std::shared_ptr<AvTransport> transport);
    ~AudioPipeline();

    // 禁用拷贝构造和赋值操作
    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    /**
     * @brief 启动音频流水线
     * @param error 如果启动失败，返回错误描述信息
     * @return 启动成功返回 true，否则返回 false
     */
    bool start(std::string& error);

    /**
     * @brief 停止音频流水线
     */
    void stop();

    /**
     * @brief 检查音频流水线是否正在运行
     * @return 运行中返回 true，否则返回 false
     */
    bool is_running() const;

    /**
     * @brief 获取流水线运行过程中的错误信息
     * @return 错误信息字符串
     */
    std::string error() const;

    /**
     * @brief 获取丢弃的音频帧数量（由于队列已满且无法及时处理）
     * @return 丢弃帧计数
     */
    std::size_t dropped_frames() const;

private:
    /**
     * @brief 音频流水线工作线程的主循环
     *        负责从音频采集端抓取 PCM 数据，放入缓存或直接通过传输层发送。
     */
    void worker_loop();

    VisionCastConfig config_;                        ///< 全局系统配置项，包含音频参数（采样率、通道数等）
    AudioCapture capture_;                           ///< 音频捕获模块，底层使用 ALSA 实现原始音频采集
    std::shared_ptr<AvTransport> transport_;          ///< 传输模块接口，负责将音频发送至流媒体服务器
    BoundedBlockingQueue<AudioFrame> raw_queue_{8};  ///< 存储原始音频帧的限长阻塞队列，防止内存无限增长
    std::thread worker_;                             ///< 运行 worker_loop 的音频处理工作线程
    std::atomic<bool> running_{false};               ///< 标志流水线运行状态的原子变量
    mutable std::mutex error_mutex_;                 ///< 保护 error_ 成员变量线程安全访问的互斥锁
    std::string error_;                              ///< 记录流水线运行中的错误描述信息
};

}  // namespace visioncast
