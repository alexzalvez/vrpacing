#ifdef _WIN32

#include "oculus/hook_oculus.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "minhook/include/MinHook.h"
#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

using ovrSession = void*;
using ovrTextureSwapChain = void*;
using ovrResult = int32_t;

enum ovrLayerType {
    ovrLayerType_Disabled = 0,
    ovrLayerType_EyeFov = 1,
    ovrLayerType_EyeFovDepth = 2,
    ovrLayerType_Quad = 3,
};

struct ovrLayerHeader {
    ovrLayerType Type;
    unsigned Flags;
};

struct ovrViewScaleDesc {
    float HmdToEyePose[2][7];
    float HmdToEyeViewOffset[2][3];
};

struct ovrLayerEyeFov {
    ovrLayerHeader Header;
    ovrTextureSwapChain ColorTexture[2];
    int Viewport[2][4];
    float Fov[2][4];
    float RenderPose[2][7];
    double SensorSampleTime;
};

enum ovrTextureFormat {
    OVR_FORMAT_UNKNOWN = 0,
    OVR_FORMAT_B5G6R5_UNORM = 1,
    OVR_FORMAT_B5G5R5A1_UNORM = 2,
    OVR_FORMAT_B4G4R4A4_UNORM = 3,
    OVR_FORMAT_R8G8B8A8_UNORM = 4,
    OVR_FORMAT_R8G8B8A8_UNORM_SRGB = 5,
    OVR_FORMAT_B8G8R8A8_UNORM = 6,
    OVR_FORMAT_B8G8R8A8_UNORM_SRGB = 7,
    OVR_FORMAT_B8G8R8X8_UNORM = 8,
    OVR_FORMAT_B8G8R8X8_UNORM_SRGB = 9,
    OVR_FORMAT_R16G16B16A16_FLOAT = 10,
    OVR_FORMAT_D16_UNORM = 11,
    OVR_FORMAT_D24_UNORM_S8_UINT = 12,
    OVR_FORMAT_D32_FLOAT = 13,
    OVR_FORMAT_D32_FLOAT_S8X24_UINT = 14,
    OVR_FORMAT_BC1_UNORM = 15,
    OVR_FORMAT_BC1_UNORM_SRGB = 16,
    OVR_FORMAT_BC2_UNORM = 17,
    OVR_FORMAT_BC2_UNORM_SRGB = 18,
    OVR_FORMAT_BC3_UNORM = 19,
    OVR_FORMAT_BC3_UNORM_SRGB = 20,
    OVR_FORMAT_BC6H_UF16 = 21,
    OVR_FORMAT_BC6H_SF16 = 22,
    OVR_FORMAT_BC7_UNORM = 23,
    OVR_FORMAT_BC7_UNORM_SRGB = 24,
};

enum ovrTextureMiscFlags {
    ovrTextureMisc_None = 0,
    ovrTextureMisc_DX_Typeless = 0x0001,
    ovrTextureMisc_AllowGenerateMips = 0x0002,
    ovrTextureMisc_ProtectedContent = 0x0004,
    ovrTextureMisc_AutoGenerateMips = 0x0008,
};

enum ovrTextureBindFlags {
    ovrTextureBind_None = 0,
    ovrTextureBind_DX_RenderTarget = 0x0001,
    ovrTextureBind_DX_UnorderedAccess = 0x0002,
    ovrTextureBind_DX_DepthStencil = 0x0004,
};

struct ovrTextureSwapChainDesc {
    int Type;
    ovrTextureFormat Format;
    int ArraySize;
    int Width;
    int Height;
    int MipLevels;
    int SampleCount;
    unsigned char StaticImage;
    unsigned int MiscFlags;
    unsigned int BindFlags;
};

struct OculusOutputSwapchain {
    ovrTextureSwapChain chain = nullptr;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    uint32_t width = 0;
    uint32_t height = 0;
    int length = 0;
};

struct OculusContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OculusOutputSwapchain outputChains[2];
};

std::mutex g_mutex;
RuntimeConfig g_cfg;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;
OculusContext g_oculusCtx;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;

using PFN_ovr_SubmitFrame = ovrResult(*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int);
using PFN_ovr_SubmitFrame2 = ovrResult(*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int, unsigned int);
using PFN_ovr_EndFrame = ovrResult(*)(ovrSession, long long, const ovrViewScaleDesc*, ovrLayerHeader const* const*, unsigned int);
using PFN_ovr_DestroyTextureSwapChain = void(*)(ovrSession, ovrTextureSwapChain);

