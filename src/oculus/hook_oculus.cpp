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
#include <memory>
#include <string>
#include <vector>

#include "MinHook.h"

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "third_party/oculus/ovr_minimal.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {
namespace {

using ovrInitializeFn = ovrResult(__cdecl*)(const ovrInitParams*);
using ovrSubmitFrameFn = ovrResult(__cdecl*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int);
using ovrWaitToBeginFrameFn = ovrResult(__cdecl*)(ovrSession, long long);
using ovrGetFovTextureSizeFn = ovrSizei(__cdecl*)(ovrSession, ovrEyeType, ovrFovPort, float);

using ovrCreateTextureSwapChainDXFn = ovrResult(__cdecl*)(ovrSession, IUnknown*, const ovrTextureSwapChainDesc*, ovrTextureSwapChain*);
using ovrGetTextureSwapChainLengthFn = ovrResult(__cdecl*)(ovrSession, ovrTextureSwapChain, int*);
using ovrGetTextureSwapChainBufferDXFn = ovrResult(__cdecl*)(ovrSession, ovrTextureSwapChain, int, IID, void**);
using ovrGetTextureSwapChainDescFn = ovrResult(__cdecl*)(ovrSession, ovrTextureSwapChain, ovrTextureSwapChainDesc*);
using ovrGetTextureSwapChainCurrentIndexFn = ovrResult(__cdecl*)(ovrSession, ovrTextureSwapChain, int*);
using ovrCommitTextureSwapChainFn = ovrResult(__cdecl*)(ovrSession, ovrTextureSwapChain);
using ovrDestroyTextureSwapChainFn = void(__cdecl*)(ovrSession, ovrTextureSwapChain);

ovrInitializeFn g_origInitialize = nullptr;
ovrSubmitFrameFn g_origSubmit = nullptr;
ovrSubmitFrameFn g_origSubmit2 = nullptr;
ovrWaitToBeginFrameFn g_origWaitToBeginFrame = nullptr;
ovrGetFovTextureSizeFn g_origGetFovTextureSize = nullptr;

ovrCreateTextureSwapChainDXFn g_ovrCreateTextureSwapChainDX = nullptr;
ovrGetTextureSwapChainLengthFn g_ovrGetTextureSwapChainLength = nullptr;
ovrGetTextureSwapChainBufferDXFn g_ovrGetTextureSwapChainBufferDX = nullptr;
ovrGetTextureSwapChainDescFn g_ovrGetTextureSwapChainDesc = nullptr;
ovrGetTextureSwapChainCurrentIndexFn g_ovrGetTextureSwapChainCurrentIndex = nullptr;
ovrCommitTextureSwapChainFn g_ovrCommitTextureSwapChain = nullptr;
ovrDestroyTextureSwapChainFn g_ovrDestroyTextureSwapChain = nullptr;

uint32_t g_oculusVersion = 32;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg;
NisUpscalerD3D11 g_upscaler;

struct OculusOutputSwapchain {
    ovrTextureSwapChain chain = nullptr;
    int length = 0;
    int width = 0;
    int height = 0;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
};

struct OculusSessionContext {
    ovrSession session = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ovrTextureSwapChain lastInputChains[ovrEye_Count] = { nullptr, nullptr };
    OculusOutputSwapchain outputChains[ovrEye_Count];
};

std::unique_ptr<OculusSessionContext> g_oculusCtx;

void CleanupOutputChains(OculusSessionContext& ctx) {
    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        if (ctx.outputChains[eye].chain && g_ovrDestroyTextureSwapChain) {
            if (eye == 1 && ctx.outputChains[1].chain == ctx.outputChains[0].chain) {
                ctx.outputChains[1].chain = nullptr;
            } else {
                g_ovrDestroyTextureSwapChain(ctx.session, ctx.outputChains[eye].chain);
                ctx.outputChains[eye].chain = nullptr;
            }
        }
        ctx.outputChains[eye].textures.clear();
        ctx.outputChains[eye].length = 0;
        ctx.outputChains[eye].width = 0;
        ctx.outputChains[eye].height = 0;
        ctx.lastInputChains[eye] = nullptr;
    }
}

