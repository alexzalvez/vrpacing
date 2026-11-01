#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gfx/vrs_foveated_d3d11.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "minhook/include/MinHook.h"
#include "common/logging_win.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

// [A5] & [F3] Proportional candidate detection constants
// Adapted from OpenXR-Toolkit (XR_APILAYER_MBUCCHIA_toolkit/vrs.cpp), MIT License, Copyright (c) 2021-2022 Matthieu Bucchianeri.
constexpr float kAspectTolerance = 0.01f;
constexpr float kMinScale = 0.75f; // [F3] Raised to 0.75 to exclude half-resolution post-process buffers
constexpr float kMaxScale = 1.50f;
constexpr float kSquareAspectTolerance = 0.02f;

constexpr float kInnerRadius = 0.60f;
constexpr float kMidRadius = 1.00f;
constexpr size_t kMaxUniqueRtLogs = 48;
constexpr uint32_t kMaxNvFailLogs = 5;

// [A6] Thread-local suppression depth counter
thread_local int g_vrsSuppressDepth = 0;
// [F5] Thread-local reentrancy guard
thread_local bool g_inVrsHook = false;

using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    UINT,
    ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*);

using OMSetRenderTargetsAndUAVsFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    UINT,
    ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*,
    UINT,
    UINT,
    ID3D11UnorderedAccessView* const*,
    const UINT*);

OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUAVsFn g_origOMSetRenderTargetsAndUAVs = nullptr;

// [F5] Restored g_inVrsHook reentrancy guard in addition to IsSuppressed()
void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D11DeviceContext* self,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {
    if (g_origOMSetRenderTargets) {
        g_origOMSetRenderTargets(self, numViews, ppRenderTargetViews, pDepthStencilView);
    }
    if (g_inVrsHook || VrsFoveatedD3D11::IsSuppressed()) {
        return;
    }
    g_inVrsHook = true;
    VrsFoveatedD3D11::Instance().OnPostOMSetRenderTargets(
        self, numViews, ppRenderTargetViews, pDepthStencilView);
    g_inVrsHook = false;
}

void STDMETHODCALLTYPE Hooked_OMSetRenderTargetsAndUAVs(
    ID3D11DeviceContext* self,
    UINT numRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT uavStartSlot,
    UINT numUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    if (g_origOMSetRenderTargetsAndUAVs) {
        g_origOMSetRenderTargetsAndUAVs(
            self, numRTVs, ppRenderTargetViews, pDepthStencilView,
            uavStartSlot, numUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    }
    if (g_inVrsHook || VrsFoveatedD3D11::IsSuppressed()) {
        return;
    }
    if (numRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        g_inVrsHook = true;
        VrsFoveatedD3D11::Instance().OnPostOMSetRenderTargets(
            self, numRTVs, ppRenderTargetViews, pDepthStencilView);
        g_inVrsHook = false;
    }
}

uint8_t DistanceToVRSLevel(float distance) {
    if (distance < kInnerRadius) {
        return 0; // 1x1 (100%)
    }
    if (distance < kMidRadius) {
        return 1; // 2x1 (1/2x)
    }
    return 2;     // 4x4 (1/16x)
}

// [A3] Unified pixel-space centered pattern generator with ceil tile coverage
std::vector<uint8_t> CreateCenteredPattern(int tilesW, int tilesH, int rtW, int rtH, bool doubleWide) {
    std::vector<uint8_t> data(static_cast<size_t>(tilesW) * static_cast<size_t>(tilesH), 0);
    if (rtW <= 0 || rtH <= 0 || tilesW <= 0 || tilesH <= 0) {
        return data;
    }

    const float eyeW = doubleWide ? (static_cast<float>(rtW) * 0.5f) : static_cast<float>(rtW);
    const float fRtH = static_cast<float>(rtH);
    if (eyeW <= 0.0f || fRtH <= 0.0f) {
        return data;
    }

    for (int y = 0; y < tilesH; ++y) {
        const float py = (std::min)((static_cast<float>(y) + 0.5f) * static_cast<float>(NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT),
                                    static_cast<float>(rtH - 1));
        const float ny = (py / fRtH) * 2.0f - 1.0f;

        for (int x = 0; x < tilesW; ++x) {
            const float px = (std::min)((static_cast<float>(x) + 0.5f) * static_cast<float>(NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH),
                                        static_cast<float>(rtW - 1));
            const float localX = (doubleWide && px >= eyeW) ? (px - eyeW) : px;
            const float nx = (localX / eyeW) * 2.0f - 1.0f;
            const float d = std::sqrt(nx * nx + ny * ny);
            data[static_cast<size_t>(y) * static_cast<size_t>(tilesW) + static_cast<size_t>(x)] = DistanceToVRSLevel(d);
        }
    }
    return data;
}

uint64_t HashRtCombo(uint32_t w,
                     uint32_t h,
                     uint32_t arr,
                     DXGI_FORMAT fmt,
                     uint32_t ms,
                     D3D11_RTV_DIMENSION dim,
                     const char* res) {
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(w);
    mix(h);
    mix(arr);
    mix(static_cast<uint32_t>(fmt));
    mix(ms);
    mix(static_cast<uint32_t>(dim));
    if (res) {
        for (const char* p = res; *p != '\0'; ++p) {
            mix(static_cast<uint8_t>(*p));
        }
    }
    return hash;
}

} // namespace

