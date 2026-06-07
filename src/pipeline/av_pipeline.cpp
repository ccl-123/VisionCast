/**
 * @file av_pipeline.cpp
 * @brief VisionCast 音视频联合处理流水线模块的实现
 * @details 实现了 AvPipeline 类。负责创建共享的 AvTransport 传输组件，
 *          顺序管理网络连接、视频流水线和音频流水线的启动与停止，并对二者进行状态汇总。
 */

#include "pipeline/av_pipeline.h"

#include <utility>

namespace visioncast {

// 构造函数：初始化配置，创建共享的网络传输实例，并以此初始化视频与音频流水线
AvPipeline::AvPipeline(VisionCastConfig config)
    : config_(std::move(config)),
      transport_(std::make_shared<AvTransport>(config_)),
      video_(config_, transport_),
      audio_(config_, transport_) {}

AvPipeline::~AvPipeline() {
    stop(); // 析构时自动调用 stop 释放所有底层资源
}

bool AvPipeline::start(std::string& error) {
    if (started_) {
        return true; // 已启动，直接返回
    }
    
    // 1. 首先打开网络传输通道（如初始化 Socket、绑定端口或建立连接）
    if (!transport_->open(error)) {
        return false;
    }
    
    // 2. 启动视频处理流水线（包含 V4L2 采集、RGA 处理、MPP 编码等工作线程）
    if (!video_.start(error)) {
        transport_->close(); // 视频启动失败，关闭传输通道以回滚
        return false;
    }
    
    // 3. 启动音频处理流水线（包含 ALSA 采集与发送工作线程）
    if (!audio_.start(error)) {
        video_.stop();       // 音频启动失败，级联关闭已启动的视频流水线
        transport_->close(); // 关闭传输通道进行回滚
        return false;
    }
    
    started_ = true;
    return true;
}

void AvPipeline::stop() {
    // 依次安全停止音频、视频流水线，最后关闭网络传输通道
    audio_.stop();
    video_.stop();
    transport_->close();
    started_ = false;
}

bool AvPipeline::is_healthy() const {
    // 检查音视频流水线是否都在正常运行
    return started_ && video_.is_running() && audio_.is_running();
}

std::string AvPipeline::error() const {
    // 依次检查并返回视频或音频流水线中发生的错误
    const std::string video_error = video_.error();
    if (!video_error.empty()) {
        return "video pipeline: " + video_error;
    }
    const std::string audio_error = audio_.error();
    if (!audio_error.empty()) {
        return "audio pipeline: " + audio_error;
    }
    return "media pipeline stopped unexpectedly";
}

}  // namespace visioncast
