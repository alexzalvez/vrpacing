#ifdef _WIN32

#include "nis_upscaler_d3d11.h"

#include <cmath>
#include <algorithm>
#include <unordered_set>

#include "d3d11_helper.h"
#include "shader_nis_upscale.h"
#include "common/logging_win.h"
#include "nis/NIS_Config.h"

namespace vrpacing {

bool NisUpscalerD3D11::Initialize(ID3D11Device* device) {
    if (!device) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: Puntero ID3D11Device nulo.");
        return false;
    }

    device_ = device;

    HRESULT hr = device_->CreateComputeShader(
        g_NISUpscaleShader,
        sizeof(g_NISUpscaleShader),
        nullptr,
        upscaleShader_.GetAddressOf()
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
        CheckResult("creando textura coeficientes scaler", device_->CreateTexture2D(&td, &texData, scalerCoeffTexture_.GetAddressOf()));

        texData.pSysMem = coef_usm;
        CheckResult("creando textura coeficientes USM", device_->CreateTexture2D(&td, &texData, usmCoeffTexture_.GetAddressOf()));

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

    thread_local static std::unordered_set<ID3D11UnorderedAccessView*> clearedUavs;
    if (clearedUavs.find(dstUav) == clearedUavs.end()) {
        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        ctx->ClearUnorderedAccessViewFloat(dstUav, clearColor);
        if (clearedUavs.size() > 64) clearedUavs.clear();
        clearedUavs.insert(dstUav);
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

    if (desc.maskEnabled) {
        const float maxRadius = 0.5f * 0.88f * static_cast<float>(dstR.height);
        constants.squaredMaxRadius = static_cast<uint32_t>(maxRadius * maxRadius);
    } else {
        constants.squaredMaxRadius = 0xFFFFFFFFu;
    }

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

    if (!initialized_) {
        ComPtr<ID3D11Device> dev;
        src->GetDevice(dev.GetAddressOf());
        if (!dev || !Initialize(dev.Get())) {
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC dstDesc{};
    dst->GetDesc(&dstDesc);

    auto srcSrv = GetOrCreateSrv(src, desc.srcArrayIndex);
    auto dstUav = GetOrCreateUav(dst, desc.dstArrayIndex);

    if (!srcSrv || !dstUav) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: No se pudieron crear las vistas SRV/UAV de las texturas.");
        return false;
    }

    return Upscale(
        ctx,
        srcSrv.Get(),
        dstUav.Get(),
        srcDesc.Width,
        srcDesc.Height,
        dstDesc.Width,
        dstDesc.Height,
        desc
    );
}

} // namespace vrpacing

#endif