PFN_ovr_SubmitFrame g_origSubmitFrame = nullptr;
PFN_ovr_SubmitFrame2 g_origSubmitFrame2 = nullptr;
PFN_ovr_EndFrame g_origEndFrame = nullptr;
PFN_ovr_DestroyTextureSwapChain g_origDestroyTextureSwapChain = nullptr;

bool IsOpenXrLayerSessionActive() {
    HMODULE hLayer = GetModuleHandleW(L"VRPacingLayer.dll");
    if (hLayer) {
        using IsActiveFn = bool(__cdecl*)();
        auto fn = reinterpret_cast<IsActiveFn>(GetProcAddress(hLayer, "VRPacingLayer_IsActiveSession"));
        if (fn && fn()) {
            return true;
        }
    }
    return g_shm.IsOpenXrD3D11ActiveForCurrentProcess();
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
        LogMessage(LogLevel::Info, "Oculus LibOVR Hook initialized.");
    });
}

double QueryOculusDisplayHz(ovrSession session) {
    if (!session) return 90.0;
    HMODULE mod = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"LibPVRRT64_1_X.dll");
    if (!mod) return 90.0;

    using PFN_ovr_GetFloat = float(*)(ovrSession, const char*, float);
    auto pfnGetFloat = reinterpret_cast<PFN_ovr_GetFloat>(GetProcAddress(mod, "ovr_getFloat"));
    if (pfnGetFloat) {
        float hz = pfnGetFloat(session, "DisplayRefreshRate", 0.0f);
        if (hz >= 45.0f && hz <= 200.0f) {
            return static_cast<double>(hz);
        }
    }
    return 90.0;
}

void ApplyOculusFramePacing(ovrSession session) {
    if (IsOpenXrLayerSessionActive()) return;

    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    const double hwHz = QueryOculusDisplayHz(session);
    g_hzTracker.Update(hwHz);
    const double effectiveHz = g_hzTracker.GetEffectiveHz();
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;

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

    g_shm.UpdateTelemetry(L"Oculus LibOVR", hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);
}

