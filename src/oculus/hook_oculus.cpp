#ifdef _WIN32

#include <windows.h>

#include "MinHook.h"

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "third_party/oculus/ovr_minimal.h"
#include "vrpacing/hz_tracker.h"

namespace vrpacing {
namespace {

using ovrSubmitFrameFn = ovrResult(__cdecl*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int);
using ovrWaitToBeginFrameFn = ovrResult(__cdecl*)(ovrSession, long long);
using ovrGetFovTextureSizeFn = ovrSizei(__cdecl*)(ovrSession, ovrEyeType, ovrFovPort, float);

ovrSubmitFrameFn g_origSubmit = nullptr;
ovrWaitToBeginFrameFn g_origWaitToBeginFrame = nullptr;
ovrGetFovTextureSizeFn g_origGetFovTextureSize = nullptr;

FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg;

ovrResult __cdecl Hooked_ovrWaitToBeginFrame(ovrSession session, long long frameIndex) {
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
    return g_origWaitToBeginFrame(session, frameIndex);
}

ovrSizei __cdecl Hooked_ovrGetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov, float pixelsPerDisplayPixel) {
    return g_origGetFovTextureSize(session, eye, fov, pixelsPerDisplayPixel);
}

ovrResult __cdecl Hooked_ovrSubmitFrame(ovrSession session,
                                        long long frameIndex,
                                        const ovrViewScaleDesc* viewScaleDesc,
                                        ovrLayerHeader const* const* layerPtrList,
                                        unsigned int layerCount) {
    return g_origSubmit(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
}

} // namespace

void InstallOculusHooks() {
    InitLogging();
    g_cfg = LoadConfig();
    g_limiter.Initialize();

    HMODULE mod = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!mod) return;

    auto submit = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_SubmitFrame"));
    auto waitBegin = reinterpret_cast<ovrWaitToBeginFrameFn>(GetProcAddress(mod, "ovr_WaitToBeginFrame"));
    auto getSize = reinterpret_cast<ovrGetFovTextureSizeFn>(GetProcAddress(mod, "ovr_GetFovTextureSize"));
    if (!submit || !waitBegin || !getSize) return;

    MH_Initialize();
    MH_CreateHook(reinterpret_cast<void*>(submit), reinterpret_cast<void*>(&Hooked_ovrSubmitFrame), reinterpret_cast<void**>(&g_origSubmit));
    MH_CreateHook(reinterpret_cast<void*>(waitBegin), reinterpret_cast<void*>(&Hooked_ovrWaitToBeginFrame), reinterpret_cast<void**>(&g_origWaitToBeginFrame));
    MH_CreateHook(reinterpret_cast<void*>(getSize), reinterpret_cast<void*>(&Hooked_ovrGetFovTextureSize), reinterpret_cast<void**>(&g_origGetFovTextureSize));

    MH_EnableHook(reinterpret_cast<void*>(submit));
    MH_EnableHook(reinterpret_cast<void*>(waitBegin));
    MH_EnableHook(reinterpret_cast<void*>(getSize));

    LogMessage(LogLevel::Info, "Hooks Oculus instalados.");
}

} // namespace vrpacing

#endif
