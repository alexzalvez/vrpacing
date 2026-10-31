#ifdef _WIN32

#include <windows.h>
#include <atomic>
#include <cwchar>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

namespace {

std::atomic<bool> g_stopWatcher{ false };
HANDLE g_hLoadedEvent = nullptr;
HANDLE g_hReadyEvent = nullptr;

void SignalNamedProcessEvent(const wchar_t* prefix, HANDLE& outHandle) {
    wchar_t evName[96]{};
    swprintf_s(evName, L"%s%lu", prefix, GetCurrentProcessId());
    if (!outHandle) {
        outHandle = CreateEventW(nullptr, TRUE, FALSE, evName);
    }
    if (outHandle) {
        SetEvent(outHandle);
    }
}

DWORD WINAPI HookWatcherThread(LPVOID) {
    SignalNamedProcessEvent(L"Local\\VRPacingHookLoaded_", g_hLoadedEvent);

    bool openvrDone = false;
    bool oculusDone = false;
    bool readySignaled = false;

    while (!g_stopWatcher.load(std::memory_order_relaxed)) {
        if (!openvrDone && GetModuleHandleW(L"openvr_api.dll")) {
            vrpacing::InstallOpenVRHooks();
            openvrDone = true;
        }

        if (!oculusDone && (GetModuleHandleW(L"LibOVRRT64_1.dll") ||
                            GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll") ||
                            GetModuleHandleW(L"LibPVRRT64_1_X.dll"))) {
            vrpacing::InstallOculusHooks();
            oculusDone = true;
        }

        if ((openvrDone || oculusDone) && !readySignaled) {
            SignalNamedProcessEvent(L"Local\\VRPacingHookReady_", g_hReadyEvent);
            readySignaled = true;
        }

        if (openvrDone && oculusDone) {
            break;
        }

        Sleep(100);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl VRPacingHook_RefreshHooks() {
    SignalNamedProcessEvent(L"Local\\VRPacingHookLoaded_", g_hLoadedEvent);
    vrpacing::InstallOpenVRHooks();
    vrpacing::InstallOculusHooks();
    SignalNamedProcessEvent(L"Local\\VRPacingHookReady_", g_hReadyEvent);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            vrpacing::InitLogging();
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook loaded into process.");

            // Instalamos los hooks exclusivamente desde HookWatcherThread (fuera de DllMain)
            // para no ejecutar MinHook (SuspendThread) bajo el Windows Loader Lock.
            HANDLE hThread = CreateThread(nullptr, 0, HookWatcherThread, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            g_stopWatcher.store(true, std::memory_order_relaxed);
            if (g_hReadyEvent) {
                CloseHandle(g_hReadyEvent);
                g_hReadyEvent = nullptr;
            }
            if (g_hLoadedEvent) {
                CloseHandle(g_hLoadedEvent);
                g_hLoadedEvent = nullptr;
            }
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook unloaded.");
            break;
        default:
            break;
    }
    return TRUE;
}

#endif