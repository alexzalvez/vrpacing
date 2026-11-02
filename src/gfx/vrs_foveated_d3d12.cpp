#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gfx/vrs_foveated_d3d12.h"
#include "gfx/vrs_foveated_d3d11.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "minhook/include/MinHook.h"
#include "common/logging_win.h"
#include "common/vrs_filter.h"
#include "common/vrs_pattern.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

// Verified 0-based vtable slots from Windows SDK d3d12.h (DirectX-Headers v1.614.1, commit 9c1eb4fc)
constexpr uint32_t kSlot_Device_CreateRenderTargetView = 20;
constexpr uint32_t kSlot_Device_CreateDepthStencilView = 21;
constexpr uint32_t kSlot_CmdList_GetType               = 8;
constexpr uint32_t kSlot_CmdList_OMSetRenderTargets    = 46;
constexpr uint32_t kSlot_CmdList4_BeginRenderPass      = 68;

// Streamline unwrap IID: {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
struct __declspec(uuid("ADEC44E2-61F0-45C3-AD9F-1B37379284FF")) StreamlineRetrieveBaseInterface : IUnknown {};

template <typename T>
T* UnwrapStreamlineObject(T* obj, const char* label) {
    if (!obj) return nullptr;
    T* baseObj = nullptr;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(StreamlineRetrieveBaseInterface), reinterpret_cast<void**>(&baseObj))) && baseObj) {
        if (baseObj != obj) {
            LogMessage(LogLevel::Info, std::string("VRS12: Streamline proxy unwrapped for ") + label);
        }
        baseObj->Release(); // Keep non-owning raw pointer parity
        return baseObj;
    }
    return obj;
}

using CreateRenderTargetViewFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    ID3D12Resource*,
    const D3D12_RENDER_TARGET_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);

using CreateDepthStencilViewFn = void(STDMETHODCALLTYPE*)(
    ID3D12Device*,
    ID3D12Resource*,
    const D3D12_DEPTH_STENCIL_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);

