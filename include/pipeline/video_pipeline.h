/**
 * @file video_pipeline.h
 * @brief VisionCast 视频流水线处理模块
 * @details 负责视频数据的采集（V4L2 驱动）、图像格式转换与缩放裁剪（Rockchip RGA 硬件加速）、
 *          视频编码（Rockchip MPP H.264 硬件编码）、本地显示渲染（DRM/KMS/X11等渲染器）以及网络发送。
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

#include "common/blocking_queue.h"
#include "common/types.h"
#include "media/display_renderer.h"
#include "media/mpp_encoder.h"
#include "media/rga_processor.h"
#include "media/video_capture.h"
#include "media/video_frame.h"
#include "transport/av_transport.h"

namespace visioncast {

class VideoPipeline {
public:
    /**
     * @brief 构造函数，初始化视频流水线
     * @param config 全局 VisionCast 配置信息
     * @param transport 传输接口共享指针
     */
    VideoPipeline(VisionCastConfig config, std::shared_ptr<AvTransport> transport);
    ~VideoPipeline();

    // 禁用拷贝构造和赋值操作
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    /**
     * @brief 启动视频流水线，包括初始化采集、RGA处理器、MPP编码器、渲染器及工作线程
     * @param error 如果启动失败，返回错误描述信息
     * @return 启动成功返回 true，否则返回 false
     */
    bool start(std::string& error);

    /**
     * @brief 停止视频流水线并释放相关资源
     */
    void stop();

    /**
     * @brief 检查视频流水线是否正在运行
     * @return 运行中返回 true，否则返回 false
     */
    bool is_running() const;

    /**
     * @brief 获取流水线运行过程中的错误描述
     * @return 错误信息字符串
     */
    std::string error() const;

    /**
     * @brief 获取丢弃的视频帧数（通常由于队列满或硬件处理不及时导致）
     * @return 丢弃帧计数
     */
    std::size_t dropped_frames() const;

private:
    /**
     * @brief 视频处理工作线程的主循环
     *        依次进行采集数据读取、RGA 像素格式转换与裁剪、本地显示渲染（可选）以及 MPP 硬件编码与网络发送。
     */
    void worker_loop();

    VisionCastConfig config_;                        ///< 全局系统配置，包含分辨率、码率、帧率等
    VideoCapture capture_;                           ///< 视频采集模块，基于 V4L2 接口实现
    RgaProcessor processor_;                         ///< Rockchip RGA 图形加速处理器，用于格式转换与缩放
    MppEncoder encoder_;                             ///< Rockchip MPP 硬件编码器，执行 H.264 等格式编码
    std::shared_ptr<AvTransport> transport_;          ///< 传输模块接口，用于编码后视频数据的网络推送
    DisplayRenderer renderer_;                       ///< 本地视频渲染显示器
    BoundedBlockingQueue<VideoFrame> raw_queue_{2};  ///< 存储原始捕获视频帧的阻塞队列，大小限制为 2
    std::thread worker_;                             ///< 视频流水线处理工作线程
    std::atomic<bool> running_{false};               ///< 指示流水线是否正在运行的原子布尔值
    mutable std::mutex error_mutex_;                 ///< 用于保护 error_ 线程安全访问的互斥锁
    std::string error_;                              ///< 存放当前发生的错误描述信息
};

}  // namespace visioncast
