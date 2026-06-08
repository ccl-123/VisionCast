/**
 * @file video_frame.h
 * @brief VisionCast 视频帧数据结构头文件
 * @details 定义了表示原始/解码后视频帧（VideoFrame）的结构体，
 *          包含像素数据流、分辨率、跨距（stride）以及时间戳和序列号等关键元数据。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace visioncast {

/**
 * @struct VideoDmaPlane
 * @brief 单个 DMA-BUF plane 描述，fd 由 VideoDmaBuffer 持有并在析构时关闭
 */
struct VideoDmaPlane {
    int fd = -1;                         // DMA-BUF 文件描述符
    std::size_t offset = 0;              // 有效数据在 plane 内的偏移
    std::size_t bytesused = 0;           // 当前帧有效字节数
    std::size_t length = 0;              // DMA-BUF 总长度
};

/**
 * @struct VideoDmaBuffer
 * @brief 视频帧 DMA-BUF 生命周期句柄，最后一个引用释放时触发底层归还逻辑
 */
struct VideoDmaBuffer {
    VideoDmaBuffer() = default;
    VideoDmaBuffer(const VideoDmaBuffer&) = delete;
    VideoDmaBuffer& operator=(const VideoDmaBuffer&) = delete;
    ~VideoDmaBuffer() {
        for (auto& plane : planes) {
            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }
        }
        if (release) {
            release();
        }
    }

    std::vector<VideoDmaPlane> planes;   // DMA-BUF planes
    std::uint32_t buffer_index = 0;      // V4L2/硬件缓冲区索引，用于日志和归还
    std::function<void()> release;       // 释放回调，例如 V4L2 VIDIOC_QBUF
};

/**
 * @struct VideoFrame
 * @brief 视频图像帧结构体，用于在采集、解码、处理与渲染模块间传递图像数据
 */
struct VideoFrame {
    std::vector<std::uint8_t> data;   // 原始或解码后的视频图像像素数据（如 NV12, YUYV, MJPEG 字节流）
    std::shared_ptr<VideoDmaBuffer> dma; // DMA-BUF backed frame, available on zero-copy paths
    std::uint64_t pts_us = 0;         // 演示时间戳（Presentation Time Stamp），单位微秒
    int width = 0;                    // 视频帧的图像宽度（像素）
    int height = 0;                   // 视频帧的图像高度（像素）
    int stride = 0;                   // 水平跨距/步长（一行的字节数，考虑内存对齐时可能大于 width）
    int vertical_stride = 0;          // 垂直步长（图像高度方向的对齐步长）
    std::string format;               // 图像格式（如 "NV12", "YUYV", "MJPEG" 等）
    std::uint64_t sequence = 0;       // 视频帧序号，用于帧序追踪和丢帧检测

    bool has_dma() const {
        return dma != nullptr && !dma->planes.empty();
    }

    bool has_single_plane_dma() const {
        return dma != nullptr && dma->planes.size() == 1 && dma->planes.front().fd >= 0;
    }

    std::size_t dma_bytesused() const {
        std::size_t total = 0;
        if (dma == nullptr) {
            return total;
        }
        for (const auto& plane : dma->planes) {
            total += plane.bytesused;
        }
        return total;
    }
};

}  // namespace visioncast
