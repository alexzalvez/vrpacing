#ifdef _WIN32

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

namespace vrpacing {
bool InstallOpenVRHooksWithStatus();
}

namespace {

std::atomic<bool> g_stopWatcher{ false };
std::mutex g_lifecycleMutex;
HANDLE g_hLoadedEvent = nullptr;
HANDLE g_hReadyEvent = nullptr;
HANDLE g_hFailedEvent = nullptr;
bool g_modulePinned = false;

uint64_t GetCurrentProcessCreationTime() {
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = ftCreate.dwLowDateTime;
        uli.HighPart = ftCreate.dwHighDateTime;
        return uli.QuadPart;
    }
    return 0;
}

void PinSelfModuleOnce() {
    if (g_modulePinned) return;
    HMODULE hPin = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&PinSelfModuleOnce), &hPin)) {
        g_modulePinned = true;
    }
}

void SignalNamedProcessEventLocked(const wchar_t* prefix, HANDLE& outHandle) {
    const DWORD pid = GetCurrentProcessId();
    const uint64_t createTime = GetCurrentProcessCreationTime();
    wchar_t evName[128]{};
    swprintf_s(evName, L"%s%lu_%llX", prefix, pid, static_cast<unsigned long long>(createTime));
    if (!outHandle) {
        outHandle = CreateEventW(nullptr, TRUE, FALSE, evName);
    }
    if (outHandle) {
        SetEvent(outHandle);
    }
}

bool TryInstallAvailableHooksLocked(bool& openvrDone, bool& oculusDone, int& openvrFailCount) {
    bool anyNewlyReady = false;

    if (!openvrDone && GetModuleHandleW(L"openvr_api.dll")) {
        PinSelfModuleOnce();
        if (vrpacing::InstallOpenVRHooksWithStatus()) {
            openvrDone = true;
            anyNewlyReady = true;
        } else {
            openvrFailCount++;
        }
    }

    if (!oculusDone && (GetModuleHandleW(L"LibOVRRT64_1.dll") ||
                        GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll") ||
                        GetModuleHandleW(L"LibPVRRT64_1_X.dll"))) {
        PinSelfModuleOnce();
        vrpacing::InstallOculusHooks();
        oculusDone = true;
        anyNewlyReady = true;
    }

    return anyNewlyReady;
}

DWORD WINAPI HookWatcherThread(LPVOID) {
    {
        std::lock_guard<std::mutex> lock(g_lifecycleMutex);
        SignalNamedProcessEventLocked(L"Local\\VRPacingHookLoaded_", g_hLoadedEvent);
    }

    bool openvrDone = false;
    bool oculusDone = false;
    bool readySignaled = false;
    bool failedSignaled = false;
    int openvrFailCount = 0;

    while (!g_stopWatcher.load(std::memory_order_relaxed)) {
        {
            std::lock_guard<std::mutex> lock(g_lifecycleMutex);
            TryInstallAvailableHooksLocked(openvrDone, oculusDone, openvrFailCount);

            if ((openvrDone || oculusDone) && !readySignaled) {
                SignalNamedProcessEventLocked(L"Local\\VRPacingHookReady_", g_hReadyEvent);
                readySignaled = true;
            } else if (!readySignaled && !failedSignaled && openvrFailCount >= 10) {
                SignalNamedProcessEventLocked(L"Local\\VRPacingHookFailed_", g_hFailedEvent);
                failedSignaled = true;
                vrpacing::LogMessage(vrpacing::LogLevel::Error,
                                     "HookWatcherThread: OpenVR hook installation failed repeatedly after 10 attempts.");
            }
        }

        if ((openvrDone && oculusDone) || failedSignaled) {
            break;
        }

        Sleep(100);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl VRPacingHook_RefreshHooks() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    SignalNamedProcessEventLocked(L"Local\\VRPacingHookLoaded_", g_hLoadedEvent);

    bool openvrDone = false;
    bool oculusDone = false;
    int openvrFailCount = 0;
    TryInstallAvailableHooksLocked(openvrDone, oculusDone, openvrFailCount);

    if (openvrDone || oculusDone) {
        SignalNamedProcessEventLocked(L"Local\\VRPacingHookReady_", g_hReadyEvent);
    } else if (openvrFailCount > 0) {
        SignalNamedProcessEventLocked(L"Local\\VRPacingHookFailed_", g_hFailedEvent);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            vrpacing::InitLogging();
            vrpacing::LogMessage(vrpacing::LogLevel::Info,
                                 std::string("VRPacingHook loaded into process (Build: ") + __DATE__ + " " + __TIME__ + ").");

            HANDLE hThread = CreateThread(nullptr, 0, HookWatcherThread, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            g_stopWatcher.store(true, std::memory_order_relaxed);
            if (lpReserved == nullptr) {
                std::lock_guard<std::mutex> lock(g_lifecycleMutex);
                if (g_hFailedEvent) {
                    CloseHandle(g_hFailedEvent);
                    g_hFailedEvent = nullptr;
                }
                if (g_hReadyEvent) {
                    CloseHandle(g_hReadyEvent);
                    g_hReadyEvent = nullptr;
                }
                if (g_hLoadedEvent) {
                    CloseHandle(g_hLoadedEvent);
                    g_hLoadedEvent = nullptr;
                }
            }
            vrpacing::LogMessage(vrpacing::LogLevel::Info,
                                 lpReserved ? "VRPacingHook detached (process terminating)."
                                            : "VRPacingHook detached (explicit unload).");
            break;
        default:
            break;
    }
    return TRUE;
}

#endif