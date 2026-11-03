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
#include "common/vrs_filter.h"
#include "common/vrs_pattern.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_vrsDetaching{ false };

namespace {

constexpr uint32_t kMaxNvFailLogs = 5;
constexpr UINT kMaxViewports = 16;

thread_local int g_vrsSuppressDepth = 0;
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

void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D11DeviceContext* self,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {
    if (g_origOMSetRenderTargets) {
        g_origOMSetRenderTargets(self, numViews, ppRenderTargetViews, pDepthStencilView);
    }
    if (g_vrsDetaching.load(std::memory_order_relaxed) || g_inVrsHook || VrsFoveatedD3D11::IsSuppressed()) {
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
    if (g_vrsDetaching.load(std::memory_order_relaxed) || g_inVrsHook || VrsFoveatedD3D11::IsSuppressed()) {
        return;
    }
    if (numRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        g_inVrsHook = true;
        VrsFoveatedD3D11::Instance().OnPostOMSetRenderTargets(
            self, numRTVs, ppRenderTargetViews, pDepthStencilView);
        g_inVrsHook = false;
    }
}

uint64_t HashRtCombo(uint32_t w,
                     uint32_t h,
                     uint32_t arr,
                     DXGI_FORMAT texFmt,
                     DXGI_FORMAT rtvFmt,
                     uint32_t ms,
                     D3D11_RTV_DIMENSION dim,
                     bool hasDsv,
                     const char* res) {
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t v) {
        hash ^= v;
        hash *= 1099511628211ULL;
    };
    mix(w);
    mix(h);
    mix(arr);
    mix(static_cast<uint32_t>(texFmt));
    mix(static_cast<uint32_t>(rtvFmt));
    mix(ms);
    mix(static_cast<uint32_t>(dim));
    mix(hasDsv ? 1u : 0u);
    if (res) {
        for (const char* p = res; *p != '\0'; ++p) {
            mix(static_cast<uint8_t>(*p));
        }
    }
    return hash;
}

} // namespace

VrsFoveatedD3D11& VrsFoveatedD3D11::Instance() {
    static VrsFoveatedD3D11* s_instance = new VrsFoveatedD3D11();
    return *s_instance;
}

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

void VrsFoveatedD3D11::RegisterExcludedTexture(ID3D11Texture2D* tex) {
    if (!tex) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* p : excludedTextures_) {
        if (p == tex) return;
    }
    excludedTextures_.push_back(tex);
}

void VrsFoveatedD3D11::UnregisterExcludedTexture(ID3D11Texture2D* tex) {
    if (!tex) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(excludedTextures_.begin(), excludedTextures_.end(), tex);
    if (it != excludedTextures_.end()) {
        excludedTextures_.erase(it);
    }
}

void VrsFoveatedD3D11::ClearExcludedTextures() {
    std::lock_guard<std::mutex> lock(mutex_);
    excludedTextures_.clear();
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
                                                   DXGI_FORMAT texFmt,
                                                   DXGI_FORMAT rtvFmt,
                                                   uint32_t samples,
                                                   D3D11_RTV_DIMENSION rtvDim,
                                                   bool hasDsv,
                                                   const char* resultLabel) {
    if (loggedUniqueRts_.size() >= kMaxUniqueRtLogs) {
        return;
    }
    const uint64_t key = HashRtCombo(w, h, arraySize, texFmt, rtvFmt, samples, rtvDim, hasDsv, resultLabel);
    if (loggedUniqueRts_.insert(key).second) {
        LogMessage(LogLevel::Info, "VRS: RT " + std::to_string(w) + "x" + std::to_string(h) +
                   " arr=" + std::to_string(arraySize) +
                   " fmt=" + std::to_string(static_cast<uint32_t>(texFmt)) +
                   " rtvfmt=" + std::to_string(static_cast<uint32_t>(rtvFmt)) +
                   " ms=" + std::to_string(samples) +
                   " dim=" + std::to_string(static_cast<uint32_t>(rtvDim)) +
                   " dsv=" + (hasDsv ? "1" : "0") +
                   " -> " + std::string(resultLabel));
    }
}

void VrsFoveatedD3D11::ClearPatternCacheLocked() {
    for (size_t i = 0; i < static_cast<size_t>(VrsLayout::Count); ++i) {
        for (auto& entry : patternCache_[i]) {
            (void)entry.view.Detach();
            entry.tex.Reset();
        }
        patternCache_[i].clear();
    }
    cacheFullLogged_ = false;
    lastBoundView_ = nullptr;
    lastBoundDebugMode_ = -1;
}

