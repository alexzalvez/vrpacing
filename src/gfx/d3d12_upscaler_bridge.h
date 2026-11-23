#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <wrl/client.h>

#include <cstdint>

#include "gfx/nis_upscaler_d3d11.h"

namespace vrpacing {

class D3D12UpscalerBridge {
public:
    D3D12UpscalerBridge() = default;
    ~D3D12UpscalerBridge() { Reset(); }

    bool Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12Queue);
    bool IsInitialized() const { return initialized_; }
    void Reset();

    ID3D12Device* GetRealD3D12Device() const { return realD3D12Device_; }
    ID3D12CommandQueue* GetRealD3D12Queue() const { return realD3D12Queue_; }

    bool WrapResource(ID3D12Resource* d3d12Resource,
                      D3D12_RESOURCE_STATES inState,
                      D3D12_RESOURCE_STATES outState,
                      Microsoft::WRL::ComPtr<ID3D11Texture2D>& outWrappedTex);

    bool UpscaleWrapped(ID3D11Texture2D* srcWrapped,
                        ID3D11Texture2D* dstWrapped,
                        const UpscaleDispatchDesc& desc);

    bool CopyWrapped1to1(ID3D11Texture2D* srcWrapped,
                         ID3D11Texture2D* dstWrapped,
                         uint32_t numSlices);

private:
    bool initialized_ = false;
    ID3D12Device* realD3D12Device_ = nullptr;
    ID3D12CommandQueue* realD3D12Queue_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<ID3D11On12Device> d3d11On12_;
    NisUpscalerD3D11 upscaler_;
};

} // namespace vrpacing

#endif // _WIN32