void ProcessOculusLayer(ovrSession session, const ovrLayerHeader* layerHeader) {
    if (!layerHeader) return;

    ovrLayerEyeFov* eyeFovLayer = nullptr;
    if (layerHeader->Type == ovrLayerType_EyeFov || static_cast<int>(layerHeader->Type) == 1) {
        eyeFovLayer = reinterpret_cast<ovrLayerEyeFov*>(const_cast<ovrLayerHeader*>(layerHeader));
    }
    if (!eyeFovLayer) return;

    ovrTextureSwapChain inChainL = eyeFovLayer->ColorTexture[0];
    ovrTextureSwapChain inChainR = eyeFovLayer->ColorTexture[1];
    if (!inChainL) return;

    const bool isSideBySide = (inChainL == inChainR);
    HMODULE mod = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"LibPVRRT64_1_X.dll");
    if (!mod) return;

    using PFN_ovr_GetTextureSwapChainCurrentIndex = ovrResult(*)(ovrSession, ovrTextureSwapChain, int*);
    using PFN_ovr_GetTextureSwapChainBufferDX = ovrResult(*)(ovrSession, ovrTextureSwapChain, int, const IID, void**);
    using PFN_ovr_CommitTextureSwapChain = ovrResult(*)(ovrSession, ovrTextureSwapChain);
    using PFN_ovr_CreateTextureSwapChainDX = ovrResult(*)(ovrSession, IUnknown*, const ovrTextureSwapChainDesc*, ovrTextureSwapChain*);
    using PFN_ovr_GetTextureSwapChainLength = ovrResult(*)(ovrSession, ovrTextureSwapChain, int*);

    auto pfnGetCurrentIndex = reinterpret_cast<PFN_ovr_GetTextureSwapChainCurrentIndex>(GetProcAddress(mod, "ovr_getTextureSwapChainCurrentIndex"));
    auto pfnGetBufferDX = reinterpret_cast<PFN_ovr_GetTextureSwapChainBufferDX>(GetProcAddress(mod, "ovr_getTextureSwapChainBufferDX"));
    auto pfnCommit = reinterpret_cast<PFN_ovr_CommitTextureSwapChain>(GetProcAddress(mod, "ovr_commitTextureSwapChain"));
    auto pfnCreateSwapChainDX = reinterpret_cast<PFN_ovr_CreateTextureSwapChainDX>(GetProcAddress(mod, "ovr_createTextureSwapChainDX"));
    auto pfnGetLength = reinterpret_cast<PFN_ovr_GetTextureSwapChainLength>(GetProcAddress(mod, "ovr_getTextureSwapChainLength"));

    if (!pfnGetCurrentIndex || !pfnGetBufferDX || !pfnCommit || !pfnCreateSwapChainDX || !pfnGetLength) {
        return;
    }

    int curIdxL = 0;
    if (pfnGetCurrentIndex(session, inChainL, &curIdxL) < 0) return;

    ComPtr<ID3D11Texture2D> inTexL;
    if (pfnGetBufferDX(session, inChainL, curIdxL, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(inTexL.GetAddressOf())) < 0) {
        return;
    }

    D3D11_TEXTURE2D_DESC inDesc{};
    inTexL->GetDesc(&inDesc);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_oculusCtx.device) {
            inTexL->GetDevice(g_oculusCtx.device.GetAddressOf());
            if (g_oculusCtx.device) {
                g_oculusCtx.device->GetImmediateContext(g_oculusCtx.context.GetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    if (!g_upscaler.Initialize(g_oculusCtx.device.Get())) {
                        g_upscaleStatus = -2;
                    }
                }
            }
        }
    }

    if (!g_oculusCtx.device || !g_oculusCtx.context) return;

    const uint32_t fullEyeW = isSideBySide ? (inDesc.Width / 2) : inDesc.Width;
    const uint32_t fullEyeH = inDesc.Height;
    if (fullEyeW < 1000 || fullEyeH < 1000) return;

    g_lastFormat = static_cast<uint32_t>(inDesc.Format);
    g_lastInW = fullEyeW;
    g_lastInH = fullEyeH;

    // [A4] & [F1] Call UpdateTarget using centered pattern; obey enabled flag strictly
    {
        const bool vrsActive = g_cfg.ffr.enabled && !IsOpenXrLayerSessionActive();
        VrsFoveatedD3D11::Instance().UpdateTarget(
            g_oculusCtx.device.Get(),
            g_oculusCtx.context.Get(),
            vrsActive,
            fullEyeW,
            fullEyeH,
            g_cfg.ffr.debugMode);
    }

    if (!g_cfg.upscaling.enabled || g_cfg.upscaling.scale <= 1.0f) return;

    const double s = g_cfg.upscaling.scale;
    uint32_t maxOutW = static_cast<uint32_t>((std::max)(1.0, std::floor(fullEyeW * s)));
    uint32_t maxOutH = static_cast<uint32_t>((std::max)(1.0, std::floor(fullEyeH * s)));

    const uint64_t maxPx = static_cast<uint64_t>(maxOutW) * static_cast<uint64_t>(maxOutH);
    if (maxPx > g_cfg.upscaling.maxOutputPixels) {
        const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(maxPx));
        maxOutW = static_cast<uint32_t>((std::max)(1.0, std::floor(maxOutW * factor)));
        maxOutH = static_cast<uint32_t>((std::max)(1.0, std::floor(maxOutH * factor)));
    }

    g_lastOutW = maxOutW;
    g_lastOutH = maxOutH;

    for (int eye = 0; eye < (isSideBySide ? 1 : 2); ++eye) {
        auto& chain = g_oculusCtx.outputChains[eye];
        const uint32_t reqW = isSideBySide ? (maxOutW * 2) : maxOutW;
        const uint32_t reqH = maxOutH;

        if (!chain.chain || chain.width != reqW || chain.height != reqH) {
            if (chain.chain && g_origDestroyTextureSwapChain) {
                g_origDestroyTextureSwapChain(session, chain.chain);
                chain.chain = nullptr;
            }
            chain.textures.clear();

            ovrTextureSwapChainDesc desc{};
            desc.Type = 1; // 2D Texture
            desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc.ArraySize = 1;
            desc.Width = reqW;
            desc.Height = reqH;
            desc.MipLevels = 1;
            desc.SampleCount = 1;
            desc.StaticImage = 0;
            desc.MiscFlags = ovrTextureMisc_DX_Typeless | ovrTextureMisc_AutoGenerateMips;
            desc.BindFlags = ovrTextureBind_DX_RenderTarget | ovrTextureBind_DX_UnorderedAccess;

            if (pfnCreateSwapChainDX(session, g_oculusCtx.device.Get(), &desc, &chain.chain) < 0) {
                g_upscaleStatus = -1;
                return;
            }

            pfnGetLength(session, chain.chain, &chain.length);
            chain.textures.resize(chain.length);
            for (int i = 0; i < chain.length; ++i) {
                pfnGetBufferDX(session, chain.chain, i, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(chain.textures[i].GetAddressOf()));
            }
            chain.width = reqW;
            chain.height = reqH;
            LogMessage(LogLevel::Info, "Oculus Output Swapchain [eye " + std::to_string(eye) + "] created: " +
                       std::to_string(reqW) + "x" + std::to_string(reqH) + " -> " +
                       std::to_string(reqW) + "x" + std::to_string(reqH));
        }

        int outIdx = 0;
        pfnGetCurrentIndex(session, chain.chain, &outIdx);
        ID3D11Texture2D* outTex = chain.textures[outIdx].Get();

        ID3D11RenderTargetView* nullRTV[1] = { nullptr };
        g_oculusCtx.context->OMSetRenderTargets(1, nullRTV, nullptr);

        if (isSideBySide) {
            UpscaleDispatchDesc dL{};
            dL.srcRect = { 0, 0, fullEyeW, fullEyeH };
            dL.dstRect = { 0, 0, maxOutW, maxOutH };
            dL.projCenterX = 0.5f;
            dL.projCenterY = 0.5f;
            dL.radius = g_cfg.upscaling.radius;
            dL.sharpness = g_cfg.upscaling.sharpness;
            g_upscaler.Upscale(g_oculusCtx.context.Get(), inTexL.Get(), outTex, dL);

            UpscaleDispatchDesc dR{};
            dR.srcRect = { fullEyeW, 0, fullEyeW, fullEyeH };
            dR.dstRect = { maxOutW, 0, maxOutW, maxOutH };
            dR.projCenterX = 0.5f;
            dR.projCenterY = 0.5f;
            dR.radius = g_cfg.upscaling.radius;
            dR.sharpness = g_cfg.upscaling.sharpness;
            g_upscaler.Upscale(g_oculusCtx.context.Get(), inTexL.Get(), outTex, dR);
        } else {
            ovrTextureSwapChain inChain = eyeFovLayer->ColorTexture[eye];
            int curIdx = 0;
            pfnGetCurrentIndex(session, inChain, &curIdx);
            ComPtr<ID3D11Texture2D> inTex;
            pfnGetBufferDX(session, inChain, curIdx, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(inTex.GetAddressOf()));

            UpscaleDispatchDesc d{};
            d.srcRect = { 0, 0, fullEyeW, fullEyeH };
            d.dstRect = { 0, 0, maxOutW, maxOutH };
            d.projCenterX = 0.5f;
            d.projCenterY = 0.5f;
            d.radius = g_cfg.upscaling.radius;
            d.sharpness = g_cfg.upscaling.sharpness;
            g_upscaler.Upscale(g_oculusCtx.context.Get(), inTex.Get(), outTex, d);
        }

        pfnCommit(session, chain.chain);
        eyeFovLayer->ColorTexture[eye] = chain.chain;
    }

    g_upscaleStatus = 1;
}

