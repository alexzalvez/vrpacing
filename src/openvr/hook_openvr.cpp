#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "minhook/include/MinHook.h"
#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"
#include "openvr/hook_openvr.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_openVrHandlingFrames{ false };

namespace {

using VRGetGenericInterfaceFn = void* (*)(const char*, int*);
VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;

using SubmitFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using WaitGetPosesFn = vr::EVRCompositorError (*)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);

std::mutex g_mutex;
std::unordered_map<void*, WaitGetPosesFn> g_origWaitGetPosesMap;
std::unordered_map<void*, SubmitFn> g_origSubmitMap;
std::unordered_set<std::string> g_hookedInterfaces;

WaitGetPosesFn g_activeOrigWaitGetPoses = nullptr;
SubmitFn g_activeOrigSubmit = nullptr;

RuntimeConfig g_cfg;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;

float g_projCenters[2][2] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };
bool g_projCentersCalculated = false;

struct OpenVROutputTexture {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct OpenVRContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OpenVROutputTexture sharedOutput;
};

OpenVRContext g_ovrCtx;

bool IsOpenXrLayerHandlingD3D11() {
    HMODULE hLayer = GetModuleHandleW(L"VRPacingLayer.dll");
    if (hLayer) {
        using IsActiveFn = bool(__cdecl*)();
        auto fn = reinterpret_cast<IsActiveFn>(GetProcAddress(hLayer, "VRPacingLayer_IsActiveD3D11"));
        if (fn && fn()) {
            return true;
        }
    }
    return g_shm.IsOpenXrD3D11ActiveForCurrentProcess();
}

double QueryOpenVRHardwareHz() {
    if (!g_origGetGenericInterface) return 0.0;
    int err = 0;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(g_origGetGenericInterface(vr::IVRSystem_Version, &err));
    if (!sys) return 0.0;
    vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
    const float freq = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propErr);
    if (propErr == vr::TrackedProp_Success && freq >= 30.0f && freq <= 200.0f) {
        return HzTracker::SnapToCommonHz(static_cast<double>(freq)).value_or(std::round(static_cast<double>(freq)));
    }
    return 0.0;
}

void CalculateOpenVRProjectionCenters() {
    if (g_projCentersCalculated || !g_origGetGenericInterface) return;
    int err = 0;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(g_origGetGenericInterface(vr::IVRSystem_Version, &err));
    if (!sys) return;

    for (int eye = 0; eye < 2; ++eye) {
        float left = -1.0f, right = 1.0f, top = -1.0f, bottom = 1.0f;
        sys->GetProjectionRaw(static_cast<vr::EVREye>(eye), &left, &right, &top, &bottom);

        auto ml = sys->GetEyeToHeadTransform(vr::Eye_Left);
        auto mr = sys->GetEyeToHeadTransform(vr::Eye_Right);
        const float dotForward = ml.m[2][0] * mr.m[2][0] + ml.m[2][1] * mr.m[2][1] + ml.m[2][2] * mr.m[2][2];
        const float clampedDot = (std::max)(-1.0f, (std::min)(1.0f, dotForward));
        const float cantedAngle = std::abs(std::acos(clampedDot) * 0.5f) * (eye == vr::Eye_Right ? -1.0f : 1.0f);
        const float canted = std::tan(cantedAngle);

        if (std::abs(left - right) > 0.001f && std::abs(top - bottom) > 0.001f) {
            g_projCenters[eye][0] = 0.5f * (1.0f + (right + left - 2.0f * canted) / (left - right));
            g_projCenters[eye][1] = 0.5f * (1.0f + (bottom + top) / (top - bottom));
        }
    }
    g_projCentersCalculated = true;
}

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_cfg.enabled = true;
        g_cfg.upscaling.enabled = true;
        g_cfg.upscaling.scale = 2.0f;
        g_cfg.upscaling.radius = 0.60f;
        g_cfg.upscaling.sharpness = 0.0f;
        g_cfg.pacing.enabled = true;
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenVR Hook initialized.");
    });
}

