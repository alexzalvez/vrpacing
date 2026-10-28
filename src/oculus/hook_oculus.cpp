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
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "openvr/hook_openvr.h"
#include "third_party/oculus/ovr_minimal.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {
namespace {

using ovrInitializeFn = ovrResult(__cdecl*)(const ovrInitParams*);
using ovrSubmitFrameFn = ovrResult(__cdecl*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int);
using ovrWaitToBeginFrameFn = ovrResult(__cdecl*)(ovrSession, long long);
using ovrGetFovTextureSizeFn = ovrSizei(__cdecl*)(ovrSession, ovrEyeType, ovrFovPort, float);
using ovrGetPredictedDisplayTimeFn = double(__cdecl*)(ovrSession, long long);

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
ovrSubmitFrameFn g_origEndFrame = nullptr;
ovrWaitToBeginFrameFn g_origWaitToBeginFrame = nullptr;
ovrGetFovTextureSizeFn g_origGetFovTextureSize = nullptr;
ovrGetPredictedDisplayTimeFn g_ovrGetPredictedDisplayTime = nullptr;

ovrCreateTextureSwapChainDXFn g_ovrCreateTextureSwapChainDX = nullptr;
ovrGetTextureSwapChainLengthFn g_ovrGetTextureSwapChainLength = nullptr;
ovrGetTextureSwapChainBufferDXFn g_ovrGetTextureSwapChainBufferDX = nullptr;
ovrGetTextureSwapChainDescFn g_ovrGetTextureSwapChainDesc = nullptr;
ovrGetTextureSwapChainCurrentIndexFn g_ovrGetTextureSwapChainCurrentIndex = nullptr;
ovrCommitTextureSwapChainFn g_ovrCommitTextureSwapChain = nullptr;
ovrDestroyTextureSwapChainFn = void(__cdecl*)(ovrSession, ovrTextureSwapChain);
ovrDestroyTextureSwapChainFn g_ovrDestroyTextureSwapChain = nullptr;

uint32_t g_oculusVersion = 0;
std::atomic<bool> g_oculusNonD3D11Passthrough{ false };
std::atomic<bool> g_waitToBeginCalled{ false };
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;

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
    int lastInputWidth[ovrEye_Count] = { 0, 0 };
    int lastInputHeight[ovrEye_Count] = { 0, 0 };
    OculusOutputSwapchain outputChains[ovrEye_Count];
};

std::unique_ptr<OculusSessionContext> g_oculusCtx;

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

bool IsOpenXrLayerSessionActive() {
    HMODULE hLayer = GetModuleHandleW(L"VRPacingLayer.dll");
    if (hLayer) {
        using IsSessionFn = bool(__cdecl*)();
        auto fn = reinterpret_cast<IsSessionFn>(GetProcAddress(hLayer, "VRPacingLayer_IsActiveSession"));
        if (fn && fn()) {
            return true;
        }
    }
    return IsOpenXrLayerHandlingD3D11();
}

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "Oculus LibOVR Hook inicializado.");
    });
}

bool IsLegacyOculusLayer(const ovrLayerHeader* header) {
    if (!header) return false;
    if (g_oculusVersion > 0) {
        return g_oculusVersion < 25;
    }

    const auto* oldL = reinterpret_cast<const ovrOldLayerEyeFov*>(header);
    const auto* newL = reinterpret_cast<const ovrLayerEyeFov*>(header);

    if (oldL->ColorTexture[0] != nullptr &&
        oldL->Viewport[0].Size.w >= 256 && oldL->Viewport[0].Size.w <= 8192 &&
        oldL->Viewport[0].Size.h >= 256 && oldL->Viewport[0].Size.h <= 8192) {
        if (!newL->ColorTexture[0] ||
            newL->Viewport[0].Size.w < 256 || newL->Viewport[0].Size.w > 8192 ||
            newL->Viewport[0].Size.h < 256 || newL->Viewport[0].Size.h > 8192) {
            g_oculusVersion = 17;
            LogMessage(LogLevel::Info, "Oculus Hook: Detectado layout legacy (< v25, ej. Lone Echo). Fijado g_oculusVersion = 17.");
            return true;
        }
    }
    return false;
}

