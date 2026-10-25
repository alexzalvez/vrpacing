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
#include <unordered_set>

#include "minhook/include/MinHook.h"
#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {
namespace {

using VRGetGenericInterfaceFn = void* (*)(const char*, int*);
VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;

using SubmitFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
SubmitFn g_origSubmit = nullptr;

using WaitGetPosesFn = vr::EVRCompositorError (*)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);
WaitGetPosesFn g_origWaitGetPoses = nullptr;

std::mutex g_mutex;
std::unordered_set<std::string> g_hookedInterfaces;

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

struct OpenVROutputTexture {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OpenVRContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OpenVROutputTexture sharedOutput;
};

OpenVRContext g_ovrCtx;

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenVR Hook inicializado.");
    });
}

vr::EVRCompositorError HookedWaitGetPoses(void* self,
                                         vr::TrackedDevicePose_t* pRenderPoseArray,
                                         uint32_t unRenderPoseArrayCount,
                                         vr::TrackedDevicePose_t* pGamePoseArray,
                                         uint32_t unGamePoseArrayCount) {
    thread_local bool inWait = false;
    if (inWait) {
        return g_origWaitGetPoses(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    }
    inWait = true;

    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    const auto hz = g_hzTracker.GetEffectiveHz().value_or(90.0);
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled) {
        LimitInputs lim{};
        lim.hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : hz;
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

    const auto start = std::chrono::steady_clock::now();
    vr::EVRCompositorError res = g_origWaitGetPoses(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    const auto end = std::chrono::steady_clock::now();

    const double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
    if (elapsedMs > 0.5) {
        const double measuredHz = 1000.0 / elapsedMs;
        if (measuredHz > 30.0 && measuredHz < 200.0) {
            g_hzTracker.PushHz(std::round(measuredHz));
        }
    }

    inWait = false;
    return res;
}

vr::EVRCompositorError HookedSubmit(void* self,
                                    vr::EVREye eEye,
                                    const vr::Texture_t* pTexture,
                                    const vr::VRTextureBounds_t* pBounds,
                                    vr::EVRSubmitFlags nSubmitFlags) {
    thread_local bool inSubmit = false;
    if (inSubmit || !pTexture || !pTexture->handle || pTexture->eType != vr::TextureType_DirectX) {
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    if (!g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

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
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
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
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    g_lastInW = cropW;
    g_lastInH = cropH;

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
        LogMessage(LogLevel::Info, "OpenVR [Submit] Primer frame: InTex=" + std::to_string(inDesc.Width) + "x" + std::to_string(inDesc.Height) +
                   " FMT=" + std::to_string(inDesc.Format) +
                   " ArraySize=" + std::to_string(inDesc.ArraySize) +
                   " Crop=" + std::to_string(cropW) + "x" + std::to_string(cropH) +
                   " Bounds=[" + std::to_string(b->uMin) + "," + std::to_string(b->vMin) + " -> " + std::to_string(b->uMax) + "," + std::to_string(b->vMax) + "]" +
                   " -> Out=" + std::to_string(outW) + "x" + std::to_string(outH) +
                   " Scale=" + std::to_string(g_cfg.upscaling.scale));
    }

    auto& sharedOut = g_ovrCtx.sharedOutput;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!sharedOut.texture || sharedOut.width != outW || sharedOut.height != outH) {
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
            outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
            outDesc.CPUAccessFlags = 0;
            outDesc.MiscFlags = 0;

            HRESULT hr = g_ovrCtx.device->CreateTexture2D(&outDesc, nullptr, sharedOut.texture.GetAddressOf());
            if (FAILED(hr)) {
                g_upscaleStatus = -1;
                g_lastHResult = hr;
                LogMessage(LogLevel::Error, "OpenVR Hook: Error al crear textura compartida escalada. HR=" + std::to_string(hr));
                inSubmit = false;
                return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
            }
            sharedOut.width = outW;
            sharedOut.height = outH;
            LogMessage(LogLevel::Info, "OpenVR: Textura unica compartida alocada: " + std::to_string(outW) + "x" + std::to_string(outH));
        }
    }

    // Desvincular RTVs activos para evitar contencion de pipeline con el Compute Shader
    ID3D11RenderTargetView* nullRTV[1] = { nullptr };
    g_ovrCtx.context->OMSetRenderTargets(1, nullRTV, nullptr);

    UpscaleDispatchDesc dispatch{};
    dispatch.srcRect = { cropX, cropY, cropW, cropH };
    dispatch.dstRect = { 0, 0, outW, outH };
    dispatch.projCenterX = 0.5f;
    dispatch.projCenterY = 0.5f;
    dispatch.radius = g_cfg.upscaling.radius;
    dispatch.sharpness = g_cfg.upscaling.sharpness;
    dispatch.maskEnabled = g_cfg.mask.enabled;
    dispatch.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

    if (!g_upscaler.Upscale(g_ovrCtx.context.Get(), inTex, sharedOut.texture.Get(), dispatch)) {
        g_upscaleStatus = -3;
        inSubmit = false;
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    g_upscaleStatus = 1;
    vr::Texture_t replacedTex = *pTexture;
    replacedTex.handle = sharedOut.texture.Get();

    vr::VRTextureBounds_t outBounds{};
    outBounds.uMin = isFlippedX ? 1.0f : 0.0f;
    outBounds.uMax = isFlippedX ? 0.0f : 1.0f;
    outBounds.vMin = isFlippedY ? 1.0f : 0.0f;
    outBounds.vMax = isFlippedY ? 0.0f : 1.0f;

    vr::EVRCompositorError res = g_origSubmit(self, eEye, &replacedTex, &outBounds, nSubmitFlags);
    inSubmit = false;
    return res;
}

void* HookedVRGetGenericInterface(const char* name, int* error) {
    void* iface = g_origGetGenericInterface(name, error);
    if (!iface || !name) {
        return iface;
    }

    if (std::strstr(name, "IVRCompositor")) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hookedInterfaces.find(name) == g_hookedInterfaces.end()) {
            g_hookedInterfaces.insert(name);

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

            if (!g_origWaitGetPoses) {
                MH_CreateHook(vtbl[2], reinterpret_cast<void*>(&HookedWaitGetPoses), reinterpret_cast<void**>(&g_origWaitGetPoses));
                MH_EnableHook(vtbl[2]);
            }

            if (!g_origSubmit) {
                MH_CreateHook(vtbl[submitIndex], reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_origSubmit));
                MH_EnableHook(vtbl[submitIndex]);
            }

            LogMessage(LogLevel::Info, "OpenVR hooks instalados en " + std::string(name) +
                       " (WaitGetPoses vtbl[2], Submit vtbl[" + std::to_string(submitIndex) + "])");
        }
    }
    return iface;
}

} // namespace

void InstallOpenVRHooks() {
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