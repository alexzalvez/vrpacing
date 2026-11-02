#ifdef _WIN32
#include <windows.h>
#include "gfx/vrs_foveated_d3d11.h"
#include "gfx/vrs_foveated_d3d12.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            vrpacing::g_vrsDetaching.store(true, std::memory_order_relaxed);
            // [F9] Log fallback shutdown if D3D12 session was never destroyed via OpenXR API
            vrpacing::VrsFoveatedD3D12::Instance().ShutdownOnProcessDetach();
            break;
    }
    return TRUE;
}
#endif