vr::EVRCompositorError HookedWaitGetPoses(void* self,
                                         vr::TrackedDevicePose_t* pRenderPoseArray,
                                         uint32_t unRenderPoseArrayCount,
                                         vr::TrackedDevicePose_t* pGamePoseArray,
                                         uint32_t unGamePoseArrayCount) {
    WaitGetPosesFn origFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_origWaitGetPosesMap.find(self);
        origFn = (it != g_origWaitGetPosesMap.end()) ? it->second : g_activeOrigWaitGetPoses;
    }

    if (!origFn) return vr::VRCompositorError_None;

    if (IsOpenXrLayerHandlingD3D11()) {
        return origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    }

    thread_local bool inWait = false;
    if (inWait) {
        return origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    }
    inWait = true;

    EnsureInit();
    g_shm.SyncConfig(g_cfg);
    VrsFoveatedD3D11::Instance().EndFrame();

    static int64_t s_qpcFreq = 0;
    static int64_t s_lastWaitExitQpc = 0;
    static uint32_t s_calibrationFrames = 0;
    static uint32_t s_sswHalfRateHits = 0;
    static uint32_t s_heavyWaitStreak = 0;
    static double s_panelHz = 90.0;
    static bool s_sswHalfRateActive = false;

    if (s_qpcFreq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s_qpcFreq = f.QuadPart;
    }

    if (s_calibrationFrames == 0) {
        const double hwHz = QueryOpenVRHardwareHz();
        if (hwHz > 0.0) {
            s_panelHz = hwHz;
        }
    }

    const double effectiveHz = s_sswHalfRateActive ? (s_panelHz * 0.5) : s_panelHz;
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled && s_calibrationFrames >= 45) {
        LimitInputs lim{};
        lim.hz = hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        g_limiter.WaitBeforeRuntime();
    }

    g_shm.UpdateTelemetry(L"OpenVR (SteamVR)", hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);

    LARGE_INTEGER beforeWaitQpc{};
    QueryPerformanceCounter(&beforeWaitQpc);

    vr::EVRCompositorError res = origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);

    LARGE_INTEGER afterWaitQpc{};
    QueryPerformanceCounter(&afterWaitQpc);

    if (s_qpcFreq > 0) {
        const double waitMs = static_cast<double>(afterWaitQpc.QuadPart - beforeWaitQpc.QuadPart) * 1000.0 / static_cast<double>(s_qpcFreq);

        if (s_lastWaitExitQpc > 0) {
            const double dt = static_cast<double>(afterWaitQpc.QuadPart - s_lastWaitExitQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.004 && dt < 0.050) {
                const double measuredHz = 1.0 / dt;
                if (s_calibrationFrames < 45) {
                    s_calibrationFrames++;
                    const double halfPanel = s_panelHz * 0.5;
                    if (waitMs > 2.0 && std::abs(measuredHz - halfPanel) <= (halfPanel * 0.04)) {
                        s_sswHalfRateHits++;
                    }
                    if (s_calibrationFrames == 45) {
                        s_sswHalfRateActive = (s_sswHalfRateHits >= 20);
                    }
                } else {
                    if (waitMs > 5.0) {
                        s_heavyWaitStreak++;
                        if (s_heavyWaitStreak >= 45) {
                            s_heavyWaitStreak = 0;
                            s_calibrationFrames = 0;
                            s_sswHalfRateHits = 0;
                            s_sswHalfRateActive = false;
                        }
                    } else {
                        s_heavyWaitStreak = 0;
                    }
                }
            }
        }
    }
    s_lastWaitExitQpc = afterWaitQpc.QuadPart;

    inWait = false;
    return res;
}

