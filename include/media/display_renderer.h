/**
 * @file display_renderer.h
 * @brief VisionCast 显示渲染器头文件
 * @details 负责将解码后的视频帧（VideoFrame）显示到本地窗口中，封装了基于 OpenCV 或其他图形界面的渲染循环，
 *          并包含线程安全的阻塞队列以同步数据提交 and 渲染线程。
 */

#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "common/blocking_queue.h"
#include "media/video_frame.h"

namespace visioncast {

/**
 * @class DisplayRenderer
 * @brief 视频显示渲染类，管理图形界面窗口与渲染循环线程
 */
class DisplayRenderer {
public:
    // 构造函数，可指定显示的窗口名称，默认为 "VisionCast"
    explicit DisplayRenderer(std::string window_name = "VisionCast");
    // 析构函数，停止渲染并释放相关资源
    ~DisplayRenderer();

    // 禁用拷贝构造和赋值，防止多线程竞争或资源泄漏
    DisplayRenderer(const DisplayRenderer&) = delete;
    DisplayRenderer& operator=(const DisplayRenderer&) = delete;

    // 启动渲染线程，初始化图形显示窗口
    bool start();
    // 停止渲染线程并销毁窗口
    void stop();
    // 提交一帧待渲染的视频帧到渲染队列（若队列满，根据策略阻塞或覆盖）
    void submit(VideoFrame frame);

private:
    // 渲染主线程循环函数，负责从队列获取视频帧并执行渲染刷新
    void render_loop();

    std::string window_name_;                     // 渲染窗口名称
    BoundedBlockingQueue<VideoFrame> queue_{1};   // 视频帧有界阻塞队列（容量通常限制在1，以保证低延迟显示最新帧）
    std::thread thread_;                          // 渲染工作线程
    std::atomic<bool> running_{false};            // 渲染线程运行标志
};

}  // namespace visioncast