using OMSetRenderTargets12Fn = void(STDMETHODCALLTYPE*)(
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

CreateRenderTargetViewFn g_origCreateRTV = nullptr;
CreateDepthStencilViewFn g_origCreateDSV = nullptr;
OMSetRenderTargets12Fn g_origOMSetRT12 = nullptr;
BeginRenderPassFn g_origBeginRP = nullptr;

thread_local bool g_inVrs12Hook = false;

void STDMETHODCALLTYPE Hooked_CreateRenderTargetView(
    ID3D12Device* self,
    ID3D12Resource* pResource,
    const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    if (g_origCreateRTV) {
        g_origCreateRTV(self, pResource, pDesc, destDescriptor);
    }
    if (g_vrsDetaching.load(std::memory_order_relaxed)) return;
    VrsFoveatedD3D12::Instance().OnCreateRenderTargetView(self, pResource, pDesc, destDescriptor);
}

void STDMETHODCALLTYPE Hooked_CreateDepthStencilView(
    ID3D12Device* self,
    ID3D12Resource* pResource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    if (g_origCreateDSV) {
        g_origCreateDSV(self, pResource, pDesc, destDescriptor);
    }
    if (g_vrsDetaching.load(std::memory_order_relaxed)) return;
    VrsFoveatedD3D12::Instance().OnCreateDepthStencilView(self, pResource, pDesc, destDescriptor);
}

void STDMETHODCALLTYPE Hooked_OMSetRenderTargets12(
    ID3D12GraphicsCommandList* self,
    UINT numRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (g_origOMSetRT12) {
        g_origOMSetRT12(self, numRenderTargetDescriptors, pRenderTargetDescriptors,
                        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    }
    if (g_vrsDetaching.load(std::memory_order_relaxed) || g_inVrs12Hook) return;
    g_inVrs12Hook = true;
    VrsFoveatedD3D12::Instance().OnPostOMSetRenderTargets(
        self, numRenderTargetDescriptors, pRenderTargetDescriptors,
        RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
    g_inVrs12Hook = false;
}

void STDMETHODCALLTYPE Hooked_BeginRenderPass(
    ID3D12GraphicsCommandList4* self,
    UINT numRenderTargets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pRenderTargets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepthStencil,
    D3D12_RENDER_PASS_FLAGS flags) {
    if (g_origBeginRP) {
        g_origBeginRP(self, numRenderTargets, pRenderTargets, pDepthStencil, flags);
    }
    if (g_vrsDetaching.load(std::memory_order_relaxed)) return;
    VrsFoveatedD3D12::Instance().OnBeginRenderPass();
}

uint32_t MapRtvDimension(D3D12_RTV_DIMENSION dim) {
    switch (dim) {
        case D3D12_RTV_DIMENSION_TEXTURE2D:        return 4;
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:   return 5;
        case D3D12_RTV_DIMENSION_TEXTURE2DMS:      return 6;
        case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: return 7;
        default:                                   return static_cast<uint32_t>(dim);
    }
}

uint64_t HashRt12(uint32_t w, uint32_t h, uint32_t arr, DXGI_FORMAT rFmt, DXGI_FORMAT vFmt,
                  uint32_t ms, uint32_t dim, bool dsv, bool sc, const char* label) {
    uint64_t hash = 14695981039346656037ULL;
    auto mix = [&hash](uint64_t v) { hash ^= v; hash *= 1099511628211ULL; };
    mix(w); mix(h); mix(arr); mix(static_cast<uint32_t>(rFmt)); mix(static_cast<uint32_t>(vFmt));
    mix(ms); mix(dim); mix(dsv ? 1u : 0u); mix(sc ? 1u : 0u);
    if (label) {
        for (const char* p = label; *p != '\0'; ++p) mix(static_cast<uint8_t>(*p));
    }
    return hash;
}

} // namespace

VrsFoveatedD3D12& VrsFoveatedD3D12::Instance() {
    static VrsFoveatedD3D12* s_instance = new VrsFoveatedD3D12();
    return *s_instance;
}

void VrsFoveatedD3D12::RecordVisibilityMaskCall() {
    visMaskCallsCount_.fetch_add(1, std::memory_order_relaxed);
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

// [B1] Anchor: check vendor id, D3D12_OPTIONS6, and install hooks if Tier >= 2
void VrsFoveatedD3D12::OnSessionCreated(ID3D12Device* dev) {
    if (!dev) return;
    std::lock_guard<std::mutex> lock(mutex_);

    ID3D12Device* realDev = UnwrapStreamlineObject(dev, "ID3D12Device");
    device_ = realDev;

    if (!capabilityChecked_) {
        capabilityChecked_ = true;

        uint32_t vendorId = 0;
        LUID luid = realDev->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) && factory) {
            ComPtr<IDXGIAdapter1> adapter;
            if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.GetAddressOf()))) && adapter) {
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                    vendorId = desc.VendorId;
                }
            }
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS6 opts6{};
        HRESULT hr = realDev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &opts6, sizeof(opts6));
        if (FAILED(hr)) {
            LogMessage(LogLevel::Warn, "VRS12: CheckFeatureSupport(D3D12_OPTIONS6) failed.");
            tier2Supported_ = false;
            return;
        }

        char hexVendor[16]{};
        std::snprintf(hexVendor, sizeof(hexVendor), "0x%04X", vendorId);
        LogMessage(LogLevel::Info, "VRS12: Adapter vendor=" + std::string(hexVendor) +
                   " VariableShadingRateTier=" + std::to_string(static_cast<int>(opts6.VariableShadingRateTier)) +
                   " ShadingRateImageTileSize=" + std::to_string(opts6.ShadingRateImageTileSize) +
                   " AdditionalShadingRatesSupported=" + std::to_string(opts6.AdditionalShadingRatesSupported ? 1 : 0));

        if (opts6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2 || opts6.ShadingRateImageTileSize < 2u) {
            LogMessage(LogLevel::Info, "VRS12: unsupported");
            tier2Supported_ = false;
            return;
        }

        tier2Supported_ = true;
        tileSize_ = opts6.ShadingRateImageTileSize;
        additionalShadingRates_ = (opts6.AdditionalShadingRatesSupported != FALSE);
    }

    if (tier2Supported_ && !hooksInstalled_) {
        InstallHooksLocked(realDev);
    }
}

