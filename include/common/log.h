/**
 * @file log.h
 * @brief VisionCast Logging System
 * 
 * 本文件提供了 VisionCast 系统专用的日志输出框架。定义了不同的日志级别（LogLevel）、
 * 线程安全的日志记录函数（log_message）以及对应的便捷宏（VC_LOG_*）。
 * 便捷宏能够自动捕获日志调用处的文件名与行号，简化调试。
 */

#pragma once

#include <string>

namespace visioncast {

/**
 * @brief 日志输出级别。
 */
enum class LogLevel {
    Debug = 0, ///< 调试级别，输出最详尽的信息，包括调用源文件名及行号
    Info = 1,  ///< 信息级别，常规运行状态提示
    Warn = 2,  ///< 警告级别，提示可能存在的问题，但不影响系统主流程
    Error = 3, ///< 错误级别，提示严重故障或异常情况
};

/**
 * @brief 设置系统当前允许的最低日志输出级别。
 * @param level 目标日志级别。
 */
void set_log_level(LogLevel level);

/**
 * @brief 获取系统当前设置的最低日志输出级别。
 * @return 当前日志级别。
 */
LogLevel get_log_level();

/**
 * @brief 打印日志消息的核心底层接口。
 * 
 * 该接口是线程安全的（通过全局互斥锁同步输出），并会自动加上高精度的本地时间戳。
 * 
 * @param level 当前输出日志的级别。
 * @param component 日志所属的系统组件或模块名称（例如 "VIDEO", "AUDIO", "RTSP"）。
 * @param message 具体要输出的日志内容。
 * @param file 调用处所在的代码源文件路径（主要用于 Debug 级别）。
 * @param line 调用处所在的代码行号。
 */
void log_message(LogLevel level,
                 const std::string& component,
                 const std::string& message,
                 const char* file,
                 int line);

}  // namespace visioncast

/**
 * @def VC_LOG_DEBUG
 * @brief 便捷调试日志宏。在 Debug 级别下，除了输出信息外还会带上源文件和行号信息。
 */
#define VC_LOG_DEBUG(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Debug, component, message, __FILE__, __LINE__)

/**
 * @def VC_LOG_INFO
 * @brief 便捷信息日志宏。
 */
#define VC_LOG_INFO(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Info, component, message, __FILE__, __LINE__)

/**
 * @def VC_LOG_WARN
 * @brief 便捷警告日志宏。
 */
#define VC_LOG_WARN(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Warn, component, message, __FILE__, __LINE__)

/**
 * @def VC_LOG_ERROR
 * @brief 便捷错误日志宏。
 */
#define VC_LOG_ERROR(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Error, component, message, __FILE__, __LINE__)
