#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gfx/vrs_foveated_d3d12.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "minhook/include/MinHook.h"
#include "common/logging_win.h"
#include "common/vrs_filter.h"
#include "common/vrs_pattern.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

thread_local bool g_inVrs12Hook = false;

using CreateRtvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    ID3D12Resource*,
    const D3D12_RENDER_TARGET_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);

using CreateDsvFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    ID3D12Resource*,
    const D3D12_DEPTH_STENCIL_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);

using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList*,
    UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*,
    BOOL,
    const D3D12_CPU_DESCRIPTOR_HANDLE*);

using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList4*,
    UINT,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
    D3D12_RENDER_PASS_FLAGS);

CreateRtvFn g_origCreateRtv = nullptr;
CreateDsvFn g_origCreateDsv = nullptr;
OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
BeginRenderPassFn g_origBeginRenderPass = nullptr;

void STDMETHODCALLTYPE Hooked_CreateRenderTargetView(
    ID3D12Device* self,
    ID3D12Resource* pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (g_origCreateRtv) {
        g_origCreateRtv(self, pResource, pDesc, DestDescriptor);
    }
    if (g_inVrs12Hook) return;
    g_inVrs12Hook = true;
    VrsFoveatedD3D12::Instance().OnCreateRenderTargetView(self, pResource, pDesc, DestDescriptor);
    g_inVrs12Hook = false;
}

void STDMETHODCALLTYPE Hooked_CreateDepthStencilView(
    ID3D12Device* self,
    ID3D12Resource* pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
    if (g_origCreateDsv) {
        g_origCreateDsv(self, pResource, pDesc, DestDescriptor);
    }
    if (g_inVrs12Hook) return;
    g_inVrs12Hook = true;
    VrsFoveatedD3D12::Instance().OnCreateDepthStencilView(self, pResource, pDesc, DestDescriptor);
    g_inVrs12Hook = false;
}

void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D12GraphicsCommandList* self,
    UINT NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (g_origOMSetRenderTargets) {
        g_origOMSetRenderTargets(self, NumRenderTargetDescriptors, pRenderTargetDescriptors,
                                 RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
    if (g_inVrs12Hook) return;
    g_inVrs12Hook = true;
    VrsFoveatedD3D12::Instance().OnPostOMSetRenderTargets(self, NumRenderTargetDescriptors,
                                                          pRenderTargetDescriptors,
                                                          RTsSingleHandleToDescriptorRange,
                                                          pDepthStencilDescriptor);
    g_inVrs12Hook = false;
}

void STDMETHODCALLTYPE Hooked_BeginRenderPass(
    ID3D12GraphicsCommandList4* self,
    UINT NumRenderTargets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS Flags) {
    if (g_origBeginRenderPass) {
        g_origBeginRenderPass(self, NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
    }
    if (g_inVrs12Hook) return;
    g_inVrs12Hook = true;
    VrsFoveatedD3D12::Instance().OnBeginRenderPass();
    g_inVrs12Hook = false;
}

} // namespace

VrsFoveatedD3D12& VrsFoveatedD3D12::Instance() {
    static VrsFoveatedD3D12* s_instance = new VrsFoveatedD3D12();
    return *s_instance;
}

void VrsFoveatedD3D12::ParseExtraFormatsCsv(const std::string& csv) {
    extraAllowedFormats_.clear();
    if (csv.empty()) return;

    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(0, item.find_first_not_of(" \t\r\n"));
        item.erase(item.find_last_not_of(" \t\r\n") + 1);
        if (item.empty()) continue;

        try {
            uint32_t fmtVal = static_cast<uint32_t>(std::stoul(item, nullptr, 0));
            extraAllowedFormats_.insert(fmtVal);
        } catch (...) {
        }
    }
}

bool VrsFoveatedD3D12::IsExtraFormatAllowed(DXGI_FORMAT fmt) const {
    if (extraAllowedFormats_.empty()) return false;
    return extraAllowedFormats_.find(static_cast<uint32_t>(fmt)) != extraAllowedFormats_.end();
}

bool VrsFoveatedD3D12::QueryTier2SupportLocked(ID3D12Device* dev) {
    if (capabilityChecked_) return tier2Supported_;
    capabilityChecked_ = true;

    if (!dev) return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
    HRESULT hr = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6));
    if (FAILED(hr)) {
        LogMessage(LogLevel::Warn, "VRS12: CheckFeatureSupport(OPTIONS6) failed. Tier2 VRS unsupported.");
        tier2Supported_ = false;
        return false;
    }

    tier2Supported_ = (opts6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2);
    tileSize_ = opts6.ShadingRateImageTileSize ? opts6.ShadingRateImageTileSize : 16;
    additionalShadingRates_ = opts6.AdditionalShadingRatesSupported;

    LogMessage(LogLevel::Info, "VRS12: Cap tier=" + std::to_string(static_cast<int>(opts6.VariableShadingRateTier)) +
               " tileSize=" + std::to_string(tileSize_) +
               " addRates=" + (additionalShadingRates_ ? "1" : "0") +
               " supported=" + (tier2Supported_ ? "1" : "0"));

    return tier2Supported_;
}