bool EnsureOutputChains(ovrSession session, ovrTextureSwapChain leftInput, ovrTextureSwapChain rightInput) {
    if (!g_oculusCtx || g_oculusCtx->session != session) {
        if (g_oculusCtx) {
            CleanupOutputChains(*g_oculusCtx);
        }
        g_oculusCtx = std::make_unique<OculusSessionContext>();
        g_oculusCtx->session = session;
    }

    auto& ctx = *g_oculusCtx;

    const bool chainsChanged = (leftInput != ctx.lastInputChains[0]) || (rightInput != ctx.lastInputChains[1]);
    if (!chainsChanged && ctx.outputChains[0].chain != nullptr) {
        return true;
    }

    CleanupOutputChains(ctx);

    ovrTextureSwapChain inChains[ovrEye_Count] = { leftInput, rightInput };
    ctx.lastInputChains[0] = leftInput;
    ctx.lastInputChains[1] = rightInput;

    const bool singleTexture = (rightInput == nullptr || rightInput == leftInput);

    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        if (eye == 1 && singleTexture) {
            ctx.outputChains[1] = ctx.outputChains[0];
            break;
        }

        ovrTextureSwapChain curIn = inChains[eye];
        if (!curIn) continue;

        ovrTextureSwapChainDesc desc{};
        if (g_ovrGetTextureSwapChainDesc(session, curIn, &desc) < 0) {
            LogMessage(LogLevel::Error, "Oculus Hook: ovr_GetTextureSwapChainDesc fallo.");
            return false;
        }

        if (!ctx.device) {
            ComPtr<ID3D11Texture2D> inTex0;
            if (g_ovrGetTextureSwapChainBufferDX(session, curIn, 0, IID_PPV_ARGS(inTex0.GetAddressOf())) >= 0 && inTex0) {
                inTex0->GetDevice(ctx.device.ReleaseAndGetAddressOf());
                if (ctx.device) {
                    ctx.device->GetImmediateContext(ctx.context.ReleaseAndGetAddressOf());
                    if (!g_upscaler.IsInitialized()) {
                        g_upscaler.Initialize(ctx.device.Get());
                    }
                }
            }
        }

        if (!ctx.device) {
            LogMessage(LogLevel::Error, "Oculus Hook: No se pudo capturar ID3D11Device.");
            return false;
        }

        const double s = (g_cfg.upscaling.scale > 1.0f) ? g_cfg.upscaling.scale : 1.0f;
        int outW = static_cast<int>((std::max)(1.0, std::floor(desc.Width * s)));
        int outH = static_cast<int>((std::max)(1.0, std::floor(desc.Height * s)));

        const uint64_t px = static_cast<uint64_t>(outW) * static_cast<uint64_t>(outH);
        if (px > g_cfg.upscaling.maxOutputPixels) {
            const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(px));
            outW = static_cast<int>((std::max)(1.0, std::floor(outW * factor)));
            outH = static_cast<int>((std::max)(1.0, std::floor(outH * factor)));
        }

        ovrTextureSwapChainDesc outDesc = desc;
        outDesc.Width = outW;
        outDesc.Height = outH;
        outDesc.SampleCount = 1;
        outDesc.MipLevels = 1;
        outDesc.BindFlags = ovrTextureBind_DX_UnorderedAccess | ovrTextureBind_DX_RenderTarget;
        outDesc.MiscFlags = ovrTextureMisc_AutoGenerateMips;

        ovrTextureSwapChain outChain = nullptr;
        if (g_ovrCreateTextureSwapChainDX(session, ctx.device.Get(), &outDesc, &outChain) < 0 || !outChain) {
            LogMessage(LogLevel::Error, "Oculus Hook: ovr_CreateTextureSwapChainDX fallo.");
            return false;
        }

        int len = 0;
        g_ovrGetTextureSwapChainLength(session, outChain, &len);

        ctx.outputChains[eye].chain = outChain;
        ctx.outputChains[eye].length = len;
        ctx.outputChains[eye].width = outW;
        ctx.outputChains[eye].height = outH;
        ctx.outputChains[eye].textures.reserve(len);

        for (int i = 0; i < len; ++i) {
            ComPtr<ID3D11Texture2D> tex;
            g_ovrGetTextureSwapChainBufferDX(session, outChain, i, IID_PPV_ARGS(tex.GetAddressOf()));
            ctx.outputChains[eye].textures.push_back(tex);
        }

        LogMessage(LogLevel::Info, "Oculus Output Swapchain [ojo " + std::to_string(eye) + "] creado: " +
                   std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                   " -> " + std::to_string(outW) + "x" + std::to_string(outH));
    }

    return true;
}