// [F4] Safe static teardown: heap object is intentionally never deleted
VrsFoveatedD3D11& VrsFoveatedD3D11::Instance() {
    static VrsFoveatedD3D11* s_instance = new VrsFoveatedD3D11();
    return *s_instance;
}

// [A6] Suppression scope helpers
void VrsFoveatedD3D11::EnterSuppressScope() noexcept {
    ++g_vrsSuppressDepth;
}

void VrsFoveatedD3D11::LeaveSuppressScope() noexcept {
    if (g_vrsSuppressDepth > 0) {
        --g_vrsSuppressDepth;
    }
}

bool VrsFoveatedD3D11::IsSuppressed() noexcept {
    return g_vrsSuppressDepth > 0;
}

void VrsFoveatedD3D11::LogNvapiFailureRateLimited(const char* opName, NvAPI_Status status) {
    nvFailCount_.fetch_add(1, std::memory_order_relaxed);
    if (nvFailLogCount_ < kMaxNvFailLogs) {
        ++nvFailLogCount_;
        LogMessage(LogLevel::Error, std::string("VRS: NVAPI call ") + opName +
                   " failed with status=" + std::to_string(static_cast<int>(status)));
    }
}

void VrsFoveatedD3D11::LogUniqueRenderTargetLocked(uint32_t w,
                                                   uint32_t h,
                                                   uint32_t arraySize,
                                                   DXGI_FORMAT format,
                                                   uint32_t samples,
                                                   D3D11_RTV_DIMENSION rtvDim,
                                                   const char* resultLabel) {
    if (loggedUniqueRts_.size() >= kMaxUniqueRtLogs) {
        return;
    }
    const uint64_t key = HashRtCombo(w, h, arraySize, format, samples, rtvDim, resultLabel);
    if (loggedUniqueRts_.insert(key).second) {
        LogMessage(LogLevel::Info, "VRS: RT " + std::to_string(w) + "x" + std::to_string(h) +
                   " arr=" + std::to_string(arraySize) +
                   " fmt=" + std::to_string(static_cast<uint32_t>(format)) +
                   " ms=" + std::to_string(samples) +
                   " dim=" + std::to_string(static_cast<uint32_t>(rtvDim)) +
                   " -> " + std::string(resultLabel));
    }
}

// [F3] & [A9] Detach NVAPI SRRVs and release textures ONLY in Shutdown() and on device change
void VrsFoveatedD3D11::ClearPatternCacheLocked() {
    for (size_t i = 0; i < static_cast<size_t>(VrsLayout::Count); ++i) {
        for (auto& entry : patternCache_[i]) {
            (void)entry.view.Detach();
            entry.tex.Reset();
        }
        patternCache_[i].clear();
    }
    cacheFullLogged_ = false;
}

