#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gfx/d3d12_upscaler_bridge.h"

#include <string>

#include "common/logging_win.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

struct __declspec(uuid("ADEC44E2-61F0-45C3-AD9F-1B37379284FF")) StreamlineBaseInterface : IUnknown {};

template <typename T>
T* UnwrapStreamline(T* obj) {
    if (!obj) return nullptr;
    T* baseObj = nullptr;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(StreamlineBaseInterface), reinterpret_cast<void**>(&baseObj))) && baseObj) {
        baseObj->Release();
        return baseObj;
    }
    return obj;
}

} // namespace

void D3D12UpscalerBridge::Reset() {
    upscaler_.ClearCache();
    d3d11On12_.Reset();
    d3d11Context_.Reset();
    d3d11Device_.Reset();
    realD3D12Device_ = nullptr;
    realD3D12Queue_ = nullptr;
    initialized_ = false;
}

bool D3D12UpscalerBridge::Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12Queue) {
    if (!d3d12Device || !d3d12Queue) {
        return false;
    }

    ID3D12Device* realDev = UnwrapStreamline(d3d12Device);
    ID3D12CommandQueue* realQueue = UnwrapStreamline(d3d12Queue);

    if (initialized_ && realD3D12Device_ == realDev && realD3D12Queue_ == realQueue) {
        return true;
    }

    Reset();

    HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
    if (!hD3D11) {
        hD3D11 = LoadLibraryW(L"d3d11.dll");
    }
    if (!hD3D11) {
        LogMessage(LogLevel::Error, "D3D11On12 Bridge: Failed to load d3d11.dll.");
        return false;
    }

    auto create11On12 = reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(
        GetProcAddress(hD3D11, "D3D11On12CreateDevice"));
    if (!create11On12) {
        LogMessage(LogLevel::Error, "D3D11On12 Bridge: D3D11On12CreateDevice export not found.");
        return false;
    }

    IUnknown* queues[1] = { realQueue };
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = create11On12(
        realDev,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        2,
        queues,
        1,
        0,
        d3d11Device_.ReleaseAndGetAddressOf(),
        d3d11Context_.ReleaseAndGetAddressOf(),
        nullptr);

    if (FAILED(hr) || !d3d11Device_ || !d3d11Context_) {
        LogMessage(LogLevel::Error, "D3D11On12 Bridge: D3D11On12CreateDevice failed. HR=" + std::to_string(hr));
        Reset();
        return false;
    }

    hr = d3d11Device_.As(&d3d11On12_);
    if (FAILED(hr) || !d3d11On12_) {
        LogMessage(LogLevel::Error, "D3D11On12 Bridge: QueryInterface(ID3D11On12Device) failed. HR=" + std::to_string(hr));
        Reset();
        return false;
    }

    if (!upscaler_.Initialize(d3d11Device_.Get())) {
        LogMessage(LogLevel::Error, "D3D11On12 Bridge: Failed to initialize NisUpscalerD3D11.");
        Reset();
        return false;
    }

    realD3D12Device_ = realDev;
    realD3D12Queue_ = realQueue;
    initialized_ = true;
    LogMessage(LogLevel::Info, "D3D11On12 Bridge initialized successfully (native DX12 NIS 2.0x + SMAA 1x ready).");
    return true;
}

bool D3D12UpscalerBridge::WrapResource(ID3D12Resource* d3d12Resource,
                                       D3D12_RESOURCE_STATES inState,
                                       D3D12_RESOURCE_STATES outState,
                                       ComPtr<ID3D11Texture2D>& outWrappedTex) {
    outWrappedTex.Reset();
    if (!initialized_ || !d3d11On12_ || !d3d12Resource) {
        return false;
    }

    D3D12_RESOURCE_DESC rDesc = d3d12Resource->GetDesc();
    D3D11_RESOURCE_FLAGS d3d11Flags{};
    d3d11Flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if ((rDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
        d3d11Flags.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }

    HRESULT hr = d3d11On12_->CreateWrappedResource(
        d3d12Resource,
        &d3d11Flags,
        inState,
        outState,
        IID_PPV_ARGS(outWrappedTex.GetAddressOf()));

    if (FAILED(hr) || !outWrappedTex) {
        d3d11Flags.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = d3d11On12_->CreateWrappedResource(
            d3d12Resource,
            &d3d11Flags,
            inState,
            outState,
            IID_PPV_ARGS(outWrappedTex.GetAddressOf()));
    }

    return SUCCEEDED(hr) && (outWrappedTex != nullptr);
}

bool D3D12UpscalerBridge::UpscaleWrapped(ID3D11Texture2D* srcWrapped,
                                         ID3D11Texture2D* dstWrapped,
                                         const UpscaleDispatchDesc& desc) {
    if (!initialized_ || !d3d11On12_ || !d3d11Context_ || !srcWrapped || !dstWrapped) {
        return false;
    }

    ID3D11Resource* resources[2] = { srcWrapped, dstWrapped };
    d3d11On12_->AcquireWrappedResources(resources, 2);
    const bool ok = upscaler_.Upscale(d3d11Context_.Get(), srcWrapped, dstWrapped, desc);
    d3d11On12_->ReleaseWrappedResources(resources, 2);
    d3d11Context_->Flush();
    return ok;
}

bool D3D12UpscalerBridge::CopyWrapped1to1(ID3D11Texture2D* srcWrapped,
                                          ID3D11Texture2D* dstWrapped,
                                          uint32_t numSlices) {
    if (!initialized_ || !d3d11On12_ || !d3d11Context_ || !srcWrapped || !dstWrapped) {
        return false;
    }

    ID3D11Resource* resources[2] = { srcWrapped, dstWrapped };
    d3d11On12_->AcquireWrappedResources(resources, 2);
    for (uint32_t slice = 0; slice < numSlices; ++slice) {
        const UINT sub = D3D11CalcSubresource(0, slice, 1);
        d3d11Context_->CopySubresourceRegion(dstWrapped, sub, 0, 0, 0, srcWrapped, sub, nullptr);
    }
    d3d11On12_->ReleaseWrappedResources(resources, 2);
    d3d11Context_->Flush();
    return true;
}

} // namespace vrpacing

#endif // _WIN32