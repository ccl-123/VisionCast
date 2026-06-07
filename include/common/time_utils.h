/**
 * @file time_utils.h
 * @brief VisionCast Time Utilities
 * 
 * 本文件声明了 VisionCast 项目中用于高精度时间测量的实用工具函数。
 * 提供了单调递增微秒级时间戳获取接口以及时间跨度格式化输出接口，
 * 主要服务于性能统计、延迟计算和网络封包的时间戳标记。
 */

#pragma once

#include <cstdint>
#include <string>

namespace visioncast {

/**
 * @brief 获取当前系统的单调递增时间戳（微秒级精度）。
 * 
 * 使用 CLOCK_MONOTONIC 系统时钟，不受系统时间调整（如 NTP 对时）的影响，
 * 非常适合用于计算时间差和音视频帧的相对时间戳。
 * 
 * @return 自系统启动以来经过的微秒数。
 */
std::uint64_t monotonic_now_us();

/**
 * @brief 将微秒单位的时间跨度转换为更易读的字符串格式。
 * 
 * 转换规则：
 * - 小于 1000 微秒，输出 "N us"；
 * - 大于等于 1 毫秒且小于 1 秒，输出保留两位小数的 "N.NN ms"；
 * - 大于等于 1 秒，输出保留两位小数的 "N.NN s"。
 * 
 * @param duration_us 微秒级的时间跨度。
 * @return 带有相应单位的格式化字符串（如 "15.50 ms", "1.24 s"）。
 */
std::string format_duration_us(std::uint64_t duration_us);

}  // namespace visioncast