vr::EVRCompositorError HookedSubmit(void* self,
                                    vr::EVREye eEye,
                                    const vr::Texture_t* pTexture,
                                    const vr::VRTextureBounds_t* pBounds,
                                    vr::EVRSubmitFlags nSubmitFlags) {
    SubmitFn origFn = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_origSubmitMap.find(self);
        origFn = (it != g_origSubmitMap.end()) ? it->second : g_activeOrigSubmit;
    }

    if (!origFn) return vr::VRCompositorError_None;

    if (IsOpenXrLayerHandlingD3D11()) {
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    thread_local bool inSubmit = false;
    if (inSubmit || !pTexture || !pTexture->handle || pTexture->eType != vr::TextureType_DirectX) {
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    inSubmit = true;
    auto* inTex = reinterpret_cast<ID3D11Texture2D*>(pTexture->handle);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ovrCtx.device) {
            inTex->GetDevice(g_ovrCtx.device.GetAddressOf());
            if (g_ovrCtx.device) {
                g_ovrCtx.device->GetImmediateContext(g_ovrCtx.context.GetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    if (!g_upscaler.Initialize(g_ovrCtx.device.Get())) {
                        g_upscaleStatus = -2;
                    }
                }
            }
        }
    }

    if (!g_ovrCtx.device || !g_ovrCtx.context) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    D3D11_TEXTURE2D_DESC inDesc{};
    inTex->GetDesc(&inDesc);
    g_lastFormat = static_cast<uint32_t>(inDesc.Format);

    static const vr::VRTextureBounds_t defaultBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    const vr::VRTextureBounds_t* b = pBounds ? pBounds : &defaultBounds;

    const bool isFlippedX = (b->uMin > b->uMax);
    const bool isFlippedY = (b->vMin > b->vMax);
    const float uMin = (std::min)(b->uMin, b->uMax);
    const float uMax = (std::max)(b->uMin, b->uMax);
    const float vMin = (std::min)(b->vMin, b->vMax);
    const float vMax = (std::max)(b->vMin, b->vMax);

    const uint32_t cropX = static_cast<uint32_t>(std::lround(uMin * inDesc.Width));
    const uint32_t cropY = static_cast<uint32_t>(std::lround(vMin * inDesc.Height));
    const uint32_t cropW = static_cast<uint32_t>(std::lround((uMax - uMin) * inDesc.Width));
    const uint32_t cropH = static_cast<uint32_t>(std::lround((vMax - vMin) * inDesc.Height));

    if (cropW == 0 || cropH == 0) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    g_lastInW = cropW;
    g_lastInH = cropH;

    CalculateOpenVRProjectionCenters();
    VrsTextureMode vrsMode = VrsTextureMode::SingleEye;
    if (inDesc.ArraySize >= 2) {
        vrsMode = VrsTextureMode::TextureArray;
    } else if (cropW < inDesc.Width * 3 / 4) {
        vrsMode = VrsTextureMode::CombinedSideBySide;
    }
    VrsFoveatedD3D11::Instance().UpdateTarget(
        g_ovrCtx.device.Get(),
        g_ovrCtx.context.Get(),
        g_cfg.ffr.enabled,
        cropW,
        cropH,
        vrsMode,
        g_projCenters[0][0], g_projCenters[0][1],
        g_projCenters[1][0], g_projCenters[1][1]);

    if (!g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    const double s = g_cfg.upscaling.scale;
    uint32_t outW = static_cast<uint32_t>((std::max)(1.0, std::floor(cropW * s)));
    uint32_t outH = static_cast<uint32_t>((std::max)(1.0, std::floor(cropH * s)));

    const uint64_t px = static_cast<uint64_t>(outW) * static_cast<uint64_t>(outH);
    if (px > g_cfg.upscaling.maxOutputPixels) {
        const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(px));
        outW = static_cast<uint32_t>((std::max)(1.0, std::floor(outW * factor)));
        outH = static_cast<uint32_t>((std::max)(1.0, std::floor(outH * factor)));
    }

    g_lastOutW = outW;
    g_lastOutH = outH;

    static std::atomic<bool> s_firstFrameLogged{ false };
    if (!s_firstFrameLogged.exchange(true)) {
        LogMessage(LogLevel::Info, "OpenVR [Submit] First frame: InTex=" + std::to_string(inDesc.Width) + "x" + std::to_string(inDesc.Height) +
                   " FMT=" + std::to_string(inDesc.Format) +
                   " ArraySize=" + std::to_string(inDesc.ArraySize) +
                   " Flags=" + std::to_string(nSubmitFlags) +
                   " Crop=" + std::to_string(cropW) + "x" + std::to_string(cropH) +
                   " -> Out=" + std::to_string(outW) + "x" + std::to_string(outH));
    }

    auto& sharedOut = g_ovrCtx.sharedOutput;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!sharedOut.texture || sharedOut.width != outW || sharedOut.height != outH || sharedOut.format != inDesc.Format) {
            sharedOut.texture.Reset();

            D3D11_TEXTURE2D_DESC outDesc{};
            outDesc.Width = outW;
            outDesc.Height = outH;
            outDesc.MipLevels = 1;
            outDesc.ArraySize = 1;
            outDesc.Format = inDesc.Format;
            outDesc.SampleDesc.Count = 1;
            outDesc.SampleDesc.Quality = 0;
            outDesc.Usage = D3D11_USAGE_DEFAULT;
            outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            outDesc.CPUAccessFlags = 0;
            outDesc.MiscFlags = 0;

            HRESULT hr = g_ovrCtx.device->CreateTexture2D(&outDesc, nullptr, sharedOut.texture.GetAddressOf());
            if (FAILED(hr)) {
                g_upscaleStatus = -1;
                g_lastHResult = hr;
                LogMessage(LogLevel::Error, "OpenVR Hook: Failed to allocate shared output texture. HR=" + std::to_string(hr));
                inSubmit = false;
                return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
            }
            sharedOut.width = outW;
            sharedOut.height = outH;
            sharedOut.format = inDesc.Format;
            LogMessage(LogLevel::Info, "OpenVR: Shared output texture allocated: " + std::to_string(outW) + "x" + std::to_string(outH));
        }
    }

    ID3D11RenderTargetView* nullRTV[1] = { nullptr };
    g_ovrCtx.context->OMSetRenderTargets(1, nullRTV, nullptr);

    UpscaleDispatchDesc dispatch{};
    dispatch.srcRect = { cropX, cropY, cropW, cropH };
    dispatch.dstRect = { 0, 0, outW, outH };
    dispatch.srcArrayIndex = (inDesc.ArraySize > 1) ? ((eEye == vr::Eye_Right) ? 1 : 0) : 0;
    dispatch.dstArrayIndex = 0;
    dispatch.projCenterX = 0.5f;
    dispatch.projCenterY = 0.5f;
    dispatch.radius = g_cfg.upscaling.radius;
    dispatch.sharpness = g_cfg.upscaling.sharpness;
    dispatch.maskEnabled = false;
    dispatch.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

    if (!g_upscaler.Upscale(g_ovrCtx.context.Get(), inTex, sharedOut.texture.Get(), dispatch)) {
        g_upscaleStatus = -3;
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    g_upscaleStatus = 1;
    g_openVrHandlingFrames.store(true, std::memory_order_relaxed);

    vr::VRTextureBounds_t outBounds{};
    outBounds.uMin = isFlippedX ? 1.0f : 0.0f;
    outBounds.uMax = isFlippedX ? 0.0f : 1.0f;
    outBounds.vMin = isFlippedY ? 1.0f : 0.0f;
    outBounds.vMax = isFlippedY ? 0.0f : 1.0f;

    auto* mutableTex = const_cast<vr::Texture_t*>(pTexture);
    void* origHandle = mutableTex->handle;
    mutableTex->handle = sharedOut.texture.Get();
    const uint32_t clearFlags = static_cast<uint32_t>(nSubmitFlags) & ~static_cast<uint32_t>(vr::Submit_GlRenderBuffer);
    vr::EVRCompositorError res = origFn(self, eEye, mutableTex, &outBounds, static_cast<vr::EVRSubmitFlags>(clearFlags));
    mutableTex->handle = origHandle;

    inSubmit = false;
    return res;
}