bool VrsFoveatedD3D11::EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) return false;

    if (device_.Get() != dev) {
        // [A9] & [F3] Clear pattern cache and detach SRRVs on device change
        ClearPatternCacheLocked();

        vrsCurrentlyBound_ = false;
        fastBound_.store(false, std::memory_order_relaxed);
        loggedUniqueRts_.clear();

        device_ = dev;
        context_ = ctx;
        nvapiChecked_ = false;
        nvapiSupported_ = false;
    } else if (context_.Get() != ctx) {
        context_ = ctx;
    }

    if (!nvapiChecked_) {
        nvapiChecked_ = true;
        if (NvAPI_Initialize() != NVAPI_OK) {
            LogMessage(LogLevel::Warn, "VRS: NvAPI_Initialize failed (non-NVIDIA GPU or driver unavailable).");
            nvapiSupported_ = false;
            return false;
        }

        NV_D3D1x_GRAPHICS_CAPS caps{};
        caps.version = NV_D3D1x_GRAPHICS_CAPS_VER;
        if (NvAPI_D3D1x_GetGraphicsCapabilities(dev, NV_D3D1x_GRAPHICS_CAPS_VER, &caps) != NVAPI_OK ||
            !caps.bVariablePixelRateShadingSupported) {
            LogMessage(LogLevel::Warn, "VRS: Variable Pixel Rate Shading not supported on this GPU.");
            nvapiSupported_ = false;
            return false;
        }

        nvapiSupported_ = true;
        LogMessage(LogLevel::Info, "VRS: NVIDIA NVAPI initialized successfully (Rings: 60% 1x, 100% 1/2x, >100% 1/16x).");
    }

    if (nvapiSupported_ && !hooksInstalled_ && !hookInstallAttempted_) {
        InstallContextHooksLocked(ctx);
    }

    return nvapiSupported_ && hooksInstalled_;
}

