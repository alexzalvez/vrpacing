#ifdef _WIN32

#include <windows.h>
#include <atomic>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

namespace {

std::atomic<bool> g_stopWatcher{ false };

DWORD WINAPI HookWatcherThread(LPVOID) {
    bool openvrDone = false;
    bool oculusDone = false;

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

        if (openvrDone && oculusDone) {
            break;
        }

        Sleep(150);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl VRPacingHook_RefreshHooks() {
    vrpacing::InstallOpenVRHooks();
    vrpacing::InstallOculusHooks();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            vrpacing::InitLogging();
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook cargado en proceso.");

            vrpacing::InstallOpenVRHooks();
            vrpacing::InstallOculusHooks();

            HANDLE hThread = CreateThread(nullptr, 0, HookWatcherThread, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            g_stopWatcher.store(true, std::memory_order_relaxed);
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook descargado.");
            break;
        default:
            break;
    }
    return TRUE;
}

#endif