void VrsFoveatedD3D12::InstallDeviceHooksLocked(ID3D12Device* dev) {
    if (hooksInstalled_ || !dev) return;

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LogMessage(LogLevel::Error, std::string("VRS12: MH_Initialize failed status=") + MH_StatusToString(st));
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(dev);
    if (!vtbl) return;

    bool okRtv = false;
    bool okDsv = false;

    if (vtbl[15]) {
        MH_STATUS s = MH_CreateHook(vtbl[15], reinterpret_cast<void*>(&Hooked_CreateRenderTargetView),
                                    reinterpret_cast<void**>(&g_origCreateRtv));
        if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
            s = MH_EnableHook(vtbl[15]);
            if (s == MH_OK || s == MH_ERROR_ENABLED) {
                okRtv = true;
            }
        }
    }

    if (vtbl[17]) {
        MH_STATUS s = MH_CreateHook(vtbl[17], reinterpret_cast<void*>(&Hooked_CreateDepthStencilView),
                                    reinterpret_cast<void**>(&g_origCreateDsv));
        if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
            s = MH_EnableHook(vtbl[17]);
            if (s == MH_OK || s == MH_ERROR_ENABLED) {
                okDsv = true;
            }
        }
    }

    rtvDescriptorSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsvDescriptorSize_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    LogMessage(LogLevel::Info, std::string("VRS12: Device hooks CreateRtv=") + (okRtv ? "OK" : "FAIL") +
               " CreateDsv=" + (okDsv ? "OK" : "FAIL") +
               " rtvInc=" + std::to_string(rtvDescriptorSize_) +
               " dsvInc=" + std::to_string(dsvDescriptorSize_));
}

void VrsFoveatedD3D12::InstallCommandListHooksLocked(ID3D12GraphicsCommandList* cmdList) {
    if (hooksInstalled_ || !cmdList) return;

    void** vtbl = *reinterpret_cast<void***>(cmdList);
    if (!vtbl) return;

    bool okOmsrt = false;
    if (vtbl[46]) {
        MH_STATUS s = MH_CreateHook(vtbl[46], reinterpret_cast<void*>(&Hooked_OMSetRenderTargets),
                                    reinterpret_cast<void**>(&g_origOMSetRenderTargets));
        if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
            s = MH_EnableHook(vtbl[46]);
            if (s == MH_OK || s == MH_ERROR_ENABLED) {
                okOmsrt = true;
            }
        }
    }

    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    bool okRp = false;
    if (SUCCEEDED(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList4))) && cmdList4) {
        void** vtbl4 = *reinterpret_cast<void***>(cmdList4.Get());
        if (vtbl4 && vtbl4[62]) {
            MH_STATUS s = MH_CreateHook(vtbl4[62], reinterpret_cast<void*>(&Hooked_BeginRenderPass),
                                        reinterpret_cast<void**>(&g_origBeginRenderPass));
            if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
                s = MH_EnableHook(vtbl4[62]);
                if (s == MH_OK || s == MH_ERROR_ENABLED) {
                    okRp = true;
                }
            }
        }
    }

    hooksInstalled_ = okOmsrt;
    LogMessage(LogLevel::Info, std::string("VRS12: CommandList hooks OMSetRenderTargets=") + (okOmsrt ? "OK" : "FAIL") +
               " BeginRenderPass=" + (okRp ? "OK" : "FAIL"));
}

void VrsFoveatedD3D12::OnSessionCreated(ID3D12Device* dev, const std::string& extraFormatsCsv) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dev) return;

    device_ = dev;
    shutdownLogged_.store(false, std::memory_order_relaxed);
    deviceRemovedLogged_.store(false, std::memory_order_relaxed);
    ParseExtraFormatsCsv(extraFormatsCsv);

    if (!QueryTier2SupportLocked(dev)) {
        return;
    }

    InstallDeviceHooksLocked(dev);
}

void VrsFoveatedD3D12::RegisterSwapchainTexture(ID3D12Resource* tex) {
    if (!tex) return;
    std::unique_lock<std::shared_mutex> lock(rtvMapMutex_);
    swapchainTextures_.insert(tex);
}