// [B2] Install MinHook on Device and dummy DIRECT command list vtable targets
void VrsFoveatedD3D12::InstallHooksLocked(ID3D12Device* rawDevice) {
    if (hooksInstalled_ || !rawDevice) return;

    MH_Initialize();

    void** devVtbl = *reinterpret_cast<void***>(rawDevice);
    if (!devVtbl) return;

    // Create ONE dummy DIRECT command allocator + command list to read vtable
    ComPtr<ID3D12CommandAllocator> dummyAlloc;
    if (FAILED(rawDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(dummyAlloc.GetAddressOf())))) {
        LogMessage(LogLevel::Error, "VRS12: Failed to create dummy command allocator for vtable inspection.");
        return;
    }

    ComPtr<ID3D12GraphicsCommandList> dummyList;
    if (FAILED(rawDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dummyAlloc.Get(), nullptr, IID_PPV_ARGS(dummyList.GetAddressOf())))) {
        LogMessage(LogLevel::Error, "VRS12: Failed to create dummy command list for vtable inspection.");
        return;
    }

    ID3D12GraphicsCommandList* realList = UnwrapStreamlineObject(dummyList.Get(), "ID3D12GraphicsCommandList");
    void** listVtbl = *reinterpret_cast<void***>(realList);

    ComPtr<ID3D12GraphicsCommandList4> dummyList4;
    void** list4Vtbl = nullptr;
    if (SUCCEEDED(realList->QueryInterface(IID_PPV_ARGS(dummyList4.GetAddressOf()))) && dummyList4) {
        ID3D12GraphicsCommandList4* realList4 = UnwrapStreamlineObject(dummyList4.Get(), "ID3D12GraphicsCommandList4");
        list4Vtbl = *reinterpret_cast<void***>(realList4);
    }

    (void)dummyList->Close();

    bool okRtv = false, okDsv = false, okOmsrt = false, okBrp = false;

    if (devVtbl[kSlot_Device_CreateRenderTargetView]) {
        if (MH_CreateHook(devVtbl[kSlot_Device_CreateRenderTargetView],
                          reinterpret_cast<void*>(&Hooked_CreateRenderTargetView),
                          reinterpret_cast<void**>(&g_origCreateRTV)) == MH_OK) {
            okRtv = (MH_EnableHook(devVtbl[kSlot_Device_CreateRenderTargetView]) == MH_OK);
        }
    }

    if (devVtbl[kSlot_Device_CreateDepthStencilView]) {
        if (MH_CreateHook(devVtbl[kSlot_Device_CreateDepthStencilView],
                          reinterpret_cast<void*>(&Hooked_CreateDepthStencilView),
                          reinterpret_cast<void**>(&g_origCreateDSV)) == MH_OK) {
            okDsv = (MH_EnableHook(devVtbl[kSlot_Device_CreateDepthStencilView]) == MH_OK);
        }
    }

    if (listVtbl && listVtbl[kSlot_CmdList_OMSetRenderTargets]) {
        if (MH_CreateHook(listVtbl[kSlot_CmdList_OMSetRenderTargets],
                          reinterpret_cast<void*>(&Hooked_OMSetRenderTargets12),
                          reinterpret_cast<void**>(&g_origOMSetRT12)) == MH_OK) {
            okOmsrt = (MH_EnableHook(listVtbl[kSlot_CmdList_OMSetRenderTargets]) == MH_OK);
        }
    }

    if (list4Vtbl && list4Vtbl[kSlot_CmdList4_BeginRenderPass]) {
        if (MH_CreateHook(list4Vtbl[kSlot_CmdList4_BeginRenderPass],
                          reinterpret_cast<void*>(&Hooked_BeginRenderPass),
                          reinterpret_cast<void**>(&g_origBeginRP)) == MH_OK) {
            okBrp = (MH_EnableHook(list4Vtbl[kSlot_CmdList4_BeginRenderPass]) == MH_OK);
        }
    }

    hooksInstalled_ = okRtv && okDsv && okOmsrt;
    LogMessage(LogLevel::Info, std::string("VRS12: hooks CreateRTV=") + (okRtv ? "OK" : "FAIL") +
               " CreateDSV=" + (okDsv ? "OK" : "FAIL") +
               " OMSetRT=" + (okOmsrt ? "OK" : "FAIL") +
               " BeginRP=" + (okBrp ? "OK" : "FAIL"));
}

