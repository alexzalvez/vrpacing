#ifdef _WIN32

#include <windows.h>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            vrpacing::InitLogging();
            vrpacing::InstallOpenVRHooks();
            vrpacing::InstallOculusHooks();
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook cargado.");
            break;
        case DLL_PROCESS_DETACH:
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingHook descargado.");
            break;
        default:
            break;
    }
    return TRUE;
}

#endif