void VrsFoveatedD3D12::ClearSwapchainTextures() {
    std::unique_lock<std::shared_mutex> lock(rtvMapMutex_);
    swapchainTextures_.clear();
}

void VrsFoveatedD3D12::RecordVisibilityMaskCall() {
    visMaskCallsCount_.fetch_add(1, std::memory_order_relaxed);
}

void VrsFoveatedD3D12::ReleasePatternResourcesLocked() {
    if (uploadFence_ && fenceEvent_ && uploadQueue_) {
        const uint64_t fv = ++fenceValue_;
        if (SUCCEEDED(uploadQueue_->Signal(uploadFence_, fv))) {
            if (uploadFence_->GetCompletedValue() < fv) {
                uploadFence_->SetEventOnCompletion(fv, fenceEvent_);
                WaitForSingleObject(fenceEvent_, 2000);
            }
        }
    }

    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
    if (uploadFence_) {
        uploadFence_->Release();
        uploadFence_ = nullptr;
    }
    if (uploadList_) {
        uploadList_->Release();
        uploadList_ = nullptr;
    }
    if (uploadAlloc_) {
        uploadAlloc_->Release();
        uploadAlloc_ = nullptr;
    }
    if (uploadQueue_) {
        uploadQueue_->Release();
        uploadQueue_ = nullptr;
    }

    for (auto* r : leakedResources_) {
        if (r) r->Release();
    }
    leakedResources_.clear();

    if (currentVrsTexture_) {
        currentVrsTexture_->Release();
        currentVrsTexture_ = nullptr;
    }
    if (currentVrsTextureDw_) {
        currentVrsTextureDw_->Release();
        currentVrsTextureDw_ = nullptr;
    }

    texTilesW_.store(0, std::memory_order_relaxed);
    texTilesH_.store(0, std::memory_order_relaxed);
    capPixelsW_.store(0, std::memory_order_relaxed);
    capPixelsH_.store(0, std::memory_order_relaxed);
    texDwTilesW_.store(0, std::memory_order_relaxed);
    texDwTilesH_.store(0, std::memory_order_relaxed);
    capDwPixelsW_.store(0, std::memory_order_relaxed);
    capDwPixelsH_.store(0, std::memory_order_relaxed);
    builtEyeW_ = 0;
    builtEyeH_ = 0;
    builtMode_ = -1;
}

