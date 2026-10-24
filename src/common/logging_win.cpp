#ifdef _WIN32

#include "logging_win.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace vrpacing {
namespace {

std::mutex gLogMutex;
std::ofstream gLogFile;

std::filesystem::path GetLogDir() {
    wchar_t* localAppData = nullptr;
    size_t len = 0;
    _wdupenv_s(&localAppData, &len, L"LOCALAPPDATA");
    std::filesystem::path dir = localAppData ? localAppData : L".";
    if (localAppData) {
        free(localAppData);
    }
    dir /= L"VRPacing";
    dir /= L"logs";
    std::filesystem::create_directories(dir);
    return dir;
}

const char* LevelText(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "UNK";
    }
}

} // namespace

void RotateLogsIfNeeded() {
    const auto dir = GetLogDir();
    const auto current = dir / "vrpacing.log";
    const auto old = dir / "vrpacing.prev.log";
    if (std::filesystem::exists(current)) {
        std::error_code ec;
        if (std::filesystem::file_size(current, ec) > (2 * 1024 * 1024)) {
            std::filesystem::remove(old, ec);
            std::filesystem::rename(current, old, ec);
        }
    }
}

void InitLogging() {
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gLogFile.is_open()) {
        return;
    }

    RotateLogsIfNeeded();
    const auto path = GetLogDir() / "vrpacing.log";
    gLogFile.open(path, std::ios::app);
}

void LogMessage(LogLevel level, const std::string& text) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (!gLogFile.is_open()) {
        InitLogging();
    }

    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " [" << LevelText(level) << "] " << text << "\n";
    OutputDebugStringA(ss.str().c_str());
    if (gLogFile.is_open()) {
        gLogFile << ss.str();
        gLogFile.flush();
    }
}

} // namespace vrpacing

#endif
