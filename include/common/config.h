#pragma once

#include <string>

#include "common/types.h"

namespace visioncast {

VisionCastConfig default_config();
bool load_config_file(const std::string& path, VisionCastConfig& config, std::string& error);
std::string summarize_config(const VisionCastConfig& config);
void replace_all(std::string& str, const std::string& from, const std::string& to);

}  // namespace visioncast
