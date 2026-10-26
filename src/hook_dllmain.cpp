#ifdef _WIN32

#include <windows.h>
#include <chrono>
#include <thread>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

namespace {

DWORD WINAPI HookWatcherThread(LPVOID) {
    const auto startTime = std::chrono::steady_clock::now();
    bool openvrDone = false;
    bool oculusDone = false;

    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < 25) {
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

        Sleep(200);
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            vrpacing::InitLogging();
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook cargado en proceso.");

            // Intento inmediato sincronizado
            vrpacing::InstallOpenVRHooks();
            vrpacing::InstallOculusHooks();

            // Vigilante en segundo plano para DLLs cargadas tardiamente por el motor (UE4 / Unity)
            HANDLE hThread = CreateThread(nullptr, 0, HookWatcherThread, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook descargado.");
            break;
        default:
            break;
    }
    return TRUE;
}

#endif