void ProcessAndUpscaleLayer(ovrSession session,
                            ovrTextureSwapChain inLeft,
                            ovrTextureSwapChain inRight,
                            ovrRecti inViewport[ovrEye_Count],
                            const ovrFovPort inFov[ovrEye_Count],
                            ovrTextureSwapChain outColor[ovrEye_Count],
                            ovrRecti outViewport[ovrEye_Count]) {
    if (!EnsureOutputChains(session, inLeft, inRight)) {
        outColor[0] = inLeft;
        outColor[1] = inRight;
        outViewport[0] = inViewport[0];
        outViewport[1] = inViewport[1];
        return;
    }

    auto& ctx = *g_oculusCtx;
    const bool singleTexture = (inRight == nullptr || inRight == inLeft);
    bool anySuccess = false;

    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        ovrTextureSwapChain curIn = (eye == 1 && singleTexture) ? inLeft : (eye == 0 ? inLeft : inRight);
        if (!curIn) continue;

        int inLen = 0;
        g_ovrGetTextureSwapChainLength(session, curIn, &inLen);
        if (inLen <= 0) continue;

        int inIdx = 0;
        g_ovrGetTextureSwapChainCurrentIndex(session, curIn, &inIdx);
        inIdx = (inIdx - 1 + inLen) % inLen;

        ComPtr<ID3D11Texture2D> inTex;
        if (g_ovrGetTextureSwapChainBufferDX(session, curIn, inIdx, IID_PPV_ARGS(inTex.GetAddressOf())) < 0 || !inTex) {
            continue;
        }

        int outIdx = 0;
        g_ovrGetTextureSwapChainCurrentIndex(session, ctx.outputChains[eye].chain, &outIdx);
        if (outIdx >= ctx.outputChains[eye].length) outIdx = 0;

        ID3D11Texture2D* outTex = ctx.outputChains[eye].textures[outIdx].Get();
        if (!outTex) continue;

        ovrTextureSwapChainDesc inDesc{};
        g_ovrGetTextureSwapChainDesc(session, curIn, &inDesc);

        const double sx = inDesc.Width ? static_cast<double>(ctx.outputChains[eye].width) / inDesc.Width : 1.0;
        const double sy = inDesc.Height ? static_cast<double>(ctx.outputChains[eye].height) / inDesc.Height : 1.0;

        ViewportRect srcR{};
        srcR.x = inViewport[eye].Pos.x;
        srcR.y = inViewport[eye].Pos.y;
        srcR.width = inViewport[eye].Size.w;
        srcR.height = inViewport[eye].Size.h;

        ViewportRect dstR{};
        dstR.x = static_cast<uint32_t>(std::lround(srcR.x * sx));
        dstR.y = static_cast<uint32_t>(std::lround(srcR.y * sy));
        dstR.width = static_cast<uint32_t>(std::lround(srcR.width * sx));
        dstR.height = static_cast<uint32_t>(std::lround(srcR.height * sy));

        float projX = 0.5f;
        float projY = 0.5f;
        if (inFov) {
            const float denomX = inFov[eye].RightTan + inFov[eye].LeftTan;
            if (std::abs(denomX) > 0.0001f) {
                projX = 0.5f * (1.0f + (inFov[eye].RightTan - inFov[eye].LeftTan) / denomX);
            }
            const float denomY = inFov[eye].DownTan + inFov[eye].UpTan;
            if (std::abs(denomY) > 0.0001f) {
                projY = 0.5f * (1.0f + (inFov[eye].DownTan - inFov[eye].UpTan) / denomY);
            }
        }

        UpscaleDispatchDesc desc{};
        desc.srcRect = srcR;
        desc.dstRect = dstR;
        desc.projCenterX = projX;
        desc.projCenterY = projY;
        desc.radius = g_cfg.upscaling.radius;
        desc.sharpness = g_cfg.upscaling.sharpness;
        desc.maskEnabled = g_cfg.mask.enabled;
        desc.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

        if (g_upscaler.Upscale(ctx.context.Get(), inTex.Get(), outTex, desc)) {
            outColor[eye] = ctx.outputChains[eye].chain;
            outViewport[eye].Pos.x = static_cast<int>(dstR.x);
            outViewport[eye].Pos.y = static_cast<int>(dstR.y);
            outViewport[eye].Size.w = static_cast<int>(dstR.width);
            outViewport[eye].Size.h = static_cast<int>(dstR.height);
            anySuccess = true;
        } else {
            outColor[eye] = curIn;
            outViewport[eye] = inViewport[eye];
        }
    }

    if (anySuccess) {
        g_ovrCommitTextureSwapChain(session, ctx.outputChains[0].chain);
        if (!singleTexture && ctx.outputChains[1].chain && ctx.outputChains[1].chain != ctx.outputChains[0].chain) {
            g_ovrCommitTextureSwapChain(session, ctx.outputChains[1].chain);
        }
    }
}