// [A1] Hook installation correctness: publish original before MH_EnableHook, verify status, single attempt
void VrsFoveatedD3D11::InstallContextHooksLocked(ID3D11DeviceContext* ctx) {
    if (hooksInstalled_ || hookInstallAttempted_ || !ctx) return;
    hookInstallAttempted_ = true;

    const MH_STATUS stInit = MH_Initialize();
    if (stInit != MH_OK && stInit != MH_ERROR_ALREADY_INITIALIZED) {
        LogMessage(LogLevel::Error, std::string("VRS: MH_Initialize failed with status=") +
                   MH_StatusToString(stInit));
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(ctx);
    if (!vtbl) return;

    bool ok33 = false;
    bool ok34 = false;

    // Hook vtbl[33]: OMSetRenderTargets
    if (vtbl[33]) {
        const MH_STATUS stCreate33 = MH_CreateHook(
            vtbl[33],
            reinterpret_cast<void*>(&Hooked_OMSetRenderTargets),
            reinterpret_cast<void**>(&g_origOMSetRenderTargets));
        if (stCreate33 == MH_OK && g_origOMSetRenderTargets != nullptr) {
            const MH_STATUS stEnable33 = MH_EnableHook(vtbl[33]);
            if (stEnable33 == MH_OK || stEnable33 == MH_ERROR_ENABLED) {
                ok33 = true;
            } else {
                MH_RemoveHook(vtbl[33]);
                g_origOMSetRenderTargets = nullptr;
                LogMessage(LogLevel::Error, std::string("VRS: MH_EnableHook(vtbl[33]) failed: ") +
                           MH_StatusToString(stEnable33));
            }
        } else {
            g_origOMSetRenderTargets = nullptr;
            LogMessage(LogLevel::Error, std::string("VRS: MH_CreateHook(vtbl[33]) failed: ") +
                       MH_StatusToString(stCreate33));
        }
    }

    // Hook vtbl[34]: OMSetRenderTargetsAndUnorderedAccessViews
    if (vtbl[34]) {
        const MH_STATUS stCreate34 = MH_CreateHook(
            vtbl[34],
            reinterpret_cast<void*>(&Hooked_OMSetRenderTargetsAndUAVs),
            reinterpret_cast<void**>(&g_origOMSetRenderTargetsAndUAVs));
        if (stCreate34 == MH_OK && g_origOMSetRenderTargetsAndUAVs != nullptr) {
            const MH_STATUS stEnable34 = MH_EnableHook(vtbl[34]);
            if (stEnable34 == MH_OK || stEnable34 == MH_ERROR_ENABLED) {
                ok34 = true;
            } else {
                MH_RemoveHook(vtbl[34]);
                g_origOMSetRenderTargetsAndUAVs = nullptr;
                LogMessage(LogLevel::Error, std::string("VRS: MH_EnableHook(vtbl[34]) failed: ") +
                           MH_StatusToString(stEnable34));
            }
        } else {
            g_origOMSetRenderTargetsAndUAVs = nullptr;
            LogMessage(LogLevel::Error, std::string("VRS: MH_CreateHook(vtbl[34]) failed: ") +
                       MH_StatusToString(stCreate34));
        }
    }

    hooksInstalled_ = ok33;
    LogMessage(LogLevel::Info, std::string("VRS: hooks OMSetRenderTargets=") +
               (ok33 ? "OK" : "FAIL") + " OMSetRenderTargetsAndUAVs=" + (ok34 ? "OK" : "FAIL"));
}

// [A4], [F1] & [F3] UpdateTarget obeys `enabled` as passed and preserves cached patterns on size changes
void VrsFoveatedD3D11::UpdateTarget(ID3D11Device* dev,
                                    ID3D11DeviceContext* ctx,
                                    bool enabled,
                                    uint32_t eyeWidth,
                                    uint32_t eyeHeight,
                                    int debugMode) {
    if (!enabled && !fastEnabled_.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    debugMode_ = debugMode;

    if (!enabled || !dev || !ctx || eyeWidth == 0 || eyeHeight == 0) {
        if (enabled_ && context_) {
            DisableVRSLocked(context_.Get(), true);
        }
        enabled_ = false;
        fastEnabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (!EnsureInitializedLocked(dev, ctx)) {
        enabled_ = false;
        fastEnabled_.store(false, std::memory_order_relaxed);
        return;
    }

    if (targetEyeWidth_ != eyeWidth || targetEyeHeight_ != eyeHeight) {
        // [F3] Update target dimensions without detaching or recreating existing pattern cache entries
        targetEyeWidth_ = eyeWidth;
        targetEyeHeight_ = eyeHeight;
        loggedUniqueRts_.clear();
    }

    enabled_ = true;
    fastEnabled_.store(true, std::memory_order_relaxed);
}

// [A8] EndFrame periodic telemetry
void VrsFoveatedD3D11::EndFrame() {
    if (!fastEnabled_.load(std::memory_order_relaxed) &&
        !fastBound_.load(std::memory_order_relaxed)) {
        return;
    }

    const uint64_t frames = framesCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frames % 900 == 0) {
        const uint64_t candidates = candidateBinds_.load(std::memory_order_relaxed);
        const uint64_t enables = enableCount_.load(std::memory_order_relaxed);
        const uint64_t disables = disableCount_.load(std::memory_order_relaxed);
        const uint64_t nvfail = nvFailCount_.load(std::memory_order_relaxed);
        int mode = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode = debugMode_;
        }
        LogMessage(LogLevel::Info, "VRS: stats frames=" + std::to_string(frames) +
                   " candidates=" + std::to_string(candidates) +
                   " enables=" + std::to_string(enables) +
                   " disables=" + std::to_string(disables) +
                   " nvfail=" + std::to_string(nvfail) +
                   " mode=" + std::to_string(mode));
    }
}

// [A9] & [F3] Shutdown detaches NVAPI SRRVs and resets D3D11 textures
void VrsFoveatedD3D11::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_) {
        DisableVRSLocked(context_.Get(), true);
    }

    // [A9] Intentionally leak NVAPI ID3D11NvShadingRateResourceView pointers during teardown
    // to prevent occasional driver crashes when the engine destroys the D3D11 device first
    // (Adapted from OpenXR-Toolkit XR_APILAYER_MBUCCHIA_toolkit/vrs.cpp:153-166).
    ClearPatternCacheLocked();

    context_.Reset();
    device_.Reset();
    enabled_ = false;
    vrsCurrentlyBound_ = false;
    targetEyeWidth_ = 0;
    targetEyeHeight_ = 0;
    loggedUniqueRts_.clear();
    fastEnabled_.store(false, std::memory_order_relaxed);
    fastBound_.store(false, std::memory_order_relaxed);
}

