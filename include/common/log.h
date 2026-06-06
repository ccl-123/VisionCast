#pragma once

#include <string>

namespace visioncast {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

void set_log_level(LogLevel level);
LogLevel get_log_level();
void log_message(LogLevel level,
                 const std::string& component,
                 const std::string& message,
                 const char* file,
                 int line);

}  // namespace visioncast

#define VC_LOG_DEBUG(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Debug, component, message, __FILE__, __LINE__)

#define VC_LOG_INFO(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Info, component, message, __FILE__, __LINE__)

#define VC_LOG_WARN(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Warn, component, message, __FILE__, __LINE__)

#define VC_LOG_ERROR(component, message) \
    ::visioncast::log_message(::visioncast::LogLevel::Error, component, message, __FILE__, __LINE__)
