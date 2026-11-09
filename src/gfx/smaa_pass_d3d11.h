#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace vrpacing {

using Microsoft::WRL::ComPtr;

struct SmaaConstants {
    float rtMetrics[4];        // xy = (1/w, 1/h), zw = (w, h)
    float threshold;
    int   maxSearchSteps;
    float fovealRadius;
    float _pad0;
    float projectionCenter[2];
    uint32_t viewportOffset[2];
};

class SmaaPassD3D11 {
public:
    SmaaPassD3D11() = default;
    ~SmaaPassD3D11() = default;

    bool Initialize(ID3D11Device* device);
    void Reset();

    ID3D11ShaderResourceView* Apply(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* inputSrv,
        uint32_t texWidth,
        uint32_t texHeight,
        uint32_t viewX,
        uint32_t viewY,
        uint32_t viewWidth,
        uint32_t viewHeight,
        DXGI_FORMAT format,
        float projCenterX,
        float projCenterY,
        float fovealRadius);

    ID3D11Texture2D* GetOutputTexture() const { return outputTex_.Get(); }

private:
    bool EnsureTextures(uint32_t texWidth, uint32_t texHeight, uint32_t viewWidth, uint32_t viewHeight, DXGI_FORMAT format);

    bool initialized_ = false;
    ComPtr<ID3D11Device> device_;

    ComPtr<ID3D11ComputeShader> shaderEdge_;
    ComPtr<ID3D11ComputeShader> shaderBlend_;
    ComPtr<ID3D11ComputeShader> shaderNeighbor_;
    ComPtr<ID3D11Buffer> constantBuffer_;

    ComPtr<ID3D11SamplerState> linearSampler_;
    ComPtr<ID3D11SamplerState> pointSampler_;

    ComPtr<ID3D11Texture2D> areaTex_;
    ComPtr<ID3D11ShaderResourceView> areaTexSrv_;
    ComPtr<ID3D11Texture2D> searchTex_;
    ComPtr<ID3D11ShaderResourceView> searchTexSrv_;

    uint32_t cachedTexW_ = 0;
    uint32_t cachedTexH_ = 0;
    uint32_t cachedViewW_ = 0;
    uint32_t cachedViewH_ = 0;
    DXGI_FORMAT cachedOutFmt_ = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D> edgesTex_;
    ComPtr<ID3D11UnorderedAccessView> edgesUav_;
    ComPtr<ID3D11ShaderResourceView> edgesSrv_;

    ComPtr<ID3D11Texture2D> blendTex_;
    ComPtr<ID3D11UnorderedAccessView> blendUav_;
    ComPtr<ID3D11ShaderResourceView> blendSrv_;

    ComPtr<ID3D11Texture2D> outputTex_;
    ComPtr<ID3D11UnorderedAccessView> outputUav_;
    ComPtr<ID3D11ShaderResourceView> outputSrv_;
};

} // namespace vrpacing

#endif