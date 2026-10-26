#ifdef _WIN32

#include "nis_upscaler_d3d11.h"

#include <cmath>
#include <algorithm>

#include "d3d11_helper.h"
#include "shader_nis_upscale.h"
#include "common/logging_win.h"
#include "nis/NIS_Config.h"

namespace vrpacing {
namespace {

struct StagingResources {
    ID3D11Device* device = nullptr;

    ComPtr<ID3D11Texture2D> srcTex;
    ComPtr<ID3D11ShaderResourceView> srcSrv;
    uint32_t srcW = 0;
    uint32_t srcH = 0;
    DXGI_FORMAT srcFmt = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D11Texture2D> dstTex;
    ComPtr<ID3D11UnorderedAccessView> dstUav;
    uint32_t dstW = 0;
    uint32_t dstH = 0;
    DXGI_FORMAT dstFmt = DXGI_FORMAT_UNKNOWN;

    void Reset() {
        device = nullptr;
        srcTex.Reset();
        srcSrv.Reset();
        srcW = 0;
        srcH = 0;
        srcFmt = DXGI_FORMAT_UNKNOWN;
        dstTex.Reset();
        dstUav.Reset();
        dstW = 0;
        dstH = 0;
        dstFmt = DXGI_FORMAT_UNKNOWN;
    }
};

StagingResources g_staging;

DXGI_FORMAT GetCompatibleUnormFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return format;
    }
}

} // namespace

bool NisUpscalerD3D11::Initialize(ID3D11Device* device) {
    if (!device) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: Puntero ID3D11Device nulo.");
        return false;
    }

    if (device_.Get() != device) {
        g_staging.Reset();
        srvCache_.clear();
        uavCache_.clear();
    }

    device_ = device;

    HRESULT hr = device_->CreateComputeShader(
        g_NISUpscaleShader,
        sizeof(g_NISUpscaleShader),
        nullptr,
        upscaleShader_.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr)) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error al crear el Compute Shader de NIS.");
        return false;
    }

    try {
        constantsBuffer_ = CreateConstantsBuffer(device_.Get(), sizeof(NISConfig));
        sampler_ = CreateLinearSampler(device_.Get());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFilterSize / 4;
        td.Height = kPhaseCount;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA texData{};
        texData.pSysMem = coef_scale;
        texData.SysMemPitch = kFilterSize * 4;
        texData.SysMemSlicePitch = kFilterSize * 4 * kPhaseCount;
        CheckResult("creando textura coeficientes scaler", device_->CreateTexture2D(&td, &texData, scalerCoeffTexture_.ReleaseAndGetAddressOf()));

        texData.pSysMem = coef_usm;
        CheckResult("creando textura coeficientes USM", device_->CreateTexture2D(&td, &texData, usmCoeffTexture_.ReleaseAndGetAddressOf()));

        scalerCoeffView_ = CreateShaderResourceView(device_.Get(), scalerCoeffTexture_.Get());
        usmCoeffView_ = CreateShaderResourceView(device_.Get(), usmCoeffTexture_.Get());
    } catch (const std::exception& e) {
        LogMessage(LogLevel::Error, std::string("NisUpscalerD3D11: Excepcion en inicializacion: ") + e.what());
        return false;
    }

    initialized_ = true;
    LogMessage(LogLevel::Info, "NisUpscalerD3D11 inicializado correctamente con Compute Shader compilado.");
    return true;
}

ComPtr<ID3D11ShaderResourceView> NisUpscalerD3D11::GetOrCreateSrv(ID3D11Texture2D* tex, int arrayIndex) {
    const auto key = std::make_pair(tex, arrayIndex);
    auto it = srvCache_.find(key);
    if (it != srvCache_.end()) {
        return it->second;
    }

    if (srvCache_.size() > 64) {
        srvCache_.clear();
    }

    auto srv = CreateShaderResourceView(device_.Get(), tex, arrayIndex);
    srvCache_[key] = srv;
    return srv;
}

