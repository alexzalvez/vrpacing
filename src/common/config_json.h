#pragma once

#ifdef _WIN32

#include <optional>
#include <string>

#include "vrpacing/config.h"

namespace vrpacing {

std::string GetConfigPath();
RuntimeConfig LoadConfig();
bool SaveConfig(const RuntimeConfig& cfg);

} // namespace vrpacing

#endif
