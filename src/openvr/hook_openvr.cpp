#ifdef _WIN32

#include <windows.h>

#include <cstring>

#include "MinHook.h"

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "vrpacing/hz_tracker.h"

namespace vrpacing {
namespace {

using VRGetGenericInterfaceFn = void* (__cdecl*)(const char*, int*);
using SubmitFn = int(__thiscall*)(void*, int, const void*, const void*, int);
using WaitGetPosesFn = int(__thiscall*)(void*, void*, uint32_t, void*, uint32_t);

VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;
SubmitFn g_origSubmit = nullptr;
WaitGetPosesFn g_origWaitGetPoses = nullptr;

FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg;
uint64_t g_submitCalls = 0;

int __fastcall HookedWaitGetPoses(void* self, void*, void* renderPoses, uint32_t renderCount, void* gamePoses, uint32_t gameCount) {
    if (g_cfg.pacing.enabled) {
        const double hz = g_hzTracker.GetEffectiveHz().value_or(90.0);
        LimitInputs lim{};
        lim.hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        g_limiter.SetTargetFps(vrpacing::ComputeTargetFps(lim));
        g_limiter.WaitBeforeRuntime();
    }
    return g_origWaitGetPoses(self, renderPoses, renderCount, gamePoses, gameCount);
}

int __fastcall HookedSubmit(void* self, void*, int eye, const void* texture, const void* bounds, int flags) {
    (void)eye;
    ++g_submitCalls;
    if ((g_submitCalls % 2) == 0) {
        // Cuenta ciclos estéreo completos
    }
    return g_origSubmit(self, eye, texture, bounds, flags);
}

void* __cdecl HookedVRGetGenericInterface(const char* name, int* error) {
    void* iface = g_origGetGenericInterface(name, error);
    if (!iface || !name) {
        return iface;
    }

    if (strstr(name, "IVRCompositor")) {
        void** vtbl = *reinterpret_cast<void***>(iface);
        if (vtbl && !g_origSubmit) {
            g_origSubmit = reinterpret_cast<SubmitFn>(vtbl[5]); // índice aproximado según versión
            g_origWaitGetPoses = reinterpret_cast<WaitGetPosesFn>(vtbl[6]);
            MH_CreateHook(reinterpret_cast<void*>(g_origSubmit), reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_origSubmit));
            MH_CreateHook(reinterpret_cast<void*>(g_origWaitGetPoses), reinterpret_cast<void*>(&HookedWaitGetPoses), reinterpret_cast<void**>(&g_origWaitGetPoses));
            MH_EnableHook(reinterpret_cast<void*>(g_origSubmit));
            MH_EnableHook(reinterpret_cast<void*>(g_origWaitGetPoses));
            LogMessage(LogLevel::Info, "Hooks OpenVR compositor instalados.");
        }
    }

    return iface;
}

} // namespace

void InstallOpenVRHooks() {
    InitLogging();
    g_cfg = LoadConfig();
    g_limiter.Initialize();

    HMODULE mod = GetModuleHandleW(L"openvr_api.dll");
    if (!mod) return;

    auto fn = reinterpret_cast<VRGetGenericInterfaceFn>(GetProcAddress(mod, "VR_GetGenericInterface"));
    if (!fn) return;

    MH_Initialize();
    MH_CreateHook(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&HookedVRGetGenericInterface), reinterpret_cast<void**>(&g_origGetGenericInterface));
    MH_EnableHook(reinterpret_cast<void*>(fn));
    LogMessage(LogLevel::Info, "Hook OpenVR VR_GetGenericInterface instalado.");
}

} // namespace vrpacing

#endif
