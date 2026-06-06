#include "common/log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace visioncast {
namespace {

std::mutex g_log_mutex;
LogLevel g_log_level = LogLevel::Info;

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

std::string now_text() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;

    std::tm local_time{};
    localtime_r(&time, &local_time);

    std::ostringstream out;
    out << std::put_time(&local_time, "%F %T")
        << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return out.str();
}

}  // namespace

void set_log_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_log_level = level;
}

LogLevel get_log_level() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log_level;
}

void log_message(LogLevel level,
                 const std::string& component,
                 const std::string& message,
                 const char* file,
                 int line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (static_cast<int>(level) < static_cast<int>(g_log_level)) {
        return;
    }

    std::ostream& output = std::cout;
    output << '[' << now_text() << "] "
           << '[' << level_name(level) << "] "
           << '[' << component << "] "
           << message;
    if (level == LogLevel::Debug) {
        output << " (" << file << ':' << line << ')';
    }
    output << std::endl;
}

}  // namespace visioncast