// [A2] & [F2] Public thread-safe DisableVRS wrapper
void VrsFoveatedD3D11::DisableVRS(ID3D11DeviceContext* ctx, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    DisableVRSLocked(ctx, force);
}

// [A5] Proportional candidate detection & [A8] diagnostics
void VrsFoveatedD3D11::OnPostOMSetRenderTargets(
    ID3D11DeviceContext* ctx,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* /*pDepthStencilView*/) {
    if (!fastEnabled_.load(std::memory_order_relaxed) &&
        !fastBound_.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!nvapiSupported_ || !ctx || ctx != context_.Get()) {
        return;
    }

    if (!enabled_ || targetEyeWidth_ == 0 || targetEyeHeight_ == 0 ||
        numViews == 0 || !ppRenderTargetViews || !ppRenderTargetViews[0]) {
        DisableVRSLocked(ctx, false);
        return;
    }

    ID3D11RenderTargetView* rtv0 = ppRenderTargetViews[0];
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtv0->GetDesc(&rtvDesc);

    ComPtr<ID3D11Resource> res;
    rtv0->GetResource(res.GetAddressOf());
    if (!res) {
        DisableVRSLocked(ctx, false);
        return;
    }

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex)) || !tex) {
        LogUniqueRenderTargetLocked(0, 0, 0, rtvDesc.Format, 0, rtvDesc.ViewDimension, "REJECT_DIM");
        DisableVRSLocked(ctx, false);
        return;
    }

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    const bool validViewDim =
        (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY);

    if (!validViewDim || td.Width == 0 || td.Height == 0 || td.ArraySize == 0 || td.ArraySize > 2) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, "REJECT_DIM");
        DisableVRSLocked(ctx, false);
        return;
    }

    const float targetAspect = static_cast<float>(targetEyeWidth_) / static_cast<float>(targetEyeHeight_);

    // [A5] Square rejection only if the target itself is not square (|targetAspect - 1| > 0.02)
    if (std::abs(targetAspect - 1.0f) > kSquareAspectTolerance && td.Width == td.Height) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, "REJECT_SQUARE");
        DisableVRSLocked(ctx, false);
        return;
    }

    const float a1 = static_cast<float>(td.Width) / static_cast<float>(td.Height);
    const float a2 = (static_cast<float>(td.Width) * 0.5f) / static_cast<float>(td.Height);
    const bool doubleWide = std::abs(a2 - targetAspect) <= kAspectTolerance;
    const bool single = !doubleWide && (std::abs(a1 - targetAspect) <= kAspectTolerance);

    if (!doubleWide && !single) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, "REJECT_ASPECT");
        DisableVRSLocked(ctx, false);
        return;
    }

    const float eyeW = doubleWide ? (static_cast<float>(td.Width) * 0.5f) : static_cast<float>(td.Width);
    const float scale = eyeW / static_cast<float>(targetEyeWidth_);
    if (scale < kMinScale || scale > kMaxScale) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, "REJECT_SCALE");
        DisableVRSLocked(ctx, false);
        return;
    }

    // [A4] Inspect RTV descriptor for slice count
    uint32_t rtvSlices = 1;
    if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        rtvSlices = rtvDesc.Texture2DArray.ArraySize;
    } else if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
        rtvSlices = rtvDesc.Texture2DMSArray.ArraySize;
    }

    if (doubleWide && rtvSlices > 1) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, "REJECT_DIM");
        DisableVRSLocked(ctx, false);
        return;
    }

    const bool isArrayTarget = (!doubleWide && rtvSlices >= 2 && td.ArraySize >= 2);
    const char* resultLabel = doubleWide ? "DOUBLEWIDE" : (isArrayTarget ? "ARRAY" : "SINGLE");

    LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format,
                                td.SampleDesc.Count, rtvDesc.ViewDimension, resultLabel);
    candidateBinds_.fetch_add(1, std::memory_order_relaxed);

    // [A8] debugMode == 1: passive mode (detect + log, never bind VRS)
    if (debugMode_ == 1) {
        DisableVRSLocked(ctx, false);
        return;
    }

    const VrsLayout layout = doubleWide ? VrsLayout::Combined
                                        : (isArrayTarget ? VrsLayout::Array : VrsLayout::SingleEye);
    const char* viewLabel = doubleWide ? "Combined" : (isArrayTarget ? "Array" : "SingleEye");

    ID3D11NvShadingRateResourceView* srrv =
        GetOrCreatePatternLocked(layout, static_cast<int>(td.Width), static_cast<int>(td.Height));
    if (srrv) {
        ApplyVRSViewLocked(ctx, srrv, viewLabel);
    } else {
        DisableVRSLocked(ctx, false);
    }
}

