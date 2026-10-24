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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "MinHook.h"
#include <openvr.h>

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {
namespace {

using VRGetGenericInterfaceFn = void* (*)(const char*, int*);
using SubmitFn = vr::EVRCompositorError(*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using WaitGetPosesFn = vr::EVRCompositorError(*)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);

VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;
SubmitFn g_origSubmit = nullptr;
WaitGetPosesFn g_origWaitGetPoses = nullptr;

std::mutex g_mutex;
std::unordered_set<std::string> g_hookedInterfaces;

FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg;
NisUpscalerD3D11 g_upscaler;

struct OpenVREyeOutput {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct OpenVRContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OpenVREyeOutput outputs[2];
};

OpenVRContext g_ovrCtx;

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
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

    vr::EVRCompositorError err = g_origWaitGetPoses(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    inWait = false;
    return err;
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
            inTex->GetDevice(g_ovrCtx.device.ReleaseAndGetAddressOf());
            if (g_ovrCtx.device) {
                g_ovrCtx.device->GetImmediateContext(g_ovrCtx.context.ReleaseAndGetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    g_upscaler.Initialize(g_ovrCtx.device.Get());
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

    static const vr::VRTextureBounds_t defaultBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    const vr::VRTextureBounds_t* b = pBounds ? pBounds : &defaultBounds;

    const bool isFlippedX = b->uMin > b->uMax;
    const bool isFlippedY = b->vMin > b->vMax;

    const float u0 = (std::min)(b->uMin, b->uMax);
    const float u1 = (std::max)(b->uMin, b->uMax);
    const float v0 = (std::min)(b->vMin, b->vMax);
    const float v1 = (std::max)(b->vMin, b->vMax);

    const uint32_t cropX = static_cast<uint32_t>(std::lround(inDesc.Width * u0));
    const uint32_t cropY = static_cast<uint32_t>(std::lround(inDesc.Height * v0));
    const uint32_t cropW = static_cast<uint32_t>(std::lround(inDesc.Width * (u1 - u0)));
    const uint32_t cropH = static_cast<uint32_t>(std::lround(inDesc.Height * (v1 - v0)));

    if (cropW == 0 || cropH == 0) {
        inSubmit = false;
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
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

    const int eyeIdx = (eEye == vr::Eye_Right) ? 1 : 0;
    auto& eyeOut = g_ovrCtx.outputs[eyeIdx];

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!eyeOut.texture || eyeOut.width != outW || eyeOut.height != outH) {
            eyeOut.texture.Reset();

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

            HRESULT hr = g_ovrCtx.device->CreateTexture2D(&outDesc, nullptr, eyeOut.texture.GetAddressOf());
            if (FAILED(hr)) {
                LogMessage(LogLevel::Error, "OpenVR Hook: Error al crear textura de salida escalada.");
                inSubmit = false;
                return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
            }
            eyeOut.width = outW;
            eyeOut.height = outH;
            LogMessage(LogLevel::Info, "OpenVR output texture creada [ojo " + std::to_string(eyeIdx) + "]: " +
                       std::to_string(outW) + "x" + std::to_string(outH));
        }
    }

    UpscaleDispatchDesc dispatch{};
    dispatch.srcRect = { cropX, cropY, cropW, cropH };
    dispatch.dstRect = { 0, 0, outW, outH };
    dispatch.projCenterX = 0.5f;
    dispatch.projCenterY = 0.5f;
    dispatch.radius = g_cfg.upscaling.radius;
    dispatch.sharpness = g_cfg.upscaling.sharpness;
    dispatch.maskEnabled = g_cfg.mask.enabled;
    dispatch.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

    if (!g_upscaler.Upscale(g_ovrCtx.context.Get(), inTex, eyeOut.texture.Get(), dispatch)) {
        inSubmit = false;
        return g_origSubmit(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    vr::Texture_t replacedTex = *pTexture;
    replacedTex.handle = eyeOut.texture.Get();

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
            } else {
                submitIndex = 5;
            }

            void** vtbl = *reinterpret_cast<void***>(iface);
            if (vtbl) {
                void* targetWaitGetPoses = vtbl[2];
                void* targetSubmit = vtbl[submitIndex];

                if (!g_origWaitGetPoses) {
                    MH_CreateHook(targetWaitGetPoses, reinterpret_cast<void*>(&HookedWaitGetPoses), reinterpret_cast<void**>(&g_origWaitGetPoses));
                    MH_EnableHook(targetWaitGetPoses);
                }

                if (!g_origSubmit) {
                    MH_CreateHook(targetSubmit, reinterpret_cast<void*>(&HookedSubmit), reinterpret_cast<void**>(&g_origSubmit));
                    MH_EnableHook(targetSubmit);
                }

                LogMessage(LogLevel::Info, "OpenVR hooks instalados en " + std::string(name) +
                           " (WaitGetPoses vtbl[2], Submit vtbl[" + std::to_string(submitIndex) + "])");
            }
        }
    }

    return iface;
}

} // namespace

void InstallOpenVRHooks() {
    EnsureInit();

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