ovrResult __cdecl Hooked_ovrInitialize(const ovrInitParams* params) {
    if (params) {
        g_oculusVersion = params->RequestedMinorVersion;
        LogMessage(LogLevel::Info, "Oculus Hook: Inicializado con versión minor " + std::to_string(g_oculusVersion));
    }
    return g_origInitialize ? g_origInitialize(params) : 0;
}

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
    return g_origWaitToBeginFrame ? g_origWaitToBeginFrame(session, frameIndex) : 0;
}

ovrSizei __cdecl Hooked_ovrGetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov, float pixelsPerDisplayPixel) {
    return g_origGetFovTextureSize(session, eye, fov, pixelsPerDisplayPixel);
}

ovrResult __cdecl Hooked_ovrSubmitFrame(ovrSession session,
                                       long long frameIndex,
                                       const ovrViewScaleDesc* viewScaleDesc,
                                       ovrLayerHeader const* const* layerPtrList,
                                       unsigned int layerCount) {
    if (!layerPtrList || layerCount == 0 || !g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        return g_origSubmit(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
    }

    std::vector<const ovrLayerHeader*> modifiedLayers(layerPtrList, layerPtrList + layerCount);
    ovrLayerEyeFovDepth depthLayerCopy{};
    ovrOldLayerEyeFovDepth oldDepthLayerCopy{};

    for (unsigned int i = 0; i < layerCount; ++i) {
        const auto* header = layerPtrList[i];
        if (!header) continue;

        if (header->Type == ovrLayerType_EyeFov || header->Type == ovrLayerType_EyeFovDepth) {
            if (g_oculusVersion >= 25) {
                const auto* eyeLayer = reinterpret_cast<const ovrLayerEyeFov*>(header);
                depthLayerCopy.Header = eyeLayer->Header;
                depthLayerCopy.RenderPose[0] = eyeLayer->RenderPose[0];
                depthLayerCopy.RenderPose[1] = eyeLayer->RenderPose[1];
                depthLayerCopy.SensorSampleTime = eyeLayer->SensorSampleTime;
                depthLayerCopy.Fov[0] = eyeLayer->Fov[0];
                depthLayerCopy.Fov[1] = eyeLayer->Fov[1];

                ProcessAndUpscaleLayer(
                    session,
                    eyeLayer->ColorTexture[0],
                    eyeLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(eyeLayer->Viewport),
                    eyeLayer->Fov,
                    depthLayerCopy.ColorTexture,
                    depthLayerCopy.Viewport
                );

                modifiedLayers[i] = &depthLayerCopy.Header;
            } else {
                const auto* eyeLayer = reinterpret_cast<const ovrOldLayerEyeFov*>(header);
                oldDepthLayerCopy.Header = eyeLayer->Header;
                oldDepthLayerCopy.RenderPose[0] = eyeLayer->RenderPose[0];
                oldDepthLayerCopy.RenderPose[1] = eyeLayer->RenderPose[1];
                oldDepthLayerCopy.SensorSampleTime = eyeLayer->SensorSampleTime;
                oldDepthLayerCopy.Fov[0] = eyeLayer->Fov[0];
                oldDepthLayerCopy.Fov[1] = eyeLayer->Fov[1];

                ProcessAndUpscaleLayer(
                    session,
                    eyeLayer->ColorTexture[0],
                    eyeLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(eyeLayer->Viewport),
                    eyeLayer->Fov,
                    oldDepthLayerCopy.ColorTexture,
                    oldDepthLayerCopy.Viewport
                );

                modifiedLayers[i] = reinterpret_cast<const ovrLayerHeader*>(&oldDepthLayerCopy.Header);
            }
            break;
        }
    }

    return g_origSubmit(session, frameIndex, viewScaleDesc, modifiedLayers.data(), layerCount);
}

} // namespace