void VrsFoveatedD3D11::EnableVRS(ID3D11DeviceContext* ctx) {
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd{};
    srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    srd.numViewports = NVAPI_MAX_VIEWPORTS;

    NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 vsrd[NVAPI_MAX_VIEWPORTS]{};
    for (int i = 0; i < NVAPI_MAX_VIEWPORTS; ++i) {
        vsrd[i].enableVariablePixelShadingRate = true;
        std::memset(vsrd[i].shadingRateTable, 0, sizeof(vsrd[i].shadingRateTable));

        if (debugMode_ == 2) {
            // [A8] debugMode 2: transparent (all 1x1)
            vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        } else if (debugMode_ == 3) {
            // [A8] debugMode 3: extreme (ring 1 and ring 2 forced to 4x4)
            vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
            vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
            vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        } else {
            // [A8] debugMode 0: normal fixed 3-ring configuration
            vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
            vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
            vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        }
    }
    srd.pViewports = vsrd;

    const NvAPI_Status st = NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &srd);
    if (st == NVAPI_OK) {
        vrsCurrentlyBound_ = true;
        fastBound_.store(true, std::memory_order_relaxed);
        enableCount_.fetch_add(1, std::memory_order_relaxed);
    } else {
        LogNvapiFailureRateLimited("NvAPI_D3D11_RSSetViewportsPixelShadingRates(enable)", st);
    }
}

// [A2] & [F2] Robust DisableVRSLocked: checks nvapiSupported_ and registered context before any NVAPI call
void VrsFoveatedD3D11::DisableVRSLocked(ID3D11DeviceContext* ctx, bool force) {
    if (!nvapiSupported_ || !ctx || ctx != context_.Get()) {
        return;
    }
    if (!force && !vrsCurrentlyBound_) {
        return;
    }

    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd{};
    srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    srd.numViewports = 0;
    srd.pViewports = nullptr;

    const NvAPI_Status st1 = NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &srd);
    if (st1 != NVAPI_OK) {
        LogNvapiFailureRateLimited("NvAPI_D3D11_RSSetViewportsPixelShadingRates(disable)", st1);
    }

    const NvAPI_Status st2 = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, nullptr);
    if (st2 != NVAPI_OK) {
        LogNvapiFailureRateLimited("NvAPI_D3D11_RSSetShadingRateResourceView(nullptr)", st2);
    }

    disableCount_.fetch_add(1, std::memory_order_relaxed);
    vrsCurrentlyBound_ = false;
    fastBound_.store(false, std::memory_order_relaxed);
}

// [A2] Set vrsCurrentlyBound_/fastBound_ immediately when SRRV bind succeeds
void VrsFoveatedD3D11::ApplyVRSViewLocked(ID3D11DeviceContext* ctx,
                                          ID3D11NvShadingRateResourceView* view,
                                          const char* label) {
    if (!view) return;
    const NvAPI_Status st = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, view);
    if (st == NVAPI_OK) {
        vrsCurrentlyBound_ = true;
        fastBound_.store(true, std::memory_order_relaxed);
        EnableVRS(ctx);
    } else {
        std::string op = std::string("NvAPI_D3D11_RSSetShadingRateResourceView(") + label + ")";
        LogNvapiFailureRateLimited(op.c_str(), st);
    }
}

