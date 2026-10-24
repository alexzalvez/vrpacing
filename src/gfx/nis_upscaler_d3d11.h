#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <map>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace vrpacing {

struct ViewportRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct UpscaleDispatchDesc {
    ViewportRect srcRect{};
    ViewportRect dstRect{};
    float projCenterX = 0.5f;
    float projCenterY = 0.5f;
    float radius = 0.60f;
    float sharpness = 0.0f;
    bool maskEnabled = true;
    uint64_t maxOutputPixels = 32000000ull;
    int srcArrayIndex = -1;
    int dstArrayIndex = -1;
};

class NisUpscalerD3D11 {
public:
    NisUpscalerD3D11() = default;
    ~NisUpscalerD3D11() = default;

    bool Initialize(ID3D11Device* device);
    bool IsInitialized() const { return initialized_; }

    bool Upscale(ID3D11DeviceContext* ctx,
                 ID3D11ShaderResourceView* srcSrv,
                 ID3D11UnorderedAccessView* dstUav,
                 uint32_t srcWidth,
                 uint32_t srcHeight,
                 uint32_t dstWidth,
                 uint32_t dstHeight,
                 const UpscaleDispatchDesc& desc);

    bool Upscale(ID3D11DeviceContext* ctx,
                 ID3D11Texture2D* src,
                 ID3D11Texture2D* dst,
                 const UpscaleDispatchDesc& desc);

    bool UpscaleStereo(ID3D11DeviceContext* ctx,
                       ID3D11Texture2D* src,
                       ID3D11Texture2D* dst,
                       const UpscaleDispatchDesc& desc) {
        return Upscale(ctx, src, dst, desc);
    }

private:
    ComPtr<ID3D11ShaderResourceView> GetOrCreateSrv(ID3D11Texture2D* tex, int arrayIndex);
    ComPtr<ID3D11UnorderedAccessView> GetOrCreateUav(ID3D11Texture2D* tex, int arrayIndex);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11ComputeShader> upscaleShader_;
    ComPtr<ID3D11Buffer> constantsBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> scalerCoeffTexture_;
    ComPtr<ID3D11ShaderResourceView> scalerCoeffView_;
    ComPtr<ID3D11Texture2D> usmCoeffTexture_;
    ComPtr<ID3D11ShaderResourceView> usmCoeffView_;

    std::map<std::pair<ID3D11Texture2D*, int>, ComPtr<ID3D11ShaderResourceView>> srvCache_;
    std::map<std::pair<ID3D11Texture2D*, int>, ComPtr<ID3D11UnorderedAccessView>> uavCache_;

    bool initialized_ = false;
};

} // namespace vrpacing

#endif