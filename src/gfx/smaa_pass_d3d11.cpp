#ifdef _WIN32

#include "smaa_pass_d3d11.h"

#include <cstring>
#include <string>

#include "common/logging_win.h"
#include "shader_smaa_edge.h"
#include "shader_smaa_blend.h"
#include "shader_smaa_neighbor.h"
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

namespace vrpacing {

void SmaaPassD3D11::Reset() {
    initialized_ = false;
    device_.Reset();
    shaderEdge_.Reset();
    shaderBlend_.Reset();
    shaderNeighbor_.Reset();
    constantBuffer_.Reset();
    linearSampler_.Reset();
    pointSampler_.Reset();
    areaTex_.Reset();
    areaTexSrv_.Reset();
    searchTex_.Reset();
    searchTexSrv_.Reset();
    edgesTex_.Reset();
    edgesUav_.Reset();
    edgesSrv_.Reset();
    blendTex_.Reset();
    blendUav_.Reset();
    blendSrv_.Reset();
    outputTex_.Reset();
    outputUav_.Reset();
    outputSrv_.Reset();
    cachedTexW_ = 0;
    cachedTexH_ = 0;
    cachedViewW_ = 0;
    cachedViewH_ = 0;
    cachedOutFmt_ = DXGI_FORMAT_UNKNOWN;
}

bool SmaaPassD3D11::Initialize(ID3D11Device* device) {
    if (!device) return false;
    if (initialized_ && device_.Get() == device) return true;

    Reset();
    device_ = device;

    if (FAILED(device_->CreateComputeShader(g_SmaaEdgeShader, sizeof(g_SmaaEdgeShader), nullptr, shaderEdge_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateComputeShader(g_SmaaBlendShader, sizeof(g_SmaaBlendShader), nullptr, shaderBlend_.ReleaseAndGetAddressOf())) ||
        FAILED(device_->CreateComputeShader(g_SmaaNeighborShader, sizeof(g_SmaaNeighborShader), nullptr, shaderNeighbor_.ReleaseAndGetAddressOf()))) {
        LogMessage(LogLevel::Error, "SmaaPassD3D11: Failed to create SMAA compute shaders.");
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (sizeof(SmaaConstants) + 15) & ~15;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, constantBuffer_.ReleaseAndGetAddressOf()))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, linearSampler_.ReleaseAndGetAddressOf()))) {
        return false;
    }

    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device_->CreateSamplerState(&sd, pointSampler_.ReleaseAndGetAddressOf()))) {
        return false;
    }

    // AreaTex (160x560 RG8_UNORM)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = AREATEX_WIDTH;
        td.Height = AREATEX_HEIGHT;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = areaTexBytes;
        initData.SysMemPitch = AREATEX_WIDTH * 2;
        if (FAILED(device_->CreateTexture2D(&td, &initData, areaTex_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(areaTex_.Get(), nullptr, areaTexSrv_.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    // SearchTex (66x33 R8_UNORM)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = SEARCHTEX_WIDTH;
        td.Height = SEARCHTEX_HEIGHT;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData{};
        initData.pSysMem = searchTexBytes;
        initData.SysMemPitch = SEARCHTEX_WIDTH;
        if (FAILED(device_->CreateTexture2D(&td, &initData, searchTex_.ReleaseAndGetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(searchTex_.Get(), nullptr, searchTexSrv_.ReleaseAndGetAddressOf()))) {
            return false;
        }
    }

    initialized_ = true;
    LogMessage(LogLevel::Info, "SmaaPassD3D11 initialized successfully (3-pass compute pipeline ready).");
    return true;
}

bool SmaaPassD3D11::EnsureTextures(uint32_t texWidth, uint32_t texHeight, uint32_t viewWidth, uint32_t viewHeight, DXGI_FORMAT format) {
    DXGI_FORMAT outFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
        outFmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else if (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
        outFmt = DXGI_FORMAT_R10G10B10A2_UNORM;
    }

    if (texWidth == cachedTexW_ && texHeight == cachedTexH_ &&
        viewWidth == cachedViewW_ && viewHeight == cachedViewH_ &&
        outFmt == cachedOutFmt_ && edgesTex_ && blendTex_ && outputTex_) {
        return true;
    }

    auto makeTex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                       ComPtr<ID3D11Texture2D>& tex,
                       ComPtr<ID3D11UnorderedAccessView>& uav,
                       ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
        tex.Reset();
        uav.Reset();
        srv.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, tex.GetAddressOf())) ||
            FAILED(device_->CreateUnorderedAccessView(tex.Get(), nullptr, uav.GetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()))) {
            return false;
        }
        return true;
    };

    if (!makeTex(viewWidth, viewHeight, DXGI_FORMAT_R16G16_FLOAT, edgesTex_, edgesUav_, edgesSrv_) ||
        !makeTex(viewWidth, viewHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, blendTex_, blendUav_, blendSrv_) ||
        !makeTex(texWidth, texHeight, outFmt, outputTex_, outputUav_, outputSrv_)) {
        LogMessage(LogLevel::Error, "SmaaPassD3D11: Failed to allocate intermediate SMAA textures.");
        return false;
    }

    cachedTexW_ = texWidth;
    cachedTexH_ = texHeight;
    cachedViewW_ = viewWidth;
    cachedViewH_ = viewHeight;
    cachedOutFmt_ = outFmt;
    return true;
}