// [B6] Build and upload R8_UINT VRS texture on our own direct queue + fence
bool VrsFoveatedD3D12::BuildAndUploadVrsTextureLocked(uint32_t eyeW, uint32_t eyeH, int mode) {
    if (!device_ || eyeW == 0 || eyeH == 0 || tileSize_ == 0) return false;

    if (!uploadQueue_) {
        D3D12_COMMAND_QUEUE_DESC qDesc{};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        qDesc.NodeMask = 0;
        if (FAILED(device_->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&uploadQueue_)))) {
            LogMessage(LogLevel::Error, "VRS12: Failed to create internal upload CommandQueue.");
            return false;
        }
    }
    if (!uploadAlloc_) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAlloc_)))) {
            return false;
        }
    }
    if (!uploadList_) {
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc_, nullptr, IID_PPV_ARGS(&uploadList_)))) {
            return false;
        }
        (void)uploadList_->Close();
    }
    if (!uploadFence_) {
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploadFence_)))) {
            return false;
        }
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    const uint32_t tilesW = (std::max)(1u, (eyeW + tileSize_ - 1) / tileSize_);
    const uint32_t tilesH = (std::max)(1u, (eyeH + tileSize_ - 1) / tileSize_);

    std::vector<uint8_t> pattern = CreateCenteredPatternD3D12(
        static_cast<int>(eyeW), static_cast<int>(eyeH),
        static_cast<int>(tileSize_), mode, additionalShadingRates_);
    if (pattern.size() < static_cast<size_t>(tilesW) * static_cast<size_t>(tilesH)) {
        return false;
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask = 1;
    defaultHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = tilesW;
    texDesc.Height = tilesH;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* newTex = nullptr;
    HRESULT hr = device_->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&newTex));
    if (FAILED(hr) || !newTex) {
        LogMessage(LogLevel::Error, "VRS12: Failed to create R8_UINT VRS texture. HR=" + std::to_string(hr));
        return false;
    }

    const uint32_t rowPitch = (tilesW + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const uint64_t uploadSize = static_cast<uint64_t>(rowPitch) * static_cast<uint64_t>(tilesH);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 1;
    uploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Alignment = 0;
    bufDesc.Width = uploadSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.SampleDesc.Quality = 0;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* uploadBuf = nullptr;
    hr = device_->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));
    if (FAILED(hr) || !uploadBuf) {
        leakedResources_.push_back(newTex);
        return false;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    if (SUCCEEDED(uploadBuf->Map(0, &readRange, reinterpret_cast<void**>(&mapped))) && mapped) {
        for (uint32_t y = 0; y < tilesH; ++y) {
            std::memcpy(mapped + static_cast<size_t>(y) * rowPitch,
                        pattern.data() + static_cast<size_t>(y) * tilesW,
                        tilesW);
        }
        uploadBuf->Unmap(0, nullptr);
    }

    (void)uploadAlloc_->Reset();
    (void)uploadList_->Reset(uploadAlloc_, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = newTex;
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

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = newTex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    uploadList_->ResourceBarrier(1, &barrier);

    (void)uploadList_->Close();

    ID3D12CommandList* lists[1] = { uploadList_ };
    uploadQueue_->ExecuteCommandLists(1, lists);

    ++fenceValue_;
    (void)uploadQueue_->Signal(uploadFence_, fenceValue_);
    if (uploadFence_->GetCompletedValue() < fenceValue_) {
        (void)uploadFence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, 5000);
    }

    leakedResources_.push_back(uploadBuf);
    if (currentVrsTexture_) {
        leakedResources_.push_back(currentVrsTexture_);
        LogMessage(LogLevel::Info, "VRS12: Rebuilt VRS texture (old texture intentionally retained for process lifetime).");
    }
    currentVrsTexture_ = newTex;
    LogMessage(LogLevel::Info, "VRS12: Pattern texture ready (" + std::to_string(tilesW) + "x" +
               std::to_string(tilesH) + " tiles for " + std::to_string(eyeW) + "x" +
               std::to_string(eyeH) + ", mode=" + std::to_string(mode) + ").");
    return true;
}

