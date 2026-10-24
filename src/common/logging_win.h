#pragma once

#ifdef _WIN32

#include <string>

namespace vrpacing {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

void InitLogging();
void RotateLogsIfNeeded();
void LogMessage(LogLevel level, const std::string& text);

} // namespace vrpacing

#endif
