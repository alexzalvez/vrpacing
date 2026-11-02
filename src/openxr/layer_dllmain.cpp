#ifdef _WIN32
#include <windows.h>
#include "gfx/vrs_foveated_d3d11.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            vrpacing::g_vrsDetaching.store(true, std::memory_order_relaxed);
            break;
    }
    return TRUE;
}
#endif