bool VrsFoveatedD3D12::AllocateAndUploadVrsTextureLocked(uint32_t eyeW, uint32_t eyeH, int mode, bool reuseExisting) {
    if (!device_ || eyeW == 0 || eyeH == 0) return false;

    const uint32_t tile = tileSize_ ? tileSize_ : 16;
    const uint32_t tilesW = (std::max)(1u, (eyeW + tile - 1) / tile);
    const uint32_t tilesH = (std::max)(1u, (eyeH + tile - 1) / tile);
    const uint32_t capW = tilesW * tile;
    const uint32_t capH = tilesH * tile;

    // [Z1] DoubleWide pattern texture dimensions
    const uint32_t dwEyeW = eyeW * 2u;
    const uint32_t tilesDwW = (std::max)(1u, (dwEyeW + tile - 1) / tile);
    const uint32_t tilesDwH = tilesH;
    const uint32_t capDwW = tilesDwW * tile;
    const uint32_t capDwH = capH;

    if (!uploadQueue_) {
        D3D12_COMMAND_QUEUE_DESC qDesc{};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        HRESULT hr = device_->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&uploadQueue_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateCommandQueue failed HR=" + std::to_string(hr));
            return false;
        }

        hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAlloc_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateCommandAllocator failed HR=" + std::to_string(hr));
            return false;
        }

        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc_, nullptr, IID_PPV_ARGS(&uploadList_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateCommandList failed HR=" + std::to_string(hr));
            return false;
        }
        uploadList_->Close();

        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateFence failed HR=" + std::to_string(hr));
            return false;
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    if (!reuseExisting || !currentVrsTexture_ || !currentVrsTextureDw_) {
        if (currentVrsTexture_) {
            currentVrsTexture_->Release();
            currentVrsTexture_ = nullptr;
        }
        if (currentVrsTextureDw_) {
            currentVrsTextureDw_->Release();
            currentVrsTextureDw_ = nullptr;
        }

        D3D12_HEAP_PROPERTIES heapDefault{};
        heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rDesc{};
        rDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rDesc.Width = tilesW;
        rDesc.Height = tilesH;
        rDesc.DepthOrArraySize = 1;
        rDesc.MipLevels = 1;
        rDesc.Format = DXGI_FORMAT_R8_UINT;
        rDesc.SampleDesc.Count = 1;
        rDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SHADING_RATE;

        HRESULT hr = device_->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &rDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&currentVrsTexture_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateCommittedResource(SingleEye) failed HR=" + std::to_string(hr));
            return false;
        }

        // [Z1] Create committed resource for DoubleWide pattern texture
        D3D12_RESOURCE_DESC dwDesc = rDesc;
        dwDesc.Width = tilesDwW;
        dwDesc.Height = tilesDwH;
        hr = device_->CreateCommittedResource(
            &heapDefault, D3D12_HEAP_FLAG_NONE, &dwDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&currentVrsTextureDw_));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "VRS12: CreateCommittedResource(DoubleWide) failed HR=" + std::to_string(hr));
            return false;
        }
    }

    const uint32_t rowPitch = (tilesW + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * tilesH;

    const uint32_t rowPitchDw = (tilesDwW + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint64_t uploadSizeDw = static_cast<uint64_t>(rowPitchDw) * tilesDwH;

    D3D12_HEAP_PROPERTIES heapUpload{};
    heapUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bDesc{};
    bDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bDesc.Width = uploadSize + uploadSizeDw;
    bDesc.Height = 1;
    bDesc.DepthOrArraySize = 1;
    bDesc.MipLevels = 1;
    bDesc.Format = DXGI_FORMAT_UNKNOWN;
    bDesc.SampleDesc.Count = 1;
    bDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* uploadBuf = nullptr;
    HRESULT hr = device_->CreateCommittedResource(
        &heapUpload, D3D12_HEAP_FLAG_NONE, &bDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr)) {
        LogMessage(LogLevel::Error, "VRS12: CreateCommittedResource(UploadBuffer) failed HR=" + std::to_string(hr));
        return false;
    }

    std::vector<uint8_t> patternData = CreateCenteredPatternD3D12(
        tilesW, tilesH, eyeW, eyeH, false, tile, tile, additionalShadingRates_, mode);

    std::vector<uint8_t> patternDataDw = CreateCenteredPatternD3D12(
        tilesDwW, tilesDwH, dwEyeW, eyeH, true, tile, tile, additionalShadingRates_, mode);

    uint8_t* pMapped = nullptr;
    D3D12_RANGE rRange{ 0, 0 };
    hr = uploadBuf->Map(0, &rRange, reinterpret_cast<void**>(&pMapped));
    if (FAILED(hr)) {
        uploadBuf->Release();
        LogMessage(LogLevel::Error, "VRS12: UploadBuffer Map failed HR=" + std::to_string(hr));
        return false;
    }

    // Copy single-eye pattern
    for (uint32_t y = 0; y < tilesH; ++y) {
        std::memcpy(pMapped + y * rowPitch, patternData.data() + y * tilesW, tilesW);
    }

    // Copy doublewide pattern immediately after single-eye slice
    uint8_t* pMappedDw = pMapped + uploadSize;
    for (uint32_t y = 0; y < tilesDwH; ++y) {
        std::memcpy(pMappedDw + y * rowPitchDw, patternDataDw.data() + y * tilesDwW, tilesDwW);
    }
    uploadBuf->Unmap(0, nullptr);

    uploadAlloc_->Reset();
    uploadList_->Reset(uploadAlloc_, nullptr);

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = currentVrsTexture_;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = currentVrsTextureDw_;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

    uploadList_->ResourceBarrier(2, barriers);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = currentVrsTexture_;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = uploadBuf;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
    srcLoc.PlacedFootprint.Footprint.Width = tilesW;
    srcLoc.PlacedFootprint.Footprint.Height = tilesH;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    uploadList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // [Z1] Copy DoubleWide texture from upload buffer
    D3D12_TEXTURE_COPY_LOCATION dstLocDw{};
    dstLocDw.pResource = currentVrsTextureDw_;
    dstLocDw.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocDw.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLocDw{};
    srcLocDw.pResource = uploadBuf;
    srcLocDw.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocDw.PlacedFootprint.Offset = uploadSize;
    srcLocDw.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
    srcLocDw.PlacedFootprint.Footprint.Width = tilesDwW;
    srcLocDw.PlacedFootprint.Footprint.Height = tilesDwH;
    srcLocDw.PlacedFootprint.Footprint.Depth = 1;
    srcLocDw.PlacedFootprint.Footprint.RowPitch = rowPitchDw;

    uploadList_->CopyTextureRegion(&dstLocDw, 0, 0, 0, &srcLocDw, nullptr);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    uploadList_->ResourceBarrier(2, barriers);

    uploadList_->Close();

    ID3D12CommandList* ppLists[] = { uploadList_ };
    uploadQueue_->ExecuteCommandLists(1, ppLists);

    const uint64_t fv = ++fenceValue_;
    uploadQueue_->Signal(uploadFence_, fv);
    if (uploadFence_->GetCompletedValue() < fv) {
        uploadFence_->SetEventOnCompletion(fv, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 2000);
    }

    uploadBuf->Release();

    texTilesW_.store(tilesW, std::memory_order_relaxed);
    texTilesH_.store(tilesH, std::memory_order_relaxed);
    capPixelsW_.store(capW, std::memory_order_relaxed);
    capPixelsH_.store(capH, std::memory_order_relaxed);
    texDwTilesW_.store(tilesDwW, std::memory_order_relaxed);
    texDwTilesH_.store(tilesDwH, std::memory_order_relaxed);
    capDwPixelsW_.store(capDwW, std::memory_order_relaxed);
    capDwPixelsH_.store(capDwH, std::memory_order_relaxed);
    builtEyeW_ = eyeW;
    builtEyeH_ = eyeH;
    builtMode_ = mode;

    LogMessage(LogLevel::Info, "VRS12: pattern texture uploaded (Single=" +
               std::to_string(tilesW) + "x" + std::to_string(tilesH) +
               " dwTiles=" + std::to_string(tilesDwW) + "x" + std::to_string(tilesDwH) +
               " cap=" + std::to_string(capW) + "x" + std::to_string(capH) +
               " dwCap=" + std::to_string(capDwW) + "x" + std::to_string(capDwH) +
               " eye=" + std::to_string(eyeW) + "x" + std::to_string(eyeH) +
               " mode=" + std::to_string(mode) + ").");
    return true;
}

