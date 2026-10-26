#pragma once

#ifdef _WIN32

#include <atomic>

namespace vrpacing {

void InstallOpenVRHooks();
extern std::atomic<bool> g_openVrHandlingFrames;

} // namespace vrpacing

#endif