void VrsFoveatedD3D12::UpdateTarget(ID3D12Device* dev,
                                    bool enabled,
                                    uint32_t eyeWidth,
                                    uint32_t eyeHeight,
                                    int debugMode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tier2Supported_ || !dev || eyeWidth == 0 || eyeHeight == 0) {
        enabled_.store(false, std::memory_order_relaxed);
        return;
    }

    debugMode_.store(debugMode, std::memory_order_relaxed);

    if (targetEyeW_ != eyeWidth || targetEyeH_ != eyeHeight || builtMode_ != debugMode || !currentVrsTexture_) {
        vrs12Ready_.store(false, std::memory_order_relaxed);
        targetEyeW_ = eyeWidth;
        targetEyeH_ = eyeHeight;
        builtMode_ = debugMode;
        loggedDistinctRts_.clear();
        if (BuildAndUploadVrsTextureLocked(eyeWidth, eyeHeight, debugMode)) {
            vrs12Ready_.store(true, std::memory_order_release);
        }
    }

    enabled_.store(enabled, std::memory_order_relaxed);
}

// [B3] Record RTV descriptor metadata at creation time
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
        pod.viewFmt = (pDesc->Format != DXGI_FORMAT_UNKNOWN) ? pDesc->Format : rDesc.Format;
        pod.dim = MapRtvDimension(pDesc->ViewDimension);
    } else {
        pod.viewFmt = rDesc.Format;
        if (rDesc.SampleDesc.Count > 1) {
            pod.dim = (rDesc.DepthOrArraySize > 1) ? 7u : 6u;
        } else {
            pod.dim = (rDesc.DepthOrArraySize > 1) ? 5u : 4u;
        }
    }

    rtvMap_[destDescriptor.ptr] = pod;
}

// [B3] Record DSV descriptor metadata at creation time
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
    pod.viewFmt = (pDesc && pDesc->Format != DXGI_FORMAT_UNKNOWN) ? pDesc->Format : rDesc.Format;
    pod.samples = rDesc.SampleDesc.Count;
    pod.arraySize = rDesc.DepthOrArraySize;
    dsvMap_[destDescriptor.ptr] = pod;
}

void VrsFoveatedD3D12::OnBeginRenderPass() {
    beginRpCount_.fetch_add(1, std::memory_order_relaxed);
}

