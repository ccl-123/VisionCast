/**
 * @file rga_processor.h
 * @brief VisionCast RGA (Raster Graphic Acceleration) 图像处理器头文件
 * @details 负责利用瑞芯微（Rockchip）的 RGA 硬件加速引擎对视频帧进行格式转换（例如 YUYV/MJPEG 转 NV12）
 *          以及图像的缩放、旋转和裁剪等图像后处理操作。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "media/mjpeg_decoder.h"
#include "media/video_frame.h"

namespace visioncast {

/**
 * @class RgaProcessor
 * @brief 图像缩放及格式转换处理器，集成 MJPEG 解码器并调用 RGA 硬件加速接口
 */
class RgaProcessor {
public:
    // 构造函数，指定输出视频帧的目标宽度和高度
    RgaProcessor(int output_width, int output_height);

    // Converts incoming V4L2 frames to NV12 and scales to the configured output.
    // 将输入的原始 V4L2 视频帧（YUYV 或 MJPEG 格式）转换并缩放为指定尺寸的 NV12 格式帧
    // @param input 输入视频帧（包含采集到的原始图像数据及格式信息）
    // @param output 输出的目标视频帧（转换并缩放后的 NV12 图像）
    // @param error 错误信息描述输出
    // @return 处理成功返回 true，否则返回 false
    bool process(const VideoFrame& input, VideoFrame& output, std::string& error);

    // Returns true if the last process call utilized RGA hardware acceleration.
    bool is_hardware_accelerated() const;
    // Returns the last processing path, such as BYPASS-DMA, RGA-FD, RGA-VA, or CPU.
    std::string last_mode() const;
    bool last_mjpeg_input() const;
    bool last_mjpeg_hardware() const;
    bool last_mjpeg_dma_output() const;
    std::uint64_t last_mjpeg_decode_us() const;

private:
    MjpegDecoder mjpeg_decoder_;  // 用于 MJPEG 格式输入帧的解码器实例
    int output_width_;            // 目标输出的图像宽度
    int output_height_;           // 目标输出的图像高度
    bool last_frame_hardware_ = false; // 记录上一帧是否使用 RGA 硬件加速
    std::string last_mode_ = "CPU";    // 记录上一帧处理路径，便于性能日志识别零拷贝状态
    bool last_mjpeg_input_ = false;
    bool last_mjpeg_hardware_ = false;
    bool last_mjpeg_dma_output_ = false;
    std::uint64_t last_mjpeg_decode_us_ = 0;
};

}  // namespace visioncast
