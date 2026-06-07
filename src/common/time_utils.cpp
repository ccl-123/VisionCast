/**
 * @file time_utils.cpp
 * @brief VisionCast Time Utilities Implementation
 * 
 * 本文件实现了 VisionCast 项目的高精度时间工具函数。
 * 使用 Linux 底层的 `clock_gettime(CLOCK_MONOTONIC, ...)` 实现微秒精度的单调时间获取，
 * 以确保音视频同步及性能评测的准确性。
 */

#include "common/time_utils.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace visioncast {

std::uint64_t monotonic_now_us() {
    timespec ts{};
    // 获取单调递增系统时钟，避免 NTP 同步带来的时间回退/跳跃问题
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 将秒和纳秒分别换算为微秒并相加
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

std::string format_duration_us(std::uint64_t duration_us) {
    std::ostringstream out;
    // 小于 1 毫秒时，保留微秒整数输出
    if (duration_us < 1000ULL) {
        out << duration_us << " us";
        return out.str();
    }
    // 小于 1 秒时，转换为毫秒并保留两位小数
    if (duration_us < 1000000ULL) {
        out << std::fixed << std::setprecision(2)
            << static_cast<double>(duration_us) / 1000.0 << " ms";
        return out.str();
    }
    // 大于等于 1 秒时，转换为秒并保留两位小数
    out << std::fixed << std::setprecision(2)
        << static_cast<double>(duration_us) / 1000000.0 << " s";
    return out.str();
}

}  // namespace visioncast