void VrsFoveatedD3D12::UpdateTarget(ID3D12Device* dev,
                                    bool enabled,
                                    uint32_t eyeWidth,
                                    uint32_t eyeHeight,
                                    int debugMode,
                                    bool debugLogging) {
    debugLogging_.store(debugLogging, std::memory_order_relaxed);
    if (deviceRemovedLogged_.load(std::memory_order_relaxed)) {
        enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!tier2Supported_ || !dev || eyeWidth == 0 || eyeHeight == 0) {
        enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    targetEyeW_.store(eyeWidth, std::memory_order_relaxed);
    targetEyeH_.store(eyeHeight, std::memory_order_relaxed);
    debugMode_.store(debugMode, std::memory_order_relaxed);

    const uint32_t curCapW = capPixelsW_.load(std::memory_order_relaxed);
    const uint32_t curCapH = capPixelsH_.load(std::memory_order_relaxed);
    const uint32_t curCapDwW = capDwPixelsW_.load(std::memory_order_relaxed);
    const bool exceedsCapacity = (!currentVrsTexture_ || !currentVrsTextureDw_ ||
                                  eyeWidth > curCapW || eyeHeight > curCapH || (eyeWidth * 2u) > curCapDwW);
    const bool needsPatternUpdate = (exceedsCapacity || builtEyeW_ != eyeWidth || builtEyeH_ != eyeHeight ||
                                     builtMode_ != debugMode || !vrs12Ready_.load(std::memory_order_relaxed));

    if (needsPatternUpdate) {
        vrs12Ready_.store(false, std::memory_order_relaxed);
        loggedDistinctRtHandles_.clear();
        distinctRtLogCount_.store(0, std::memory_order_relaxed);
        if (AllocateAndUploadVrsTextureLocked(eyeWidth, eyeHeight, debugMode, !exceedsCapacity)) {
            vrs12Ready_.store(true, std::memory_order_release);
        }
    }

    enabled_.store(enabled, std::memory_order_relaxed);
}

void VrsFoveatedD3D12::OnCreateRenderTargetView(
    ID3D12Device* /*dev*/,
    ID3D12Resource* pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    std::unique_lock<std::shared_mutex> lock(rtvMapMutex_);
    if (!pResource) {
        rtvMap_.erase(destDescriptor.ptr);
        return;
    }

    D3D12_RESOURCE_DESC rDesc = pResource->GetDesc();
    if (rDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        rtvMap_.erase(destDescriptor.ptr);
        return;
    }

    DescriptorPod pod{};
    pod.w = static_cast<uint32_t>(rDesc.Width);
    pod.h = rDesc.Height;
    pod.resourceFmt = rDesc.Format;
    pod.samples = rDesc.SampleDesc.Count;
    pod.arraySize = rDesc.DepthOrArraySize;
    pod.isSwapchain = (swapchainTextures_.find(pResource) != swapchainTextures_.end());

    if (pDesc) {
        pod.viewFmt = pDesc->Format;
        pod.dim = static_cast<uint32_t>(pDesc->ViewDimension);
    } else {
        pod.viewFmt = rDesc.Format;
        pod.dim = static_cast<uint32_t>(D3D12_RTV_DIMENSION_TEXTURE2D);
    }

    rtvMap_[destDescriptor.ptr] = pod;
}

void VrsFoveatedD3D12::OnCreateDepthStencilView(
    ID3D12Device* /*dev*/,
    ID3D12Resource* pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    std::unique_lock<std::shared_mutex> lock(dsvMapMutex_);
    if (!pResource) {
        dsvMap_.erase(destDescriptor.ptr);
        return;
    }

    D3D12_RESOURCE_DESC rDesc = pResource->GetDesc();
    if (rDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        dsvMap_.erase(destDescriptor.ptr);
        return;
    }

    DescriptorPod pod{};
    pod.w = static_cast<uint32_t>(rDesc.Width);
    pod.h = rDesc.Height;
    pod.resourceFmt = rDesc.Format;
    pod.samples = rDesc.SampleDesc.Count;
    pod.arraySize = rDesc.DepthOrArraySize;

    if (pDesc) {
        pod.viewFmt = pDesc->Format;
        pod.dim = static_cast<uint32_t>(pDesc->ViewDimension);
    } else {
        pod.viewFmt = rDesc.Format;
        pod.dim = static_cast<uint32_t>(D3D12_DSV_DIMENSION_TEXTURE2D);
    }

    dsvMap_[destDescriptor.ptr] = pod;
}

void VrsFoveatedD3D12::OnBeginRenderPass() {
    beginRpCount_.fetch_add(1, std::memory_order_relaxed);
}

void VrsFoveatedD3D12::OnPostOMSetRenderTargets(
    ID3D12GraphicsCommandList* cmdList,
    UINT numRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    omsrtCount_.fetch_add(1, std::memory_order_relaxed);

    if (!hooksInstalled_ && cmdList) {
        std::lock_guard<std::mutex> lock(mutex_);
        InstallCommandListHooksLocked(cmdList);
    }

    if (!enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    if (!vrs12Ready_.load(std::memory_order_acquire)) {
        return;
    }

    if (!cmdList) return;

    ComPtr<ID3D12GraphicsCommandList5> cmdList5;
    if (FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&cmdList5))) || !cmdList5) {
        return;
    }

    directCount_.fetch_add(1, std::memory_order_relaxed);

    const int curDebugMode = debugMode_.load(std::memory_order_relaxed);
    const uint32_t eyeW = targetEyeW_.load(std::memory_order_relaxed);
    const uint32_t eyeH = targetEyeH_.load(std::memory_order_relaxed);

    auto bindOrResetVrs = [&](bool bind, bool doubleWide) {
        if (bind && curDebugMode != 1) {
            ID3D12Resource* vrsTex = doubleWide ? currentVrsTextureDw_ : currentVrsTexture_;
            if (vrsTex) {
                cmdList5->RSSetShadingRateImage(vrsTex);
                bindsCount_.fetch_add(1, std::memory_order_relaxed);
            } else {
                cmdList5->RSSetShadingRateImage(nullptr);
                resetsCount_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            cmdList5->RSSetShadingRateImage(nullptr);
            resetsCount_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    if (numRenderTargetDescriptors == 0 || !pRenderTargetDescriptors) {
        bindOrResetVrs(false, false);
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rt0Handle = pRenderTargetDescriptors[0];

    DescriptorPod rtPod{};
    const bool isDebugLogging = debugLogging_.load(std::memory_order_relaxed);
    bool needDistinctLog = isDebugLogging && (distinctRtLogCount_.load(std::memory_order_relaxed) < kMaxDistinctRtLogs);
    std::string mrtFormatsStr;

    {
        std::shared_lock<std::shared_mutex> rLock(rtvMapMutex_);
        auto it = rtvMap_.find(rt0Handle.ptr);
        if (it == rtvMap_.end()) {
            rtvUnknownCount_.fetch_add(1, std::memory_order_relaxed);
            bindOrResetVrs(false, false);
            return;
        }
        rtPod = it->second;

        if (needDistinctLog) {
            const UINT mrtCount = (std::min)(numRenderTargetDescriptors, 8u);
            mrtFormatsStr = " mrt=" + std::to_string(numRenderTargetDescriptors) + " f=[";
            for (UINT i = 0; i < mrtCount; ++i) {
                if (i > 0) mrtFormatsStr += ",";
                D3D12_CPU_DESCRIPTOR_HANDLE h{};
                if (RTsSingleHandleToDescriptorRange) {
                    h.ptr = pRenderTargetDescriptors[0].ptr + static_cast<SIZE_T>(i) * rtvDescriptorSize_;
                } else {
                    h = pRenderTargetDescriptors[i];
                }
                auto itM = rtvMap_.find(h.ptr);
                if (itM != rtvMap_.end()) {
                    mrtFormatsStr += std::to_string(static_cast<uint32_t>(itM->second.resourceFmt));
                } else {
                    mrtFormatsStr += "?";
                }
            }
            mrtFormatsStr += "]";
        }
    }

    bool hasValidDsv = false;
    DescriptorPod dsvPod{};
    if (pDepthStencilDescriptor && pDepthStencilDescriptor->ptr != 0) {
        std::shared_lock<std::shared_mutex> dLock(dsvMapMutex_);
        auto itD = dsvMap_.find(pDepthStencilDescriptor->ptr);
        if (itD != dsvMap_.end()) {
            hasValidDsv = true;
            dsvPod = itD->second;
        }
    }

    // [Z1] Enable DOUBLEWIDE parity with D3D11; require sc=0 for DOUBLEWIDE
    const float targetAspect = (eyeH > 0) ? (static_cast<float>(eyeW) / static_cast<float>(eyeH)) : 1.0f;
    const float a2 = (rtPod.h > 0) ? ((static_cast<float>(rtPod.w) * 0.5f) / static_cast<float>(rtPod.h)) : 0.0f;
    const bool isDwAspect = (std::abs(a2 - targetAspect) <= kVrsAspectTolerance);

    VrsFilterInput fIn{};
    fIn.rtW = rtPod.w;
    fIn.rtH = rtPod.h;
    fIn.rtArraySize = rtPod.arraySize;
    fIn.rtSamples = rtPod.samples;
    fIn.rtResourceFormat = rtPod.resourceFmt;
    fIn.isExtraFormatAllowed = IsExtraFormatAllowed(rtPod.resourceFmt);
    fIn.validViewDim = (rtPod.dim == 4u || rtPod.dim == 6u);
    fIn.rtvSlices = 1;
    fIn.isExcludedSwapchain = isDwAspect ? rtPod.isSwapchain : false;
    fIn.hasDsv = hasValidDsv;
    fIn.dsvW = dsvPod.w;
    fIn.dsvH = dsvPod.h;
    fIn.dsvArraySize = dsvPod.arraySize;
    fIn.dsvSamples = dsvPod.samples;
    fIn.targetEyeW = eyeW;
    fIn.targetEyeH = eyeH;
    fIn.allowArrayAndDoubleWide = false;
    fIn.allowDoubleWideOnly = true; // [Z1]

    VrsFilterOutput fOut = EvaluateVrsCandidate(fIn);

    if (isDebugLogging && (needDistinctLog || fOut.accepted)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (loggedDistinctRtHandles_.size() < kMaxDistinctRtLogs) {
            if (loggedDistinctRtHandles_.insert(rt0Handle.ptr).second) {
                distinctRtLogCount_.store(loggedDistinctRtHandles_.size(), std::memory_order_relaxed);
                LogMessage(LogLevel::Info, "VRS12: RT " + std::to_string(rtPod.w) + "x" + std::to_string(rtPod.h) +
                           " arr=" + std::to_string(rtPod.arraySize) +
                           " fmt=" + std::to_string(static_cast<uint32_t>(rtPod.resourceFmt)) +
                           " rtvfmt=" + std::to_string(static_cast<uint32_t>(rtPod.viewFmt)) +
                           " ms=" + std::to_string(rtPod.samples) +
                           " dim=" + std::to_string(rtPod.dim) +
                           " dsv=" + (hasValidDsv ? "1" : "0") +
                           " sc=" + (rtPod.isSwapchain ? "1" : "0") +
                           mrtFormatsStr +
                           " -> " + std::string(fOut.label));
            }
        }
        if (fOut.accepted) {
            const HistKey12 hKey{
                rtPod.w, rtPod.h,
                static_cast<uint32_t>(rtPod.resourceFmt),
                static_cast<uint32_t>(rtPod.viewFmt),
                rtPod.samples,
                static_cast<uint8_t>(hasValidDsv ? 1 : 0),
                static_cast<uint8_t>(rtPod.isSwapchain ? 1 : 0)
            };
            if (histogram_.size() < kMaxHistEntries || histogram_.find(hKey) != histogram_.end()) {
                histogram_[hKey]++;
            }
        }
    }

    if (fOut.accepted) {
        candidatesCount_.fetch_add(1, std::memory_order_relaxed);
        if (fOut.doubleWide) {
            dwCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    bindOrResetVrs(fOut.accepted, fOut.doubleWide);
}

void VrsFoveatedD3D12::EndFrame() {
    if (!tier2Supported_) return;

    const uint64_t frames = framesCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frames % 900 == 0) {
        if (device_ && !deviceRemovedLogged_.load(std::memory_order_relaxed)) {
            const HRESULT hrRemoved = device_->GetDeviceRemovedReason();
            if (hrRemoved != S_OK) {
                if (!deviceRemovedLogged_.exchange(true, std::memory_order_relaxed)) {
                    enabled_.store(false, std::memory_order_relaxed);
                    char hrBuf[32]{};
                    std::snprintf(hrBuf, sizeof(hrBuf), "0x%08X", static_cast<uint32_t>(hrRemoved));
                    LogMessage(LogLevel::Error, std::string("VRS12: DEVICE_REMOVED hr=") + hrBuf);
                }
            }
        }

        if (debugLogging_.load(std::memory_order_relaxed)) {
            const uint64_t omsrt = omsrtCount_.load(std::memory_order_relaxed);
            const uint64_t direct = directCount_.load(std::memory_order_relaxed);
            const uint64_t candidates = candidatesCount_.load(std::memory_order_relaxed);
            const uint64_t dw = dwCount_.load(std::memory_order_relaxed); // [Z1]
            const uint64_t binds = bindsCount_.load(std::memory_order_relaxed);
            const uint64_t resets = resetsCount_.load(std::memory_order_relaxed);
            const uint64_t rtvUnk = rtvUnknownCount_.load(std::memory_order_relaxed);
            const uint64_t beginRp = beginRpCount_.load(std::memory_order_relaxed);
            const uint64_t visMask = visMaskCallsCount_.load(std::memory_order_relaxed);
            const int mode = debugMode_.load(std::memory_order_relaxed);
            const uint32_t tW = texTilesW_.load(std::memory_order_relaxed);
            const uint32_t tH = texTilesH_.load(std::memory_order_relaxed);
            const uint32_t cW = capPixelsW_.load(std::memory_order_relaxed);
            const uint32_t cH = capPixelsH_.load(std::memory_order_relaxed);

            std::string topStr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                std::vector<std::pair<HistKey12, uint64_t>> sorted(histogram_.begin(), histogram_.end());
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });

                if (!sorted.empty()) {
                    topStr = " top=";
                    const size_t limit = (std::min)(size_t(3), sorted.size());
                    for (size_t i = 0; i < limit; ++i) {
                        if (i > 0) topStr += " ";
                        const auto& k = sorted[i].first;
                        topStr += std::to_string(k.w) + "x" + std::to_string(k.h) +
                                  "/f" + std::to_string(k.fRt) +
                                  "/f" + std::to_string(k.fView) +
                                  "/ms" + std::to_string(k.ms) +
                                  "/dsv" + std::to_string(k.dsv) +
                                  "/sc" + std::to_string(k.sc) +
                                  ":" + std::to_string(sorted[i].second);
                    }
                }
                histogram_.clear();
            }

            LogMessage(LogLevel::Info, "VRS12: stats frames=" + std::to_string(frames) +
                       " omsrt=" + std::to_string(omsrt) +
                       " direct=" + std::to_string(direct) +
                       " candidates=" + std::to_string(candidates) +
                       " dw=" + std::to_string(dw) +
                       " binds=" + std::to_string(binds) +
                       " resets=" + std::to_string(resets) +
                       " rtv_unknown=" + std::to_string(rtvUnk) +
                       " beginRP=" + std::to_string(beginRp) +
                       " visMask=" + std::to_string(visMask) +
                       " mode=" + std::to_string(mode) +
                       " texTiles=" + std::to_string(tW) + "x" + std::to_string(tH) +
                       " cap=" + std::to_string(cW) + "x" + std::to_string(cH) +
                       topStr);
        }
    }
}

void VrsFoveatedD3D12::Shutdown(const char* reason) {
    if (!capabilityChecked_) return;
    enabled_.store(false, std::memory_order_relaxed);
    shutdownLogged_.store(true, std::memory_order_relaxed);
    LogMessage(LogLevel::Info, std::string("VRS12: Shutdown (reason=") + (reason ? reason : "DestroySession") + ")");
}

void VrsFoveatedD3D12::ShutdownOnProcessDetach() {
    if (!capabilityChecked_) return;
    if (!shutdownLogged_.exchange(true, std::memory_order_relaxed)) {
        enabled_.store(false, std::memory_order_relaxed);
        LogMessage(LogLevel::Info, "VRS12: Shutdown (reason=ProcessDetach)");
    }
}

} // namespace vrpacing

#endif // _WIN32