void CleanupOutputChains(OculusSessionContext& ctx) {
    if (ctx.outputChains[1].chain == ctx.outputChains[0].chain) {
        ctx.outputChains[1].chain = nullptr;
    }

    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        if (ctx.outputChains[eye].chain && g_ovrDestroyTextureSwapChain) {
            g_ovrDestroyTextureSwapChain(ctx.session, ctx.outputChains[eye].chain);
            ctx.outputChains[eye].chain = nullptr;
        }
        ctx.outputChains[eye].textures.clear();
        ctx.outputChains[eye].length = 0;
        ctx.outputChains[eye].width = 0;
        ctx.outputChains[eye].height = 0;
        ctx.lastInputChains[eye] = nullptr;
        ctx.lastInputWidth[eye] = 0;
        ctx.lastInputHeight[eye] = 0;
    }
    g_upscaler.ClearCache();
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

    ovrTextureSwapChain inChains[ovrEye_Count] = { leftInput, rightInput };

    ovrTextureSwapChainDesc leftDesc{};
    bool dimChanged = false;
    if (leftInput && g_ovrGetTextureSwapChainDesc && g_ovrGetTextureSwapChainDesc(session, leftInput, &leftDesc) >= 0) {
        if (ctx.lastInputWidth[0] > 0 && (leftDesc.Width != ctx.lastInputWidth[0] || leftDesc.Height != ctx.lastInputHeight[0])) {
            dimChanged = true;
        }
    }

    const bool chainsChanged = (leftInput != ctx.lastInputChains[0]) || (rightInput != ctx.lastInputChains[1]) || dimChanged;
    if (!chainsChanged && ctx.outputChains[0].chain != nullptr) {
        return true;
    }

    CleanupOutputChains(ctx);

    ctx.lastInputChains[0] = leftInput;
    ctx.lastInputChains[1] = rightInput;

    const bool singleTexture = (rightInput == nullptr || rightInput == leftInput);

    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        if (eye == 1 && singleTexture) {
            ctx.outputChains[1] = ctx.outputChains[0];
            ctx.lastInputWidth[1] = ctx.lastInputWidth[0];
            ctx.lastInputHeight[1] = ctx.lastInputHeight[0];
            break;
        }

        ovrTextureSwapChain curIn = inChains[eye];
        if (!curIn) continue;

        ovrTextureSwapChainDesc desc{};
        ovrResult descRes = g_ovrGetTextureSwapChainDesc(session, curIn, &desc);
        if (descRes < 0) {
            g_upscaleStatus = -1;
            g_lastHResult = descRes;
            LogMessage(LogLevel::Error, "Oculus Hook: ovr_GetTextureSwapChainDesc fallo. Res=" + std::to_string(descRes));
            return false;
        }

        ctx.lastInputWidth[eye] = desc.Width;
        ctx.lastInputHeight[eye] = desc.Height;
        g_lastFormat = static_cast<uint32_t>(desc.Format);
        g_lastInW = desc.Width;
        g_lastInH = desc.Height;

        if (!ctx.device) {
            ComPtr<IUnknown> unk0;
            if (g_ovrGetTextureSwapChainBufferDX(session, curIn, 0, IID_PPV_ARGS(unk0.GetAddressOf())) >= 0 && unk0) {
                ComPtr<ID3D11Texture2D> inTex0;
                if (SUCCEEDED(unk0->QueryInterface(IID_PPV_ARGS(inTex0.GetAddressOf()))) && inTex0) {
                    inTex0->GetDevice(ctx.device.ReleaseAndGetAddressOf());
                    if (ctx.device) {
                        ctx.device->GetImmediateContext(ctx.context.ReleaseAndGetAddressOf());
                        if (!g_upscaler.IsInitialized()) {
                            if (!g_upscaler.Initialize(ctx.device.Get())) {
                                g_upscaleStatus = -2;
                                LogMessage(LogLevel::Error, "Oculus Hook: Error al inicializar NisUpscalerD3D11.");
                            }
                        }
                    }
                } else {
                    g_oculusNonD3D11Passthrough.store(true, std::memory_order_relaxed);
                    LogMessage(LogLevel::Warn, "Oculus Hook: El Swapchain recibido no es D3D11 (juego D3D12/Vulkan detectado). Escalado en passthrough para evitar crash; Frame Pacing activo.");
                    return false;
                }
            }
        }

        if (!ctx.device) {
            g_upscaleStatus = -2;
            LogMessage(LogLevel::Error, "Oculus Hook: No se pudo capturar ID3D11Device desde el Swapchain.");
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

        g_lastOutW = outW;
        g_lastOutH = outH;

        ovrTextureSwapChainDesc outDesc = desc;
        outDesc.Width = outW;
        outDesc.Height = outH;
        outDesc.SampleCount = 1;
        outDesc.MipLevels = 1;
        outDesc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;
        outDesc.MiscFlags = desc.MiscFlags | ovrTextureMisc_DX_Typeless;

        ovrTextureSwapChain outChain = nullptr;
        ovrResult createRes = g_ovrCreateTextureSwapChainDX(session, ctx.device.Get(), &outDesc, &outChain);
        if (createRes < 0 || !outChain) {
            outDesc.BindFlags = ovrTextureBind_DX_RenderTarget;
            outDesc.MiscFlags = desc.MiscFlags;
            createRes = g_ovrCreateTextureSwapChainDX(session, ctx.device.Get(), &outDesc, &outChain);
        }

        if (createRes < 0 || !outChain) {
            g_upscaleStatus = -1;
            g_lastHResult = createRes;
            LogMessage(LogLevel::Error, "Oculus Hook: ovr_CreateTextureSwapChainDX fallo. Res=" + std::to_string(createRes));
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
                   " (FMT=" + std::to_string(desc.Format) + ") -> " +
                   std::to_string(outW) + "x" + std::to_string(outH));
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
    if (inLeft && g_oculusCtx && g_ovrGetTextureSwapChainDesc) {
        ovrTextureSwapChainDesc curDesc{};
        if (g_ovrGetTextureSwapChainDesc(session, inLeft, &curDesc) >= 0) {
            if (g_oculusCtx->lastInputWidth[0] > 0 &&
                (curDesc.Width != g_oculusCtx->lastInputWidth[0] || curDesc.Height != g_oculusCtx->lastInputHeight[0])) {
                LogMessage(LogLevel::Info, "Oculus Hook: Cambio en resolucion de Swapchain (" +
                           std::to_string(g_oculusCtx->lastInputWidth[0]) + "x" + std::to_string(g_oculusCtx->lastInputHeight[0]) +
                           " -> " + std::to_string(curDesc.Width) + "x" + std::to_string(curDesc.Height) + "). Recreando salida...");
                CleanupOutputChains(*g_oculusCtx);
            }
        }
    }

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

    float leftProjX = 0.5f, leftProjY = 0.5f;
    float rightProjX = 0.5f, rightProjY = 0.5f;
    if (inFov) {
        const float denomX0 = inFov[0].RightTan + inFov[0].LeftTan;
        if (std::abs(denomX0) > 0.0001f) {
            leftProjX = 0.5f * (1.0f + (inFov[0].RightTan - inFov[0].LeftTan) / denomX0);
        }
        const float denomY0 = inFov[0].DownTan + inFov[0].UpTan;
        if (std::abs(denomY0) > 0.0001f) {
            leftProjY = 0.5f * (1.0f + (inFov[0].DownTan - inFov[0].UpTan) / denomY0);
        }

        const float denomX1 = inFov[1].RightTan + inFov[1].LeftTan;
        if (std::abs(denomX1) > 0.0001f) {
            rightProjX = 0.5f * (1.0f + (inFov[1].RightTan - inFov[1].LeftTan) / denomX1);
        }
        const float denomY1 = inFov[1].DownTan + inFov[1].UpTan;
        if (std::abs(denomY1) > 0.0001f) {
            rightProjY = 0.5f * (1.0f + (inFov[1].DownTan - inFov[1].UpTan) / denomY1);
        }
    }

    ovrTextureSwapChainDesc inDesc0{};
    g_ovrGetTextureSwapChainDesc(session, inLeft, &inDesc0);

    VrsTextureMode vrsMode = VrsTextureMode::SingleEye;
    if (inDesc0.ArraySize >= 2) {
        vrsMode = VrsTextureMode::TextureArray;
    } else if (singleTexture) {
        vrsMode = VrsTextureMode::CombinedSideBySide;
    }

    const uint32_t eyeW = static_cast<uint32_t>(inViewport[0].Size.w);
    const uint32_t eyeH = static_cast<uint32_t>(inViewport[0].Size.h);

    if (ctx.device && ctx.context) {
        VrsFoveatedD3D11::Instance().UpdateTarget(
            ctx.device.Get(),
            ctx.context.Get(),
            g_cfg.ffr.enabled,
            eyeW,
            eyeH,
            vrsMode,
            leftProjX, leftProjY,
            rightProjX, rightProjY
        );
    }

    if (!g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        outColor[0] = inLeft;
        outColor[1] = inRight;
        outViewport[0] = inViewport[0];
        outViewport[1] = inViewport[1];
        return;
    }

    if (ctx.context) {
        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        ctx.context->OMSetRenderTargets(1, nullRTV, nullptr);
    }

    for (int eye = 0; eye < ovrEye_Count; ++eye) {
        ovrTextureSwapChain curIn = (eye == 1 && singleTexture) ? inLeft : (eye == 0 ? inLeft : inRight);
        if (!curIn) continue;

        int inLen = 0;
        g_ovrGetTextureSwapChainLength(session, curIn, &inLen);
        if (inLen <= 0) continue;

        int inIdx = 0;
        g_ovrGetTextureSwapChainCurrentIndex(session, curIn, &inIdx);
        inIdx = (inIdx - 1 + inLen) % inLen;

        ComPtr<IUnknown> unk;
        if (g_ovrGetTextureSwapChainBufferDX(session, curIn, inIdx, IID_PPV_ARGS(unk.GetAddressOf())) < 0 || !unk) {
            continue;
        }

        ComPtr<ID3D11Texture2D> inTex;
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(inTex.GetAddressOf()))) || !inTex) {
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

        float projX = (eye == 0) ? leftProjX : rightProjX;
        float projY = (eye == 0) ? leftProjY : rightProjY;

        UpscaleDispatchDesc desc{};
        desc.srcRect = srcR;
        desc.dstRect = dstR;
        desc.srcArrayIndex = (inDesc.ArraySize > 1) ? eye : 0;
        desc.dstArrayIndex = (inDesc.ArraySize > 1) ? eye : 0;
        desc.projCenterX = projX;
        desc.projCenterY = projY;
        desc.radius = g_cfg.upscaling.radius;
        desc.sharpness = g_cfg.upscaling.sharpness;
        desc.maskEnabled = false;
        desc.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

        if (g_upscaler.Upscale(ctx.context.Get(), inTex.Get(), outTex, desc)) {
            outColor[eye] = ctx.outputChains[eye].chain;
            outViewport[eye].Pos.x = static_cast<int>(dstR.x);
            outViewport[eye].Pos.y = static_cast<int>(dstR.y);
            outViewport[eye].Size.w = static_cast<int>(dstR.width);
            outViewport[eye].Size.h = static_cast<int>(dstR.height);
            anySuccess = true;
        } else {
            g_upscaleStatus = -3;
            outColor[eye] = curIn;
            outViewport[eye] = inViewport[eye];
        }
    }

    if (anySuccess) {
        g_upscaleStatus = 1;
        g_ovrCommitTextureSwapChain(session, ctx.outputChains[0].chain);
        if (!singleTexture && ctx.outputChains[1].chain && ctx.outputChains[1].chain != ctx.outputChains[0].chain) {
            g_ovrCommitTextureSwapChain(session, ctx.outputChains[1].chain);
        }
    }
}