ID3D11ShaderResourceView* SmaaPassD3D11::Apply(
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
    float fovealRadius) {
    if (!device || !context || !inputSrv || viewWidth == 0 || viewHeight == 0) {
        return inputSrv;
    }

    if (!Initialize(device)) {
        return inputSrv;
    }

    if (!EnsureTextures(texWidth, texHeight, viewWidth, viewHeight, format)) {
        return inputSrv;
    }

    SmaaConstants cb{};
    cb.rtMetrics[0] = 1.0f / static_cast<float>(viewWidth);
    cb.rtMetrics[1] = 1.0f / static_cast<float>(viewHeight);
    cb.rtMetrics[2] = static_cast<float>(viewWidth);
    cb.rtMetrics[3] = static_cast<float>(viewHeight);
    cb.threshold = 0.025f;
    cb.maxSearchSteps = 64;
    cb.fovealRadius = fovealRadius;
    cb.projectionCenter[0] = projCenterX;
    cb.projectionCenter[1] = projCenterY;
    cb.viewportOffset[0] = viewX;
    cb.viewportOffset[1] = viewY;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        context->Unmap(constantBuffer_.Get(), 0);
    }

    const UINT gx = (viewWidth + 7) / 8;
    const UINT gy = (viewHeight + 7) / 8;

    ID3D11Buffer* cbs[] = { constantBuffer_.Get() };
    ID3D11SamplerState* smpls[] = { linearSampler_.Get(), pointSampler_.Get() };
    ID3D11UnorderedAccessView* nullUav[] = { nullptr };
    ID3D11ShaderResourceView* nullSrvs[] = { nullptr, nullptr, nullptr };

    // Pass 1: Edge Detection
    {
        ID3D11UnorderedAccessView* uavs[] = { edgesUav_.Get() };
        ID3D11ShaderResourceView* srvs[] = { inputSrv };
        context->CSSetShader(shaderEdge_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cbs);
        context->CSSetSamplers(0, 2, smpls);
        context->CSSetShaderResources(0, 1, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch(gx, gy, 1);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CSSetShaderResources(0, 1, nullSrvs);
    }

    // Pass 2: Blend Weight Calculation
    {
        ID3D11UnorderedAccessView* uavs[] = { blendUav_.Get() };
        ID3D11ShaderResourceView* srvs[] = { edgesSrv_.Get(), areaTexSrv_.Get(), searchTexSrv_.Get() };
        context->CSSetShader(shaderBlend_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cbs);
        context->CSSetSamplers(0, 2, smpls);
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch(gx, gy, 1);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CSSetShaderResources(0, 3, nullSrvs);
    }

    // Pass 3: Neighborhood Blending
    {
        ID3D11UnorderedAccessView* uavs[] = { outputUav_.Get() };
        ID3D11ShaderResourceView* srvs[] = { inputSrv, blendSrv_.Get() };
        context->CSSetShader(shaderNeighbor_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, cbs);
        context->CSSetSamplers(0, 2, smpls);
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch(gx, gy, 1);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CSSetShaderResources(0, 2, nullSrvs);
    }

    return outputSrv_.Get();
}

} // namespace vrpacing

#endif