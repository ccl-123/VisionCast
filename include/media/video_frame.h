/**
 * @file video_frame.h
 * @brief VisionCast 视频帧数据结构头文件
 * @details 定义了表示原始/解码后视频帧（VideoFrame）的结构体，
 *          包含像素数据流、分辨率、跨距（stride）以及时间戳和序列号等关键元数据。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visioncast {

/**
 * @struct VideoFrame
 * @brief 视频图像帧结构体，用于在采集、解码、处理与渲染模块间传递图像数据
 */
struct VideoFrame {
    std::vector<std::uint8_t> data;   // 原始或解码后的视频图像像素数据（如 NV12, YUYV, MJPEG 字节流）
    std::uint64_t pts_us = 0;         // 演示时间戳（Presentation Time Stamp），单位微秒
    int width = 0;                    // 视频帧的图像宽度（像素）
    int height = 0;                   // 视频帧的图像高度（像素）
    int stride = 0;                   // 水平跨距/步长（一行的字节数，考虑内存对齐时可能大于 width）
    int vertical_stride = 0;          // 垂直步长（图像高度方向的对齐步长）
    std::string format;               // 图像格式（如 "NV12", "YUYV", "MJPEG" 等）
    std::uint64_t sequence = 0;       // 视频帧序号，用于帧序追踪和丢帧检测
};

}  // namespace visioncast