bool VrsFoveatedD3D11::EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) return false;

    if (device_.Get() != dev) {
        ClearPatternCacheLocked();
        vrsCurrentlyBound_ = false;
        fastBound_.store(false, std::memory_order_relaxed);
        loggedUniqueRts_.clear();
        rtHistogram_.clear();

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

void VrsFoveatedD3D11::InstallContextHooksLocked(ID3D11DeviceContext* ctx) {
    if (hooksInstalled_ || hookInstallAttempted_ || !ctx) return;
    hookInstallAttempted_ = true;

    HMODULE hPin = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&Hooked_OMSetRenderTargets), &hPin) || !hPin) {
        LogMessage(LogLevel::Error, "VRS: CRITICAL: Failed to pin module in memory. Hook installation aborted.");
        return;
    }
    LogMessage(LogLevel::Info, "VRS: module pinned successfully in memory.");

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

void VrsFoveatedD3D11::UpdateTarget(ID3D11Device* dev,
                                    ID3D11DeviceContext* ctx,
                                    bool enabled,
                                    uint32_t eyeWidth,
                                    uint32_t eyeHeight,
                                    int debugMode,
                                    bool debugLogging) {
    debugLogging_.store(debugLogging, std::memory_order_relaxed);
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
        targetEyeWidth_ = eyeWidth;
        targetEyeHeight_ = eyeHeight;
        loggedUniqueRts_.clear();
        rtHistogram_.clear();
    }

    enabled_ = true;
    fastEnabled_.store(true, std::memory_order_relaxed);
}

void VrsFoveatedD3D11::EndFrame() {
    if (!fastEnabled_.load(std::memory_order_relaxed) &&
        !fastBound_.load(std::memory_order_relaxed)) {
        return;
    }

    const uint64_t frames = framesCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frames % 900 == 0 && debugLogging_.load(std::memory_order_relaxed)) {
        const uint64_t candidates = candidateBinds_.load(std::memory_order_relaxed);
        const uint64_t nvbinds = actualNvapiBinds_.load(std::memory_order_relaxed);
        const uint64_t enables = enableCount_.load(std::memory_order_relaxed);
        const uint64_t disables = disableCount_.load(std::memory_order_relaxed);
        const uint64_t nvfail = nvFailCount_.load(std::memory_order_relaxed);
        int mode = 0;
        std::string topStr;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            mode = debugMode_;

            std::vector<std::pair<RtHistogramKey, uint64_t>> sortedEntries(rtHistogram_.begin(), rtHistogram_.end());
            std::sort(sortedEntries.begin(), sortedEntries.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

            if (!sortedEntries.empty()) {
                topStr = " top=";
                const size_t limit = (std::min)(size_t(3), sortedEntries.size());
                for (size_t i = 0; i < limit; ++i) {
                    if (i > 0) topStr += " ";
                    const auto& k = sortedEntries[i].first;
                    topStr += std::to_string(k.w) + "x" + std::to_string(k.h) +
                              "/f" + std::to_string(k.fmt) +
                              "/ms" + std::to_string(k.ms) +
                              ":" + std::to_string(sortedEntries[i].second);
                }
            }
            rtHistogram_.clear();
        }

        LogMessage(LogLevel::Info, "VRS: stats frames=" + std::to_string(frames) +
                   " candidates=" + std::to_string(candidates) +
                   " nvbinds=" + std::to_string(nvbinds) +
                   " enables=" + std::to_string(enables) +
                   " disables=" + std::to_string(disables) +
                   " nvfail=" + std::to_string(nvfail) +
                   " mode=" + std::to_string(mode) +
                   topStr);
    }
}

void VrsFoveatedD3D11::Shutdown(const char* reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!device_ && !context_ && !nvapiSupported_) {
        return;
    }

    LogMessage(LogLevel::Info, std::string("VRS: Shutdown (reason=") + (reason ? reason : "Unknown") + ")");

    if (context_) {
        DisableVRSLocked(context_.Get(), true);
    }

    ClearPatternCacheLocked();
    excludedTextures_.clear();
    rtHistogram_.clear();

    context_.Reset();
    device_.Reset();
    enabled_ = false;
    vrsCurrentlyBound_ = false;
    targetEyeWidth_ = 0;
    targetEyeHeight_ = 0;
    lastBoundView_ = nullptr;
    lastBoundDebugMode_ = -1;
    loggedUniqueRts_.clear();
    fastEnabled_.store(false, std::memory_order_relaxed);
    fastBound_.store(false, std::memory_order_relaxed);
}

void VrsFoveatedD3D11::DisableVRS(ID3D11DeviceContext* ctx, bool force) {
    std::lock_guard<std::mutex> lock(mutex_);
    DisableVRSLocked(ctx, force);
}