ComPtr<ID3D11UnorderedAccessView> NisUpscalerD3D11::GetOrCreateUav(ID3D11Texture2D* tex, int arrayIndex) {
    const auto key = std::make_pair(tex, arrayIndex);
    auto it = uavCache_.find(key);
    if (it != uavCache_.end()) {
        return it->second;
    }

    if (uavCache_.size() > 64) {
        uavCache_.clear();
    }

    auto uav = CreateUnorderedAccessView(device_.Get(), tex, arrayIndex);
    uavCache_[key] = uav;
    return uav;
}

bool NisUpscalerD3D11::Upscale(ID3D11DeviceContext* ctx,
                               ID3D11ShaderResourceView* srcSrv,
                               ID3D11UnorderedAccessView* dstUav,
                               uint32_t srcWidth,
                               uint32_t srcHeight,
                               uint32_t dstWidth,
                               uint32_t dstHeight,
                               const UpscaleDispatchDesc& desc) {
    if (!initialized_ || !ctx || !srcSrv || !dstUav) {
        return false;
    }

    const uint64_t totalPixels = static_cast<uint64_t>(dstWidth) * static_cast<uint64_t>(dstHeight);
    if (totalPixels > desc.maxOutputPixels) {
        LogMessage(LogLevel::Warn, "NisUpscalerD3D11: Omitido por exceder maxOutputPixels.");
        return false;
    }

    ViewportRect srcR = desc.srcRect;
    if (srcR.width == 0 || srcR.height == 0) {
        srcR.x = 0;
        srcR.y = 0;
        srcR.width = srcWidth;
        srcR.height = srcHeight;
    }

    ViewportRect dstR = desc.dstRect;
    if (dstR.width == 0 || dstR.height == 0) {
        dstR.x = 0;
        dstR.y = 0;
        dstR.width = dstWidth;
        dstR.height = dstHeight;
    }

    D3D11State state{};
    StoreD3D11State(ctx, state);

    NISConfig constants{};
    NVScalerUpdateConfig(
        constants,
        desc.sharpness,
        srcR.x, srcR.y, srcR.width, srcR.height, srcWidth, srcHeight,
        dstR.x, dstR.y, dstR.width, dstR.height, dstWidth, dstHeight
    );

    const float radius = 0.5f * desc.radius * static_cast<float>(dstR.height);
    constants.projCentre[0] = static_cast<uint32_t>(static_cast<float>(dstR.width) * desc.projCenterX);
    constants.projCentre[1] = static_cast<uint32_t>(static_cast<float>(dstR.height) * desc.projCenterY);
    constants.squaredRadius = static_cast<uint32_t>(radius * radius);
    constants.squaredMaxRadius = 0xFFFFFFFFu;
    constants.debugMode = 0;

    ctx->UpdateSubresource(constantsBuffer_.Get(), 0, nullptr, &constants, 0, 0);

    ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());

    ID3D11ShaderResourceView* srvs[3] = {
        srcSrv,
        scalerCoeffView_.Get(),
        usmCoeffView_.Get()
    };
    ctx->CSSetShaderResources(0, 3, srvs);

    ID3D11UnorderedAccessView* uavs[1] = { dstUav };
    UINT initCounts[1] = { 0 };
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, initCounts);

    ctx->CSSetConstantBuffers(0, 1, constantsBuffer_.GetAddressOf());
    ctx->CSSetShader(upscaleShader_.Get(), nullptr, 0);

    const UINT groupsX = (dstR.width + 31) / 32;
    const UINT groupsY = (dstR.height + 23) / 24;
    ctx->Dispatch(groupsX, groupsY, 1);

    RestoreD3D11State(ctx, state);
    return true;
}

