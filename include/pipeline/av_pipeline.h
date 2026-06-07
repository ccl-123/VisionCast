/**
 * @file av_pipeline.h
 * @brief VisionCast 音视频联合处理流水线模块
 * @details 整合了视频处理流水线（VideoPipeline）和音频处理流水线（AudioPipeline），
 *          统一管理音视频的生命周期、数据流传输（AvTransport）以及整体的健康状态。
 */

#pragma once

#include <string>
#include <memory>

#include "common/types.h"
#include "pipeline/audio_pipeline.h"
#include "pipeline/video_pipeline.h"
#include "transport/av_transport.h"

namespace visioncast {

class AvPipeline {
public:
    /**
     * @brief 构造函数，初始化音视频联合流水线
     * @param config VisionCast 全局配置信息
     */
    explicit AvPipeline(VisionCastConfig config);
    ~AvPipeline();

    // 禁用拷贝构造和赋值操作
    AvPipeline(const AvPipeline&) = delete;
    AvPipeline& operator=(const AvPipeline&) = delete;

    /**
     * @brief 启动音视频联合流水线，依次启动视频和音频子流水线
     * @param error 如果启动失败，返回错误描述信息
     * @return 启动成功返回 true，否则返回 false
     */
    bool start(std::string& error);

    /**
     * @brief 停止音视频联合流水线
     */
    void stop();

    /**
     * @brief 检查联合流水线运行状态是否健康
     * @return 如果音视频流水线均正常运行或未出错返回 true，否则返回 false
     */
    bool is_healthy() const;

    /**
     * @brief 获取流水线运行过程中的汇总错误信息
     * @return 错误信息字符串
     */
    std::string error() const;

private:
    VisionCastConfig config_;                 ///< 系统全局配置，包含音视频的参数
    std::shared_ptr<AvTransport> transport_;  ///< 音视频发送的传输接口指针，供子流水线共享使用
    VideoPipeline video_;                     ///< 视频处理流水线实例
    AudioPipeline audio_;                     ///< 音频处理流水线实例
    bool started_ = false;                    ///< 标识该音视频联合流水线是否已启动
};

}  // namespace visioncast