ovrResult __cdecl Hooked_ovrInitialize(const ovrInitParams* params) {
    if (params) {
        g_oculusVersion = params->RequestedMinorVersion;
        LogMessage(LogLevel::Info, "Oculus Hook: ovr_Initialize interceptado con version minor " + std::to_string(g_oculusVersion));
    }
    return g_origInitialize ? g_origInitialize(params) : 0;
}

ovrResult __cdecl Hooked_ovrWaitToBeginFrame(ovrSession session, long long frameIndex) {
    if (!g_origWaitToBeginFrame) return 0;

    if (IsOpenXrLayerSessionActive() || g_openVrHandlingFrames.load(std::memory_order_relaxed)) {
        return g_origWaitToBeginFrame(session, frameIndex);
    }

    g_waitToBeginCalled.store(true, std::memory_order_relaxed);
    EnsureInit();
    g_shm.SyncConfig(g_cfg);
    VrsFoveatedD3D11::Instance().EndFrame();

    if (g_ovrGetPredictedDisplayTime) {
        const double t0 = g_ovrGetPredictedDisplayTime(session, frameIndex);
        const double t1 = g_ovrGetPredictedDisplayTime(session, frameIndex + 1);
        const double dt = t1 - t0;
        if (dt > 0.004 && dt < 0.025) {
            g_hzTracker.PushHz(std::round(1.0 / dt));
        }
    }

    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : g_hzTracker.GetEffectiveHz().value_or(90.0);
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled) {
        LimitInputs lim{};
        lim.hz = hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        g_limiter.WaitBeforeRuntime();
    }

    g_shm.UpdateTelemetry(L"LibOVR", hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);

    return g_origWaitToBeginFrame(session, frameIndex);
}