void VrsFoveatedD3D12::LogDistinctRtLocked(const DescriptorPod& rt, bool hasDsv, const char* label) {
    if (loggedDistinctRts_.size() >= kMaxDistinctRtLogs) return;
    const uint64_t key = HashRt12(rt.w, rt.h, rt.arraySize, rt.resourceFmt, rt.viewFmt,
                                  rt.samples, rt.dim, hasDsv, rt.isSwapchain, label);
    if (loggedDistinctRts_.insert(key).second) {
        LogMessage(LogLevel::Info, "VRS12: RT " + std::to_string(rt.w) + "x" + std::to_string(rt.h) +
                   " arr=" + std::to_string(rt.arraySize) +
                   " fmt=" + std::to_string(static_cast<uint32_t>(rt.resourceFmt)) +
                   " rtvfmt=" + std::to_string(static_cast<uint32_t>(rt.viewFmt)) +
                   " ms=" + std::to_string(rt.samples) +
                   " dim=" + std::to_string(rt.dim) +
                   " dsv=" + (hasDsv ? "1" : "0") +
                   " sc=" + (rt.isSwapchain ? "1" : "0") +
                   " -> " + std::string(label));
    }
}

// [B3, B4, B7] Core OMSetRenderTargets logic for D3D12
void VrsFoveatedD3D12::OnPostOMSetRenderTargets(
    ID3D12GraphicsCommandList* cmdList,
    UINT numRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
    BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor) {
    if (!cmdList || !tier2Supported_) return;

    omsrtCount_.fetch_add(1, std::memory_order_relaxed);

    if (cmdList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }
    directCount_.fetch_add(1, std::memory_order_relaxed);

    // [B7] QueryInterface ID3D12GraphicsCommandList5 cached per vtable
    void** vtbl = *reinterpret_cast<void***>(cmdList);
    bool useDirectCast = false;
    bool supported5 = false;
    {
        std::shared_lock<std::shared_mutex> rLock(vtblCacheMutex_);
        auto it = cmdList5VtblCache_.find(vtbl);
        if (it != cmdList5VtblCache_.end() && it->second.checked) {
            supported5 = it->second.supported;
            useDirectCast = it->second.pointerEqualsThis;
        }
    }

    if (!supported5) {
        std::unique_lock<std::shared_mutex> wLock(vtblCacheMutex_);
        auto& entry = cmdList5VtblCache_[vtbl];
        if (!entry.checked) {
            entry.checked = true;
            ComPtr<ID3D12GraphicsCommandList5> tmp5;
            if (SUCCEEDED(cmdList->QueryInterface(IID_PPV_ARGS(tmp5.GetAddressOf()))) && tmp5) {
                entry.supported = true;
                entry.pointerEqualsThis = (reinterpret_cast<void*>(tmp5.Get()) == reinterpret_cast<void*>(cmdList));
                LogMessage(LogLevel::Info, std::string("VRS12: QI ID3D12GraphicsCommandList5 supported=1 pointerEqualsThis=") +
                           (entry.pointerEqualsThis ? "1" : "0"));
            } else {
                entry.supported = false;
                LogMessage(LogLevel::Warn, "VRS12: QI ID3D12GraphicsCommandList5 supported=0");
            }
        }
        supported5 = entry.supported;
        useDirectCast = entry.pointerEqualsThis;
    }

    if (!supported5) return;

    const int mode = debugMode_.load(std::memory_order_relaxed);
    const bool isEnabled = enabled_.load(std::memory_order_relaxed);
    const bool isReady = vrs12Ready_.load(std::memory_order_acquire);

    auto applyOrReset = [&](bool isCandidate) {
        if (mode == 1) {
            return; // [B8] Mode 1 = passive: never call RS*
        }

        ComPtr<ID3D12GraphicsCommandList5> qiList5;
        ID3D12GraphicsCommandList5* list5 = nullptr;
        if (useDirectCast) {
            list5 = reinterpret_cast<ID3D12GraphicsCommandList5*>(cmdList);
        } else {
            if (SUCCEEDED(cmdList->QueryInterface(IID_PPV_ARGS(qiList5.GetAddressOf())))) {
                list5 = qiList5.Get();
            }
        }
        if (!list5) return;

        if (isCandidate && isEnabled && isReady && currentVrsTexture_) {
            static const D3D12_SHADING_RATE_COMBINER kCombiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_OVERRIDE
            };
            list5->RSSetShadingRateImage(currentVrsTexture_);
            list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, kCombiners);
            bindsCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        }
    };

    if (numRenderTargetDescriptors == 0 || !pRenderTargetDescriptors) {
        applyOrReset(false);
        return;
    }

    // [B3] Whether RTsSingleHandleToDescriptorRange is TRUE (contiguous range starting at [0])
    // or FALSE (array of individual handles where [0] is RT0), RT0 is always pRenderTargetDescriptors[0].
    const D3D12_CPU_DESCRIPTOR_HANDLE rt0Handle =
        RTsSingleHandleToDescriptorRange ? pRenderTargetDescriptors[0] : pRenderTargetDescriptors[0];

    DescriptorPod rtPod{};
    {
        std::shared_lock<std::shared_mutex> rLock(rtvMapMutex_);
        auto it = rtvMap_.find(rt0Handle.ptr);
        if (it == rtvMap_.end()) {
            rtvUnknownCount_.fetch_add(1, std::memory_order_relaxed);
            applyOrReset(false);
            return;
        }
        rtPod = it->second;
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

    // [B4] Evaluate candidate with shared pure filter: dim==TEXTURE2D(4)/TEXTURE2DMS(6), arraySize==1
    VrsFilterInput fIn{};
    fIn.rtW = rtPod.w;
    fIn.rtH = rtPod.h;
    fIn.rtArraySize = rtPod.arraySize;
    fIn.rtSamples = rtPod.samples;
    fIn.rtResourceFormat = rtPod.resourceFmt;
    fIn.validViewDim = (rtPod.dim == 4u || rtPod.dim == 6u);
    fIn.rtvSlices = 1;
    fIn.isExcludedSwapchain = false; // [B4] Do NOT reject on swapchain in D3D12
    fIn.hasDsv = hasValidDsv;
    fIn.dsvW = dsvPod.w;
    fIn.dsvH = dsvPod.h;
    fIn.dsvArraySize = dsvPod.arraySize;
    fIn.dsvSamples = dsvPod.samples;
    fIn.targetEyeW = targetEyeW_;
    fIn.targetEyeH = targetEyeH_;
    fIn.allowArrayAndDoubleWide = false;

    VrsFilterOutput fOut = EvaluateVrsCandidate(fIn);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        LogDistinctRtLocked(rtPod, hasValidDsv, fOut.label);
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
    }

    applyOrReset(fOut.accepted);
}

