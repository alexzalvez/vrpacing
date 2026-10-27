#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/logging_win.h"

#include <windows.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#include "vrpacing/shared_state.h"

namespace vrpacing {
namespace {

std::mutex g_logMutex;
std::wstring g_logFilePath;
bool g_initialized = false;

std::wstring GetDefaultLogPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir;
    if (len > 0 && len < MAX_PATH) {
        dir = std::wstring(buf) + L"\\VRPacing\\logs";
    } else {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path);
        const auto pos = p.find_last_of(L"\\/");
        if (pos != std::wstring::npos) p.resize(pos);
        dir = p;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + L"\\vrpacing.log";
}

} // namespace

void InitLogging(const std::wstring& customPath) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_initialized) return;

    g_logFilePath = customPath.empty() ? GetDefaultLogPath() : customPath;
    g_initialized = true;

    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modPath, MAX_PATH);
    std::wstring exeStr(modPath);
    const auto pos = exeStr.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        exeStr = exeStr.substr(pos + 1);
    }
    const DWORD pid = GetCurrentProcessId();

    char exeA[MAX_PATH]{};
    size_t converted = 0;
    wcstombs_s(&converted, exeA, exeStr.c_str(), sizeof(exeA) - 1);

    std::ofstream ofs(g_logFilePath, std::ios::app);
    if (ofs.is_open()) {
        ofs << "=== VRPacing " << kAppVersionAnsi << " | Proceso: "
            << exeA << " [PID " << pid << "] ===" << std::endl;
    }
}

void InitLogging() {
    InitLogging(L"");
}

std::wstring GetLogFilePath() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_initialized) {
        return GetDefaultLogPath();
    }
    return g_logFilePath;
}

void LogMessage(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_initialized) {
        InitLogging(L"");
    }

    const auto now = std::chrono::system_clock::now();
    const auto inTimeT = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuffer{};
    localtime_s(&tmBuffer, &inTimeT);

    std::ostringstream ss;
    ss << std::put_time(&tmBuffer, "%Y-%m-%d %H:%M:%S") << " [";

    switch (level) {
    case LogLevel::Info:  ss << "INFO";  break;
    case LogLevel::Warn:  ss << "WARN";  break;
    case LogLevel::Error: ss << "ERROR"; break;
    }
    ss << "] " << message << "\n";

    std::ofstream ofs(g_logFilePath, std::ios::app);
    if (ofs.is_open()) {
        ofs << ss.str();
        ofs.flush();
    }
    OutputDebugStringA(ss.str().c_str());
}

} // namespace vrpacing

#endif