ovrResult Hooked_ovr_SubmitFrame(ovrSession session, long long frameIndex, const ovrViewScaleDesc* viewScaleDesc,
                                 ovrLayerHeader const* const* layerHeaders, unsigned int layerCount) {
    ApplyOculusFramePacing(session);
    VrsFoveatedD3D11::Instance().EndFrame();

    static std::atomic<bool> s_firstSubmitLogged{ false };
    if (!s_firstSubmitLogged.exchange(true)) {
        LogMessage(LogLevel::Info, "Oculus [SubmitFrame] First frame: layerCount=" + std::to_string(layerCount) +
                   " Layer0 Type=" + std::to_string(layerHeaders && layerCount > 0 && layerHeaders[0] ? layerHeaders[0]->Type : -1) +
                   " Scale=" + std::to_string(g_cfg.upscaling.scale));
    }

    if (layerHeaders && layerCount > 0 && !IsOpenXrLayerSessionActive()) {
        ProcessOculusLayer(session, layerHeaders[0]);
    }

    return g_origSubmitFrame(session, frameIndex, viewScaleDesc, layerHeaders, layerCount);
}

ovrResult Hooked_ovr_SubmitFrame2(ovrSession session, long long frameIndex, const ovrViewScaleDesc* viewScaleDesc,
                                  ovrLayerHeader const* const* layerHeaders, unsigned int layerCount, unsigned int flags) {
    ApplyOculusFramePacing(session);
    VrsFoveatedD3D11::Instance().EndFrame();

    if (layerHeaders && layerCount > 0 && !IsOpenXrLayerSessionActive()) {
        ProcessOculusLayer(session, layerHeaders[0]);
    }

    return g_origSubmitFrame2(session, frameIndex, viewScaleDesc, layerHeaders, layerCount, flags);
}