// [B9] Telemetry every 900 frames
void VrsFoveatedD3D12::EndFrame() {
    if (!tier2Supported_) return;

    const uint64_t frames = framesCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frames % 900 == 0) {
        const uint64_t omsrt = omsrtCount_.load(std::memory_order_relaxed);
        const uint64_t direct = directCount_.load(std::memory_order_relaxed);
        const uint64_t candidates = candidatesCount_.load(std::memory_order_relaxed);
        const uint64_t binds = bindsCount_.load(std::memory_order_relaxed);
        const uint64_t rtvUnk = rtvUnknownCount_.load(std::memory_order_relaxed);
        const uint64_t beginRp = beginRpCount_.load(std::memory_order_relaxed);
        const uint64_t visMask = visMaskCallsCount_.load(std::memory_order_relaxed);
        const int mode = debugMode_.load(std::memory_order_relaxed);

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
                   " binds=" + std::to_string(binds) +
                   " rtv_unknown=" + std::to_string(rtvUnk) +
                   " beginRP=" + std::to_string(beginRp) +
                   " visMask=" + std::to_string(visMask) +
                   " mode=" + std::to_string(mode) +
                   topStr);
    }
}

// [B10] Shutdown without releasing D3D12 resources
void VrsFoveatedD3D12::Shutdown(const char* reason) {
    if (!capabilityChecked_) return;
    enabled_.store(false, std::memory_order_relaxed);
    LogMessage(LogLevel::Info, std::string("VRS12: Shutdown (reason=") + (reason ? reason : "DestroySession") + ")");
}

} // namespace vrpacing

#endif // _WIN32