void VrsFoveatedD3D11::OnPostOMSetRenderTargets(
    ID3D11DeviceContext* ctx,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {
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
        if (debugLogging_.load(std::memory_order_relaxed)) {
            LogUniqueRenderTargetLocked(0, 0, 0, DXGI_FORMAT_UNKNOWN, rtvDesc.Format, 0, rtvDesc.ViewDimension, (pDepthStencilView != nullptr), "REJECT_DIM");
        }
        DisableVRSLocked(ctx, false);
        return;
    }

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    const bool hasDsv = (pDepthStencilView != nullptr);

    const bool validViewDim =
        (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
         rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY);

    bool isExcl = false;
    for (auto* pExcl : excludedTextures_) {
        if (pExcl == tex.Get()) {
            isExcl = true;
            break;
        }
    }

    bool validDsvTex = false;
    D3D11_TEXTURE2D_DESC dsvDesc{};
    if (pDepthStencilView) {
        ComPtr<ID3D11Resource> dsvRes;
        pDepthStencilView->GetResource(dsvRes.GetAddressOf());
        ComPtr<ID3D11Texture2D> dsvTex;
        if (dsvRes && SUCCEEDED(dsvRes.As(&dsvTex)) && dsvTex) {
            dsvTex->GetDesc(&dsvDesc);
            validDsvTex = true;
        }
    }

    uint32_t rtvSlices = 1;
    if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        rtvSlices = rtvDesc.Texture2DArray.ArraySize;
    } else if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
        rtvSlices = rtvDesc.Texture2DMSArray.ArraySize;
    }

    VrsFilterInput fIn{};
    fIn.rtW = td.Width;
    fIn.rtH = td.Height;
    fIn.rtArraySize = td.ArraySize;
    fIn.rtSamples = td.SampleDesc.Count;
    fIn.rtResourceFormat = td.Format;
    fIn.validViewDim = validViewDim;
    fIn.rtvSlices = rtvSlices;
    fIn.isExcludedSwapchain = isExcl;
    fIn.hasDsv = validDsvTex;
    fIn.dsvW = dsvDesc.Width;
    fIn.dsvH = dsvDesc.Height;
    fIn.dsvArraySize = dsvDesc.ArraySize;
    fIn.dsvSamples = dsvDesc.SampleDesc.Count;
    fIn.targetEyeW = targetEyeWidth_;
    fIn.targetEyeH = targetEyeHeight_;
    fIn.allowArrayAndDoubleWide = true;

    VrsFilterOutput fOut = EvaluateVrsCandidate(fIn);

    if (debugLogging_.load(std::memory_order_relaxed)) {
        LogUniqueRenderTargetLocked(td.Width, td.Height, td.ArraySize, td.Format, rtvDesc.Format,
                                    td.SampleDesc.Count, rtvDesc.ViewDimension, hasDsv, fOut.label);
    }

    if (!fOut.accepted) {
        DisableVRSLocked(ctx, false);
        return;
    }

    candidateBinds_.fetch_add(1, std::memory_order_relaxed);

    if (debugLogging_.load(std::memory_order_relaxed)) {
        const RtHistogramKey histKey{ td.Width, td.Height, static_cast<uint32_t>(td.Format), td.SampleDesc.Count, td.ArraySize };
        if (rtHistogram_.size() < kMaxHistogramEntries || rtHistogram_.find(histKey) != rtHistogram_.end()) {
            rtHistogram_[histKey]++;
        }
    }

    if (debugMode_ == 1) {
        DisableVRSLocked(ctx, false);
        return;
    }

    const VrsLayout layout = fOut.doubleWide ? VrsLayout::Combined
                                             : (fOut.isArray ? VrsLayout::Array : VrsLayout::SingleEye);
    const char* viewLabel = fOut.doubleWide ? "Combined" : (fOut.isArray ? "Array" : "SingleEye");

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
    srd.numViewports = kMaxViewports;

    NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 vsrd[kMaxViewports]{};
    for (UINT i = 0; i < kMaxViewports; ++i) {
        vsrd[i].enableVariablePixelShadingRate = true;
        std::memset(vsrd[i].shadingRateTable, 0, sizeof(vsrd[i].shadingRateTable));

        if (debugMode_ == 2) {
            vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        } else if (debugMode_ == 3) {
            vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
            vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
            vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
            vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        } else {
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
    lastBoundView_ = nullptr;
    lastBoundDebugMode_ = -1;
    fastBound_.store(false, std::memory_order_relaxed);
}

void VrsFoveatedD3D11::ApplyVRSViewLocked(ID3D11DeviceContext* ctx,
                                          ID3D11NvShadingRateResourceView* view,
                                          const char* label) {
    if (!view) return;

    if (vrsCurrentlyBound_ && lastBoundView_ == view && lastBoundDebugMode_ == debugMode_) {
        return;
    }

    const NvAPI_Status st = NvAPI_D3D11_RSSetShadingRateResourceView(ctx, view);
    if (st == NVAPI_OK) {
        vrsCurrentlyBound_ = true;
        lastBoundView_ = view;
        lastBoundDebugMode_ = debugMode_;
        fastBound_.store(true, std::memory_order_relaxed);
        actualNvapiBinds_.fetch_add(1, std::memory_order_relaxed);
        EnableVRS(ctx);
    } else {
        std::string op = std::string("NvAPI_D3D11_RSSetShadingRateResourceView(") + label + ")";
        LogNvapiFailureRateLimited(op.c_str(), st);
    }
}

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
    std::vector<uint8_t> patternData = CreateCenteredPatternD3D11(
        tilesW, tilesH, rtWidth, rtHeight, doubleWide,
        NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH, NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

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