ovrResult Hooked_ovr_EndFrame(ovrSession session, long long frameIndex, const ovrViewScaleDesc* viewScaleDesc,
                              ovrLayerHeader const* const* layerHeaders, unsigned int layerCount) {
    ApplyOculusFramePacing(session);
    VrsFoveatedD3D11::Instance().EndFrame();

    if (layerHeaders && layerCount > 0 && !IsOpenXrLayerSessionActive()) {
        ProcessOculusLayer(session, layerHeaders[0]);
    }

    return g_origEndFrame(session, frameIndex, viewScaleDesc, layerHeaders, layerCount);
}

void Hooked_ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain) {
    for (int eye = 0; eye < 2; ++eye) {
        if (g_oculusCtx.outputChains[eye].chain == chain) {
            g_oculusCtx.outputChains[eye].chain = nullptr;
            g_oculusCtx.outputChains[eye].textures.clear();
        }
    }
    if (g_origDestroyTextureSwapChain) {
        g_origDestroyTextureSwapChain(session, chain);
    }
}

} // namespace

void InstallOculusHooks() {
    static std::atomic<bool> s_installed{ false };
    if (s_installed.load()) return;

    HMODULE mod = GetModuleHandleW(L"LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll");
    if (!mod) mod = GetModuleHandleW(L"LibPVRRT64_1_X.dll");
    if (!mod) return;

    MH_Initialize();

    auto fnSubmit = reinterpret_cast<PFN_ovr_SubmitFrame>(GetProcAddress(mod, "ovr_submitFrame"));
    if (fnSubmit) {
        MH_CreateHook(reinterpret_cast<void*>(fnSubmit), reinterpret_cast<void*>(&Hooked_ovr_SubmitFrame), reinterpret_cast<void**>(&g_origSubmitFrame));
        MH_EnableHook(reinterpret_cast<void*>(fnSubmit));
    }

    auto fnSubmit2 = reinterpret_cast<PFN_ovr_SubmitFrame2>(GetProcAddress(mod, "ovr_submitFrame2"));
    if (fnSubmit2) {
        MH_CreateHook(reinterpret_cast<void*>(fnSubmit2), reinterpret_cast<void*>(&Hooked_ovr_SubmitFrame2), reinterpret_cast<void**>(&g_origSubmitFrame2));
        MH_EnableHook(reinterpret_cast<void*>(fnSubmit2));
    }

    auto fnEnd = reinterpret_cast<PFN_ovr_EndFrame>(GetProcAddress(mod, "ovr_endFrame"));
    if (fnEnd) {
        MH_CreateHook(reinterpret_cast<void*>(fnEnd), reinterpret_cast<void*>(&Hooked_ovr_EndFrame), reinterpret_cast<void**>(&g_origEndFrame));
        MH_EnableHook(reinterpret_cast<void*>(fnEnd));
    }

    auto fnDestroy = reinterpret_cast<PFN_ovr_DestroyTextureSwapChain>(GetProcAddress(mod, "ovr_destroyTextureSwapChain"));
    if (fnDestroy) {
        MH_CreateHook(reinterpret_cast<void*>(fnDestroy), reinterpret_cast<void*>(&Hooked_ovr_DestroyTextureSwapChain), reinterpret_cast<void**>(&g_origDestroyTextureSwapChain));
        MH_EnableHook(reinterpret_cast<void*>(fnDestroy));
    }

    s_installed.store(true);
    LogMessage(LogLevel::Info, "Oculus LibOVR hooks installed with full D3D11 NIS support (SubmitFrame, SubmitFrame2, EndFrame).");
}

} // namespace vrpacing

#endif