void HookCompositorInstance(const char* name, void* iface) {
    if (!iface || !name) return;

    std::lock_guard<std::mutex> lock(g_mutex);

    unsigned int version = 0;
    if (::sscanf_s(name, "IVRCompositor_%u", &version) != 1) {
        version = 26;
    }

    uint32_t submitIndex = 5;
    if (version < 9) {
        submitIndex = 6;
    } else if (version < 12) {
        submitIndex = 4;
    }

    void** vtbl = *reinterpret_cast<void***>(iface);
    MH_Initialize();

    WaitGetPosesFn origWait = nullptr;
    if (MH_CreateHook(vtbl[2], reinterpret_cast<void*>(&HookedWaitGetPoses), reinterpret_cast<void**>(&origWait)) == MH_OK) {
        MH_EnableHook(vtbl[2]);
        g_origWaitGetPosesMap[iface] = origWait;
        g_activeOrigWaitGetPoses = origWait;
    } else if (g_activeOrigWaitGetPoses) {
        g_origWaitGetPosesMap[iface] = g_activeOrigWaitGetPoses;
    }

    SubmitFn origSub = nullptr;
    if (MH_CreateHook(vtbl[submitIndex], reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&origSub)) == MH_OK) {
        MH_EnableHook(vtbl[submitIndex]);
        g_origSubmitMap[iface] = origSub;
        g_activeOrigSubmit = origSub;
    } else if (g_activeOrigSubmit) {
        g_origSubmitMap[iface] = g_activeOrigSubmit;
    }

    if (g_hookedInterfaces.find(name) == g_hookedInterfaces.end()) {
        g_hookedInterfaces.insert(name);
        LogMessage(LogLevel::Info, "OpenVR hooks installed on " + std::string(name) +
                   " (WaitGetPoses vtbl[2], Submit vtbl[" + std::to_string(submitIndex) + "])");
    }
}

void* HookedVRGetGenericInterface(const char* name, int* error) {
    void* iface = g_origGetGenericInterface(name, error);
    if (!iface || !name) {
        return iface;
    }

    std::string sName(name);
    if (sName.rfind("IVRCompositor_", 0) == 0) {
        HookCompositorInstance(name, iface);
    } else if (sName.rfind("FnTable:IVRCompositor_", 0) == 0) {
        const char* cppName = name + 8;
        int dummyErr = 0;
        void* cppIface = g_origGetGenericInterface(cppName, &dummyErr);
        if (cppIface) {
            HookCompositorInstance(cppName, cppIface);
        }
    }
    return iface;
}

} // namespace

void InstallOpenVRHooks() {
    static std::atomic<bool> s_installed{ false };
    if (s_installed.load()) return;

    HMODULE mod = GetModuleHandleW(L"openvr_api.dll");
    if (!mod) return;

    auto fn = reinterpret_cast<VRGetGenericInterfaceFn>(GetProcAddress(mod, "VR_GetGenericInterface"));
    if (!fn) return;

    MH_Initialize();
    if (MH_CreateHook(reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&HookedVRGetGenericInterface), reinterpret_cast<void**>(&g_origGetGenericInterface)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<void*>(fn));
        s_installed.store(true);
        LogMessage(LogLevel::Info, "OpenVR VR_GetGenericInterface hook installed.");
    }
}

} // namespace vrpacing

#endif