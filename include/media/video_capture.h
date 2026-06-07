/**
 * @file video_capture.h
 * @brief VisionCast 视频采集模块头文件
 * @details 封装了 Linux V4L2 (Video for Linux Two) 接口，负责与摄像头硬件交互，
 *          提供设备探测、参数初始化、视频流启动/停止以及采集线程的管理。
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "common/types.h"
#include "media/video_frame.h"

namespace visioncast {

/**
 * @class VideoCapture
 * @brief 视频采集类，基于 V4L2 实现摄像头的图像数据采集
 */
class VideoCapture {
public:
    // 视频帧采集回调函数
    using FrameCallback = std::function<void(VideoFrame&&)>;

    // 构造函数，指定视频配置参数（分辨率、格式、帧率、设备节点等）
    explicit VideoCapture(VideoConfig config);
    // 析构函数，负责停止采集线程并释放摄像头设备资源
    ~VideoCapture();

    // 禁用拷贝构造和赋值
    VideoCapture(const VideoCapture&) = delete;
    VideoCapture& operator=(const VideoCapture&) = delete;

    // 探测当前配置的摄像头设备是否支持所需的分辨率和格式
    VideoProbeResult probe() const;
    // 静态方法：探测指定摄像头设备节点（如 "/dev/video0"）的支持情况
    static VideoProbeResult probe_device(const std::string& device);

    // 启动视频采集线程，并注册帧数据到达时的回调函数
    bool start(FrameCallback callback);
    // 停止视频采集，安全退出工作线程并关闭设备
    void stop();

    // 检查视频采集是否正常运行中
    bool is_running() const;
    // 获取当前正在使用的活动设备节点路径
    std::string active_device() const;
    // 获取最新的错误信息描述
    std::string error() const;

private:
    // 视频采集主循环，负责在独立线程中执行 V4L2 设备的打开、缓冲区映射、帧入队出队和回调分发
    void capture_loop();
    // 辅助函数：更新并通知主线程初始化工作已完成
    void finish_initialization(bool success, const std::string& error = {});

    VideoConfig config_;                  // 视频采集配置信息
    FrameCallback callback_;              // 视频帧就绪时的回调函数
    std::thread thread_;                  // 采集工作线程对象
    std::atomic<bool> running_{false};    // 指示采集工作是否在运行的原子标志

    mutable std::mutex state_mutex_;      // 用于状态同步的互斥锁
    std::condition_variable state_cv_;    // 用于等待初始化完成的条件变量
    bool initialization_done_ = false;     // 初始化是否结束的标志
    bool initialization_success_ = false;  // 初始化是否成功的标志
    std::string active_device_;           // 保存当前打开的活动设备名
    std::string error_;                   // 保存最新的错误日志
};

}  // namespace visioncast