void InstallOculusHooks() {
    InitLogging();
    g_cfg = LoadConfig();
    g_limiter.Initialize();

    HMODULE mod = nullptr;
    const wchar_t* dllNames[] = {
        L"LibOVRRT64_1.dll",
        L"VirtualDesktop.LibOVRRT64_1.dll",
        L"LibPVRRT64_1_X.dll"
    };

    for (const auto* name : dllNames) {
        mod = GetModuleHandleW(name);
        if (mod) break;
    }

    if (!mod) return;

    g_ovrCreateTextureSwapChainDX = reinterpret_cast<ovrCreateTextureSwapChainDXFn>(GetProcAddress(mod, "ovr_CreateTextureSwapChainDX"));
    g_ovrGetTextureSwapChainLength = reinterpret_cast<ovrGetTextureSwapChainLengthFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainLength"));
    g_ovrGetTextureSwapChainBufferDX = reinterpret_cast<ovrGetTextureSwapChainBufferDXFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainBufferDX"));
    g_ovrGetTextureSwapChainDesc = reinterpret_cast<ovrGetTextureSwapChainDescFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainDesc"));
    g_ovrGetTextureSwapChainCurrentIndex = reinterpret_cast<ovrGetTextureSwapChainCurrentIndexFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainCurrentIndex"));
    g_ovrCommitTextureSwapChain = reinterpret_cast<ovrCommitTextureSwapChainFn>(GetProcAddress(mod, "ovr_CommitTextureSwapChain"));
    g_ovrDestroyTextureSwapChain = reinterpret_cast<ovrDestroyTextureSwapChainFn>(GetProcAddress(mod, "ovr_DestroyTextureSwapChain"));

    auto initFn = reinterpret_cast<ovrInitializeFn>(GetProcAddress(mod, "ovr_Initialize"));
    auto submit = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_SubmitFrame"));
    auto submit2 = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_SubmitFrame2"));
    auto waitBegin = reinterpret_cast<ovrWaitToBeginFrameFn>(GetProcAddress(mod, "ovr_WaitToBeginFrame"));
    auto getSize = reinterpret_cast<ovrGetFovTextureSizeFn>(GetProcAddress(mod, "ovr_GetFovTextureSize"));

    MH_Initialize();

    if (initFn) {
        MH_CreateHook(reinterpret_cast<void*>(initFn), reinterpret_cast<void*>(&Hooked_ovrInitialize), reinterpret_cast<void**>(&g_origInitialize));
        MH_EnableHook(reinterpret_cast<void*>(initFn));
    }

    if (submit) {
        MH_CreateHook(reinterpret_cast<void*>(submit), reinterpret_cast<void*>(&Hooked_ovrSubmitFrame), reinterpret_cast<void**>(&g_origSubmit));
        MH_EnableHook(reinterpret_cast<void*>(submit));
    }

    if (submit2) {
        MH_CreateHook(reinterpret_cast<void*>(submit2), reinterpret_cast<void*>(&Hooked_ovrSubmitFrame), reinterpret_cast<void**>(&g_origSubmit2));
        MH_EnableHook(reinterpret_cast<void*>(submit2));
    }

    if (waitBegin) {
        MH_CreateHook(reinterpret_cast<void*>(waitBegin), reinterpret_cast<void*>(&Hooked_ovrWaitToBeginFrame), reinterpret_cast<void**>(&g_origWaitToBeginFrame));
        MH_EnableHook(reinterpret_cast<void*>(waitBegin));
    }

    if (getSize) {
        MH_CreateHook(reinterpret_cast<void*>(getSize), reinterpret_cast<void*>(&Hooked_ovrGetFovTextureSize), reinterpret_cast<void**>(&g_origGetFovTextureSize));
        MH_EnableHook(reinterpret_cast<void*>(getSize));
    }

    LogMessage(LogLevel::Info, "Hooks Oculus LibOVR instalados con soporte completo D3D11 NIS.");
}

} // namespace vrpacing

#endif