bool NisUpscalerD3D11::Upscale(ID3D11DeviceContext* ctx,
                               ID3D11Texture2D* src,
                               ID3D11Texture2D* dst,
                               const UpscaleDispatchDesc& desc) {
    if (!src || !dst || !ctx) {
        return false;
    }

    ComPtr<ID3D11Device> dev;
    src->GetDevice(dev.GetAddressOf());
    if (!dev) return false;

    if (!initialized_ || device_.Get() != dev.Get()) {
        if (!Initialize(dev.Get())) {
            return false;
        }
    }

    if (g_staging.device != device_.Get()) {
        g_staging.Reset();
        g_staging.device = device_.Get();
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC dstDesc{};
    dst->GetDesc(&dstDesc);

    DXGI_FORMAT srcUnormFmt = (srcDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS || srcDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                              ? DXGI_FORMAT_R16G16B16A16_FLOAT
                              : GetCompatibleUnormFormat(srcDesc.Format);

    DXGI_FORMAT dstUnormFmt = (dstDesc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS || dstDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                              ? DXGI_FORMAT_R16G16B16A16_FLOAT
                              : GetCompatibleUnormFormat(dstDesc.Format);

    if (!g_staging.srcTex || g_staging.srcW != srcDesc.Width || g_staging.srcH != srcDesc.Height || g_staging.srcFmt != srcUnormFmt) {
        g_staging.srcTex.Reset();
        g_staging.srcSrv.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = srcDesc.Width;
        td.Height = srcDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = srcUnormFmt;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, g_staging.srcTex.GetAddressOf())) ||
            FAILED(device_->CreateShaderResourceView(g_staging.srcTex.Get(), nullptr, g_staging.srcSrv.GetAddressOf()))) {
            LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error creando staging SRV UNORM.");
            return false;
        }
        g_staging.srcW = srcDesc.Width;
        g_staging.srcH = srcDesc.Height;
        g_staging.srcFmt = srcUnormFmt;
    }

    if (!g_staging.dstTex || g_staging.dstW != dstDesc.Width || g_staging.dstH != dstDesc.Height || g_staging.dstFmt != dstUnormFmt) {
        g_staging.dstTex.Reset();
        g_staging.dstUav.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = dstDesc.Width;
        td.Height = dstDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dstUnormFmt;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, g_staging.dstTex.GetAddressOf())) ||
            FAILED(device_->CreateUnorderedAccessView(g_staging.dstTex.Get(), nullptr, g_staging.dstUav.GetAddressOf()))) {
            LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error creando staging UAV UNORM.");
            return false;
        }
        g_staging.dstW = dstDesc.Width;
        g_staging.dstH = dstDesc.Height;
        g_staging.dstFmt = dstUnormFmt;
    }

    const UINT srcSlice = (desc.srcArrayIndex >= 0 && static_cast<UINT>(desc.srcArrayIndex) < srcDesc.ArraySize)
                          ? static_cast<UINT>(desc.srcArrayIndex) : 0;
    const UINT srcSub = D3D11CalcSubresource(0, srcSlice, srcDesc.MipLevels);

    if (srcDesc.SampleDesc.Count > 1) {
        ctx->ResolveSubresource(g_staging.srcTex.Get(), 0, src, srcSub, srcUnormFmt);
    } else {
        ctx->CopySubresourceRegion(g_staging.srcTex.Get(), 0, 0, 0, 0, src, srcSub, nullptr);
    }

    if (!Upscale(ctx, g_staging.srcSrv.Get(), g_staging.dstUav.Get(),
                 srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height, desc)) {
        return false;
    }

    const UINT dstSlice = (desc.dstArrayIndex >= 0 && static_cast<UINT>(desc.dstArrayIndex) < dstDesc.ArraySize)
                          ? static_cast<UINT>(desc.dstArrayIndex) : 0;
    const UINT dstSub = D3D11CalcSubresource(0, dstSlice, dstDesc.MipLevels);
    ctx->CopySubresourceRegion(dst, dstSub, 0, 0, 0, g_staging.dstTex.Get(), 0, nullptr);

    return true;
}

} // namespace vrpacing

#endif