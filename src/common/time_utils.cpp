#include "common/time_utils.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace visioncast {

std::uint64_t monotonic_now_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000ULL;
}

std::string format_duration_us(std::uint64_t duration_us) {
    std::ostringstream out;
    if (duration_us < 1000ULL) {
        out << duration_us << " us";
        return out.str();
    }
    if (duration_us < 1000000ULL) {
        out << std::fixed << std::setprecision(2)
            << static_cast<double>(duration_us) / 1000.0 << " ms";
        return out.str();
    }
    out << std::fixed << std::setprecision(2)
        << static_cast<double>(duration_us) / 1000000.0 << " s";
    return out.str();
}

}  // namespace visioncast