// [A3], [A4] & [F3] Bounded pattern cache per layout (max 4 entries per layout, never recreated)
ID3D11NvShadingRateResourceView* VrsFoveatedD3D11::GetOrCreatePatternLocked(
    VrsLayout layout, int rtWidth, int rtHeight) {
    if (!device_ || rtWidth <= 0 || rtHeight <= 0) return nullptr;

    const int tilesW = (std::max)(1, (rtWidth + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
    const int tilesH = (std::max)(1, (rtHeight + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

    const size_t layoutIdx = static_cast<size_t>(layout);
    auto& cache = patternCache_[layoutIdx];

    for (const auto& entry : cache) {
        if (entry.tilesW == tilesW && entry.tilesH == tilesH) {
            return entry.view.Get();
        }
    }

    if (cache.size() >= kMaxPatternsPerLayout) {
        if (!cacheFullLogged_) {
            cacheFullLogged_ = true;
            LogMessage(LogLevel::Warn, "VRS: pattern cache full, RT " +
                       std::to_string(rtWidth) + "x" + std::to_string(rtHeight) + " skipped");
        }
        return nullptr;
    }

    const bool doubleWide = (layout == VrsLayout::Combined);
    const bool isArray = (layout == VrsLayout::Array);
    std::vector<uint8_t> patternData = CreateCenteredPattern(tilesW, tilesH, rtWidth, rtHeight, doubleWide);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(tilesW);
    td.Height = static_cast<UINT>(tilesH);
    td.ArraySize = isArray ? 2u : 1u;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    td.MipLevels = 1;

    D3D11_SUBRESOURCE_DATA srd[2]{};
    for (UINT slice = 0; slice < td.ArraySize; ++slice) {
        srd[slice].pSysMem = patternData.data();
        srd[slice].SysMemPitch = static_cast<UINT>(tilesW);
        srd[slice].SysMemSlicePitch = 0;
    }

    PatternEntry newEntry{};
    newEntry.tilesW = tilesW;
    newEntry.tilesH = tilesH;

    HRESULT hr = device_->CreateTexture2D(&td, srd, newEntry.tex.GetAddressOf());
    if (FAILED(hr)) {
        LogMessage(LogLevel::Error, "VRS: Failed to create pattern texture. HR=" + std::to_string(hr));
        return nullptr;
    }

    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC srvd{};
    srvd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    srvd.Format = td.Format;
    if (isArray) {
        srvd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2DARRAY;
        srvd.Texture2DArray.MipSlice = 0;
        srvd.Texture2DArray.FirstArraySlice = 0;
        srvd.Texture2DArray.ArraySize = 2;
    } else {
        srvd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipSlice = 0;
    }

    const NvAPI_Status st = NvAPI_D3D11_CreateShadingRateResourceView(
        device_.Get(), newEntry.tex.Get(), &srvd, newEntry.view.GetAddressOf());
    if (st != NVAPI_OK) {
        LogNvapiFailureRateLimited("NvAPI_D3D11_CreateShadingRateResourceView", st);
        newEntry.tex.Reset();
        return nullptr;
    }

    ID3D11NvShadingRateResourceView* outView = newEntry.view.Get();
    cache.push_back(std::move(newEntry));

    const char* prefix = doubleWide ? "Combined Side-by-Side" : (isArray ? "ArraySize=2" : "SingleEye");
    LogMessage(LogLevel::Info, std::string("VRS: ") + prefix + " pattern texture created (" +
               std::to_string(tilesW) + "x" + std::to_string(tilesH) +
               " tiles for " + std::to_string(rtWidth) + "x" + std::to_string(rtHeight) + ").");
    return outView;
}

} // namespace vrpacing

#endif // _WIN32