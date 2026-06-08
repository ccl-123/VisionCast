/**
 * @file mjpeg_decoder.h
 * @brief VisionCast MJPEG 视频解码器头文件
 * @details 负责将从 V4L2 摄像头捕获的原始 MJPEG 编码帧图像解码为 NV12 或者其他格式（例如 YUV 格式），
 *          采用了 Pimpl (Pointer to Implementation) 模式以隐藏底层解码依赖（如 Rockchip MPP、libjpeg-turbo 等）。
 */

#pragma once

#include <memory>
#include <string>

#include "media/video_frame.h"

namespace visioncast {

/**
 * @class MjpegDecoder
 * @brief MJPEG 解码器类，提供硬解码/软解码接口
 */
class MjpegDecoder {
public:
    // 前置声明具体实现类，用于 Pimpl 模式
    struct Impl;

    // 构造函数，初始化解码器上下文
    MjpegDecoder();
    // 析构函数，销毁解码器上下文并释放资源
    ~MjpegDecoder();

    // 禁用拷贝构造和赋值
    MjpegDecoder(const MjpegDecoder&) = delete;
    MjpegDecoder& operator=(const MjpegDecoder&) = delete;

    // Decodes one complete V4L2 MJPEG image to NV12.
    // 将一帧完整的 V4L2 MJPEG 编码图像解码为 NV12 格式
    // @param input 输入的 MJPEG 视频帧
    // @param output 输出的解码后视频帧（例如 NV12）
    // @param error 若解码失败，用于保存错误信息的字符串
    // @param allow_dma_output 是否允许硬解输出 DMA-BUF 帧，可直接送入 RGA/MPP fd 路径
    // @return 解码成功返回 true，否则返回 false
    bool decode(const VideoFrame& input,
                VideoFrame& output,
                std::string& error,
                bool allow_dma_output = true);

    // Returns true if the last frame was decoded using Rockchip MPP hardware acceleration.
    bool is_hardware_accelerated() const;

private:
    std::unique_ptr<Impl> impl_; // 指向具体实现类的智能指针
};

}  // namespace visioncast
