/**
 * @file pipeline_manager.h
 * @brief VisionCast 流水线管理中心
 * @details 负责探测音视频硬件设备（ALSA, V4L2等），输出硬件探测摘要，打印系统配置，
 *          生成 SDP 会话描述文件，并控制和运行整个流媒体的生命周期。
 */

#pragma once

#include "common/types.h"
#include "media/audio_capture.h"
#include "media/video_capture.h"

namespace visioncast {

/**
 * @brief 流水线设备探测的摘要结果结构体
 *        包含主视频设备、备份视频设备、音频设备的探测状态，以及设备是否准备就绪的标志。
 */
struct PipelineProbeSummary {
    VideoProbeResult primary_video;   ///< 主视频设备的探测结果
    VideoProbeResult fallback_video;  ///< 备用视频设备的探测结果
    AudioProbeResult audio;           ///< 音频设备的探测结果
    bool fallback_checked = false;    ///< 是否检查过备用视频设备
    bool video_ready = false;         ///< 视频设备是否至少有一个可用且准备就绪
    bool audio_ready = false;         ///< 音频设备是否可用且准备就绪
};

/**
 * @class PipelineManager
 * @brief 整个音视频推流系统的管理者，负责统筹配置、硬件检测、启动/停止流媒体任务
 */
class PipelineManager {
public:
    /**
     * @brief 构造函数，使用全局配置初始化流水线管理器
     * @param config 全局 VisionCastConfig 配置对象
     */
    explicit PipelineManager(VisionCastConfig config);

    /**
     * @brief 打印当前的音视频及网络流传输配置
     */
    void print_config() const;

    /**
     * @brief 探测系统中的音频和视频硬件设备（通过 ALSA 和 V4L2 驱动）
     * @return 返回包含探测结果的 PipelineProbeSummary 结构体
     */
    PipelineProbeSummary probe_devices() const;

    /**
     * @brief 执行设备探测流程并根据参数决定是否强制要求设备存在
     * @param require_devices 是否必须要求至少有视频或音频设备可用。若为真且设备不可用，则返回错误码。
     * @return 执行状态码，0 表示成功，非 0 表示有设备故障或缺失
     */
    int run_probe(bool require_devices) const;

    /**
     * @brief 启动流媒体推送流程，包括初始化网络传输、创建并启动音视频联合流水线
     *        该函数会阻塞并持续监测流水线健康状态，直到被中断或发生严重错误。
     * @return 运行状态码，0 表示正常退出，非 0 表示运行期间出错
     */
    int run_stream();

    /**
     * @brief 生成用于 RTP 单播/组播传输的 SDP 配置文件
     * @param error 如果生成或写入文件失败，返回错误描述信息
     * @return 写入成功返回 true，否则返回 false
     */
    bool write_sdp_file(std::string& error) const;

private:
    VisionCastConfig config_;  ///< 系统全局配置参数
};

}  // namespace visioncast