ovrSizei __cdecl Hooked_ovrGetFovTextureSize(ovrSession session, ovrEyeType eye, ovrFovPort fov, float pixelsPerDisplayPixel) {
    return g_origGetFovTextureSize ? g_origGetFovTextureSize(session, eye, fov, pixelsPerDisplayPixel) : ovrSizei{ 0, 0 };
}

ovrResult SubmitFrameInternal(ovrSubmitFrameFn origFn,
                              ovrSession session,
                              long long frameIndex,
                              const ovrViewScaleDesc* viewScaleDesc,
                              ovrLayerHeader const* const* layerPtrList,
                              unsigned int layerCount) {
    if (!origFn) return 0;

    if (g_oculusNonD3D11Passthrough.load(std::memory_order_relaxed)) {
        return origFn(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
    }

    if (IsOpenXrLayerHandlingD3D11() || g_openVrHandlingFrames.load(std::memory_order_relaxed)) {
        return origFn(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
    }

    thread_local bool inOculusSubmit = false;
    if (inOculusSubmit) {
        return origFn(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
    }
    inOculusSubmit = true;

    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    if (!g_waitToBeginCalled.load(std::memory_order_relaxed)) {
        VrsFoveatedD3D11::Instance().EndFrame();
    }

    const bool doUpscale = g_cfg.upscaling.enabled && (g_cfg.upscaling.scale > 1.0f);
    if (!layerPtrList || layerCount == 0 || (!doUpscale && !g_cfg.ffr.enabled)) {
        ovrResult res = origFn(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
        inOculusSubmit = false;
        return res;
    }

    static std::atomic<bool> s_submitLogged{ false };
    if (!s_submitLogged.exchange(true)) {
        LogMessage(LogLevel::Info, "Oculus [SubmitFrame] Primer frame: layerCount=" + std::to_string(layerCount) +
                   " Layer0 Type=" + std::to_string(layerPtrList[0] ? layerPtrList[0]->Type : -1) +
                   " Scale=" + std::to_string(g_cfg.upscaling.scale));
    }

    std::vector<const ovrLayerHeader*> modifiedLayers(layerPtrList, layerPtrList + layerCount);

    ovrLayerEyeFov eyeFovCopy{};
    ovrLayerEyeFovDepth depthLayerCopy{};

    ovrOldLayerEyeFov oldEyeFovCopy{};
    ovrOldLayerEyeFovDepth oldDepthCopy{};

    for (unsigned int i = 0; i < layerCount; ++i) {
        const auto* header = layerPtrList[i];
        if (!header) continue;

        const bool isLegacy = IsLegacyOculusLayer(header);

        if (header->Type == ovrLayerType_EyeFov) {
            if (isLegacy) {
                const auto* eyeLayer = reinterpret_cast<const ovrOldLayerEyeFov*>(header);
                oldEyeFovCopy = *eyeLayer;

                ProcessAndUpscaleLayer(
                    session,
                    eyeLayer->ColorTexture[0],
                    eyeLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(eyeLayer->Viewport),
                    eyeLayer->Fov,
                    oldEyeFovCopy.ColorTexture,
                    oldEyeFovCopy.Viewport
                );

                modifiedLayers[i] = reinterpret_cast<const ovrLayerHeader*>(&oldEyeFovCopy.Header);
            } else {
                const auto* eyeLayer = reinterpret_cast<const ovrLayerEyeFov*>(header);
                eyeFovCopy = *eyeLayer;

                ProcessAndUpscaleLayer(
                    session,
                    eyeLayer->ColorTexture[0],
                    eyeLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(eyeLayer->Viewport),
                    eyeLayer->Fov,
                    eyeFovCopy.ColorTexture,
                    eyeFovCopy.Viewport
                );

                modifiedLayers[i] = &eyeFovCopy.Header;
            }
            break;
        } else if (header->Type == ovrLayerType_EyeFovDepth) {
            if (isLegacy) {
                const auto* depthLayer = reinterpret_cast<const ovrOldLayerEyeFovDepth*>(header);
                oldDepthCopy = *depthLayer;

                ProcessAndUpscaleLayer(
                    session,
                    depthLayer->ColorTexture[0],
                    depthLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(depthLayer->Viewport),
                    depthLayer->Fov,
                    oldDepthCopy.ColorTexture,
                    oldDepthCopy.Viewport
                );

                modifiedLayers[i] = reinterpret_cast<const ovrLayerHeader*>(&oldDepthCopy.Header);
            } else {
                const auto* depthLayer = reinterpret_cast<const ovrLayerEyeFovDepth*>(header);
                depthLayerCopy = *depthLayer;

                ProcessAndUpscaleLayer(
                    session,
                    depthLayer->ColorTexture[0],
                    depthLayer->ColorTexture[1],
                    const_cast<ovrRecti*>(depthLayer->Viewport),
                    depthLayer->Fov,
                    depthLayerCopy.ColorTexture,
                    depthLayerCopy.Viewport
                );

                modifiedLayers[i] = &depthLayerCopy.Header;
            }
            break;
        }
    }

    g_shm.UpdateResolutionAndStatus(g_lastInW, g_lastInH, g_lastOutW, g_lastOutH, g_lastFormat, g_upscaleStatus, g_lastHResult);

    if (!IsOpenXrLayerSessionActive() && !g_waitToBeginCalled.load(std::memory_order_relaxed)) {
        const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : g_hzTracker.GetEffectiveHz().value_or(90.0);
        double targetFps = 0.0;
        if (g_cfg.pacing.enabled) {
            LimitInputs lim{};
            lim.hz = hz;
            lim.offsetFps = g_cfg.pacing.offsetFps;
            lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
            lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
            targetFps = ComputeTargetFps(lim);
            g_limiter.SetTargetFps(targetFps);
            g_limiter.WaitBeforeRuntime();
        }
        g_shm.UpdateTelemetry(L"LibOVR (VDXR)", hz, targetFps,
                              g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                              g_lastFormat, g_upscaleStatus, g_lastHResult);
    }

    ovrResult res = origFn(session, frameIndex, viewScaleDesc, modifiedLayers.data(), layerCount);
    inOculusSubmit = false;
    return res;
}

ovrResult __cdecl Hooked_ovrSubmitFrame(ovrSession session,
                                       long long frameIndex,
                                       const ovrViewScaleDesc* viewScaleDesc,
                                       ovrLayerHeader const* const* layerPtrList,
                                       unsigned int layerCount) {
    return SubmitFrameInternal(g_origSubmit, session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
}

ovrResult __cdecl Hooked_ovrSubmitFrame2(ovrSession session,
                                        long long frameIndex,
                                        const ovrViewScaleDesc* viewScaleDesc,
                                        ovrLayerHeader const* const* layerPtrList,
                                        unsigned int layerCount) {
    return SubmitFrameInternal(g_origSubmit2 ? g_origSubmit2 : g_origSubmit, session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
}

ovrResult __cdecl Hooked_ovrEndFrame(ovrSession session,
                                     long long frameIndex,
                                     const ovrViewScaleDesc* viewScaleDesc,
                                     ovrLayerHeader const* const* layerPtrList,
                                     unsigned int layerCount) {
    return SubmitFrameInternal(g_origEndFrame ? g_origEndFrame : (g_origSubmit ? g_origSubmit : g_origSubmit2),
                               session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
}

} // namespace

void InstallOculusHooks() {
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

    if (!mod) {
        return;
    }

    static std::mutex s_installMutex;
    std::lock_guard<std::mutex> lock(s_installMutex);

    static std::atomic<bool> s_installed{ false };
    if (s_installed.exchange(true)) return;

    InitLogging();
    g_cfg = LoadConfig();
    g_limiter.Initialize();

    g_ovrCreateTextureSwapChainDX = reinterpret_cast<ovrCreateTextureSwapChainDXFn>(GetProcAddress(mod, "ovr_CreateTextureSwapChainDX"));
    g_ovrGetTextureSwapChainLength = reinterpret_cast<ovrGetTextureSwapChainLengthFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainLength"));
    g_ovrGetTextureSwapChainBufferDX = reinterpret_cast<ovrGetTextureSwapChainBufferDXFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainBufferDX"));
    g_ovrGetTextureSwapChainDesc = reinterpret_cast<ovrGetTextureSwapChainDescFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainDesc"));
    g_ovrGetTextureSwapChainCurrentIndex = reinterpret_cast<ovrGetTextureSwapChainCurrentIndexFn>(GetProcAddress(mod, "ovr_GetTextureSwapChainCurrentIndex"));
    g_ovrCommitTextureSwapChain = reinterpret_cast<ovrCommitTextureSwapChainFn>(GetProcAddress(mod, "ovr_CommitTextureSwapChain"));
    g_ovrDestroyTextureSwapChain = reinterpret_cast<ovrDestroyTextureSwapChainFn>(GetProcAddress(mod, "ovr_DestroyTextureSwapChain"));
    g_ovrGetPredictedDisplayTime = reinterpret_cast<ovrGetPredictedDisplayTimeFn>(GetProcAddress(mod, "ovr_GetPredictedDisplayTime"));

    auto initFn = reinterpret_cast<ovrInitializeFn>(GetProcAddress(mod, "ovr_Initialize"));
    auto submit = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_SubmitFrame"));
    auto submit2 = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_SubmitFrame2"));
    auto endFrame = reinterpret_cast<ovrSubmitFrameFn>(GetProcAddress(mod, "ovr_EndFrame"));
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

    if (submit2 && submit2 != submit) {
        MH_CreateHook(reinterpret_cast<void*>(submit2), reinterpret_cast<void*>(&Hooked_ovrSubmitFrame2), reinterpret_cast<void**>(&g_origSubmit2));
        MH_EnableHook(reinterpret_cast<void*>(submit2));
    }

    if (endFrame && endFrame != submit && endFrame != submit2) {
        MH_CreateHook(reinterpret_cast<void*>(endFrame), reinterpret_cast<void*>(&Hooked_ovrEndFrame), reinterpret_cast<void**>(&g_origEndFrame));
        MH_EnableHook(reinterpret_cast<void*>(endFrame));
    }

    if (waitBegin) {
        MH_CreateHook(reinterpret_cast<void*>(waitBegin), reinterpret_cast<void*>(&Hooked_ovrWaitToBeginFrame), reinterpret_cast<void**>(&g_origWaitToBeginFrame));
        MH_EnableHook(reinterpret_cast<void*>(waitBegin));
    }

    if (getSize) {
        MH_CreateHook(reinterpret_cast<void*>(getSize), reinterpret_cast<void*>(&Hooked_ovrGetFovTextureSize), reinterpret_cast<void**>(&g_origGetFovTextureSize));
        MH_EnableHook(reinterpret_cast<void*>(getSize));
    }

    LogMessage(LogLevel::Info, "Hooks Oculus LibOVR instalados con soporte completo D3D11 NIS (SubmitFrame, SubmitFrame2, EndFrame).");
}

} // namespace vrpacing

#endif