/**
 * @file log.cpp
 * @brief VisionCast Logging System Implementation
 * 
 * 本文件实现了 VisionCast 的日志记录功能。
 * 通过内部全局互斥锁（g_log_mutex）确保多线程日志输出的有序与安全。
 * 并使用时钟（std::chrono）接口获取并格式化高精度的本地时间戳（毫秒级）。
 */

#include "common/log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace visioncast {
namespace {

std::mutex g_log_mutex;                  ///< 全局互斥锁，用于同步多线程对控制台 stdout 的写入操作
LogLevel g_log_level = LogLevel::Info;   ///< 当前全局日志级别，默认为 Info 级

/**
 * @brief 辅助函数：将日志级别枚举翻译为短文本标识符。
 */
const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

const char* ANSI_RESET = "\033[0m";

/**
 * @brief 辅助函数：根据日志级别获取对应的 ANSI 颜色控制字符。
 */
const char* level_color(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "\033[36m"; // 青色 (Cyan)
        case LogLevel::Info:
            return "\033[32m"; // 绿色 (Green)
        case LogLevel::Warn:
            return "\033[33m"; // 黄色 (Yellow)
        case LogLevel::Error:
            return "\033[31m"; // 红色 (Red)
    }
    return "\033[0m";
}

/**
 * @brief 获取格式化的当前本地时间字符串，精确到毫秒级。
 * 
 * 格式示例: "YYYY-MM-DD HH:MM:SS.mmm"
 */
std::string now_text() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    // 计算当前秒的剩余毫秒数
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::tm local_time{};
    localtime_r(&time, &local_time); // 使用线程安全的 localtime_r 获取本地时间

    std::ostringstream out;
    out << std::put_time(&local_time, "%F %T")
        << '.' << std::setw(3) << std::setfill('0') << ms.count(); // 补齐三位毫秒
    return out.str();
}

}  // namespace

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_log_mutex); // 加锁保护全局日志级别的写操作
    g_log_level = level;
}

LogLevel get_log_level() {
    std::lock_guard<std::mutex> lock(g_log_mutex); // 加锁保护全局日志级别的读操作
    return g_log_level;
}

void log_message(LogLevel level,
                 const std::string& component,
                 const std::string& message,
                 const char* file,
                 int line) {
    std::lock_guard<std::mutex> lock(g_log_mutex); // 加锁保护整个日志输出逻辑，防止控制台内容交错
    if (static_cast<int>(level) < static_cast<int>(g_log_level)) {
        return; // 低于设定输出级别的日志直接丢弃
    }

    std::ostream& output = std::cout; // 默认输出到标准输出设备 (stdout)
    output << level_color(level)
           << '[' << now_text() << "] "
           << '[' << level_name(level) << "] "
           << '[' << component << "] "
           << ANSI_RESET
           << message;
    if (level == LogLevel::Debug) {
        // Debug 级别下额外输出文件名和行号，辅助定位问题
        output << " (" << file << ':' << line << ')';
    }
    output << std::endl; // 刷缓冲并换行
}

}  // namespace visioncast
