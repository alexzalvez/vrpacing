#ifdef _WIN32

#include "nis_upscaler_d3d11.h"

#include <wincodec.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "d3d11_helper.h"
#include "smaa_pass_d3d11.h"
#include "shader_nis_upscale.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "nis/NIS_Config.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace vrpacing {

namespace {

struct StagingFallback {
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

StagingFallback g_fallback;
SmaaPassD3D11 g_smaaPass;
SharedMemoryClient g_screenshotShm;

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

float HalfToFloat(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x03FFu;

    uint32_t bits = 0;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }

    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(float));
    return out;
}

uint8_t FloatToSrgbByte(float v) noexcept {
    const float clamped = (std::max)(0.0f, (std::min)(1.0f, v));
    const float srgb = (clamped <= 0.0031308f)
                           ? (clamped * 12.92f)
                           : (1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f);
    return static_cast<uint8_t>(std::lround(srgb * 255.0f));
}

bool SaveBgraBufferToPngWic(const std::wstring& filePath, const uint8_t* bgraData, uint32_t width, uint32_t height) {
    if (!bgraData || width == 0 || height == 0) return false;

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mustUninit = (hrCo == S_OK || hrCo == S_FALSE);

    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        if (SUCCEEDED(hr) && factory) {
            ComPtr<IWICStream> stream;
            hr = factory->CreateStream(stream.GetAddressOf());
            if (SUCCEEDED(hr) && stream) {
                hr = stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
                if (SUCCEEDED(hr)) {
                    ComPtr<IWICBitmapEncoder> encoder;
                    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
                    if (SUCCEEDED(hr) && encoder) {
                        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                        if (SUCCEEDED(hr)) {
                            ComPtr<IWICBitmapFrameEncode> frame;
                            hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
                            if (SUCCEEDED(hr) && frame) {
                                hr = frame->Initialize(nullptr);
                                if (SUCCEEDED(hr)) {
                                    hr = frame->SetSize(width, height);
                                    if (SUCCEEDED(hr)) {
                                        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                                        hr = frame->SetPixelFormat(&fmt);
                                        if (SUCCEEDED(hr)) {
                                            const UINT stride = width * 4;
                                            const UINT totalBytes = stride * height;
                                            hr = frame->WritePixels(height, stride, totalBytes, const_cast<BYTE*>(bgraData));
                                            if (SUCCEEDED(hr)) {
                                                hr = frame->Commit();
                                                if (SUCCEEDED(hr)) {
                                                    hr = encoder->Commit();
                                                    ok = SUCCEEDED(hr);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (mustUninit) {
        CoUninitialize();
    }
    return ok;
}

bool CaptureTextureRegionToPng(ID3D11Device* device,
                               ID3D11DeviceContext* ctx,
                               ID3D11Texture2D* tex,
                               int arrayIndex,
                               ViewportRect rect,
                               const std::wstring& filePath) {
    if (!device || !ctx || !tex) return false;

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) return false;

    if (rect.width == 0 || rect.height == 0) {
        rect.x = 0;
        rect.y = 0;
        rect.width = desc.Width;
        rect.height = desc.Height;
    }

    if (rect.x >= desc.Width || rect.y >= desc.Height) return false;
    const uint32_t cropW = (std::min)(rect.width, desc.Width - rect.x);
    const uint32_t cropH = (std::min)(rect.height, desc.Height - rect.y);
    if (cropW == 0 || cropH == 0) return false;

    D3D11_TEXTURE2D_DESC stDesc{};
    stDesc.Width = cropW;
    stDesc.Height = cropH;
    stDesc.MipLevels = 1;
    stDesc.ArraySize = 1;
    stDesc.Format = desc.Format;
    stDesc.SampleDesc.Count = 1;
    stDesc.SampleDesc.Quality = 0;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stDesc, nullptr, staging.GetAddressOf())) || !staging) {
        return false;
    }

    const UINT slice = (arrayIndex >= 0 && static_cast<UINT>(arrayIndex) < desc.ArraySize)
                           ? static_cast<UINT>(arrayIndex) : 0;
    const UINT sub = D3D11CalcSubresource(0, slice, desc.MipLevels);

    D3D11_BOX box{};
    box.left = rect.x;
    box.top = rect.y;
    box.front = 0;
    box.right = rect.x + cropW;
    box.bottom = rect.y + cropH;
    box.back = 1;

    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, sub, &box);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
        return false;
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(cropW) * static_cast<size_t>(cropH) * 4u, 255u);
    const auto* srcBytes = static_cast<const uint8_t*>(mapped.pData);

    const DXGI_FORMAT baseFmt = GetCompatibleUnormFormat(desc.Format);
    bool supportedFmt = true;

    for (uint32_t y = 0; y < cropH; ++y) {
        const uint8_t* row = srcBytes + static_cast<size_t>(y) * mapped.RowPitch;
        uint8_t* dstRow = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(cropW) * 4u;

        if (baseFmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            for (uint32_t x = 0; x < cropW; ++x) {
                dstRow[x * 4 + 0] = row[x * 4 + 2]; // B
                dstRow[x * 4 + 1] = row[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = row[x * 4 + 0]; // R
                dstRow[x * 4 + 3] = 255u;           // A (opaque)
            }
        } else if (baseFmt == DXGI_FORMAT_B8G8R8A8_UNORM || baseFmt == DXGI_FORMAT_B8G8R8X8_UNORM) {
            for (uint32_t x = 0; x < cropW; ++x) {
                dstRow[x * 4 + 0] = row[x * 4 + 0]; // B
                dstRow[x * 4 + 1] = row[x * 4 + 1]; // G
                dstRow[x * 4 + 2] = row[x * 4 + 2]; // R
                dstRow[x * 4 + 3] = 255u;           // A (opaque)
            }
        } else if (baseFmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
            const auto* p32 = reinterpret_cast<const uint32_t*>(row);
            for (uint32_t x = 0; x < cropW; ++x) {
                const uint32_t v = p32[x];
                dstRow[x * 4 + 2] = static_cast<uint8_t>(((v & 0x3FFu) * 255u + 511u) / 1023u);
                dstRow[x * 4 + 1] = static_cast<uint8_t>((((v >> 10) & 0x3FFu) * 255u + 511u) / 1023u);
                dstRow[x * 4 + 0] = static_cast<uint8_t>((((v >> 20) & 0x3FFu) * 255u + 511u) / 1023u);
                dstRow[x * 4 + 3] = 255u;
            }
        } else if (baseFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const auto* p16 = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < cropW; ++x) {
                const float r = HalfToFloat(p16[x * 4 + 0]);
                const float g = HalfToFloat(p16[x * 4 + 1]);
                const float b = HalfToFloat(p16[x * 4 + 2]);
                dstRow[x * 4 + 0] = FloatToSrgbByte(b);
                dstRow[x * 4 + 1] = FloatToSrgbByte(g);
                dstRow[x * 4 + 2] = FloatToSrgbByte(r);
                dstRow[x * 4 + 3] = 255u;
            }
        } else if (desc.Format == DXGI_FORMAT_R16G16B16A16_UNORM) {
            const auto* p16 = reinterpret_cast<const uint16_t*>(row);
            for (uint32_t x = 0; x < cropW; ++x) {
                dstRow[x * 4 + 2] = static_cast<uint8_t>(p16[x * 4 + 0] >> 8);
                dstRow[x * 4 + 1] = static_cast<uint8_t>(p16[x * 4 + 1] >> 8);
                dstRow[x * 4 + 0] = static_cast<uint8_t>(p16[x * 4 + 2] >> 8);
                dstRow[x * 4 + 3] = 255u;
            }
        } else {
            supportedFmt = false;
            break;
        }
    }

    ctx->Unmap(staging.Get(), 0);
    if (!supportedFmt) return false;

    return SaveBgraBufferToPngWic(filePath, bgra.data(), cropW, cropH);
}

std::wstring GetCleanProcessStem() {
    wchar_t modPath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, modPath, MAX_PATH)) {
        return L"Game";
    }
    std::filesystem::path p(modPath);
    std::wstring stem = p.stem().wstring();
    for (wchar_t& c : stem) {
        if (c == L' ' || c == L':' || c == L'/' || c == L'\\') c = L'_';
    }
    return stem.empty() ? L"Game" : stem;
}

} // namespace

void NisUpscalerD3D11::ClearCache() {
    srvCache_.clear();
    uavCache_.clear();
    g_fallback.Reset();
    g_smaaPass.Reset();
}

bool NisUpscalerD3D11::Initialize(ID3D11Device* device) {
    if (!device) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: Puntero ID3D11Device nulo.");
        return false;
    }
    if (device_.Get() != device) {
        ClearCache();
    }
    device_ = device;

    HRESULT hr = device_->CreateComputeShader(
        g_NISUpscaleShader,
        sizeof(g_NISUpscaleShader),
        nullptr,
        upscaleShader_.ReleaseAndGetAddressOf()
    );
    if (FAILED(hr)) {
        LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error al crear el Compute Shader de NIS. HR=" + std::to_string(hr));
        return false;
    }

    try {
        constantsBuffer_ = CreateConstantsBuffer(device_.Get(), sizeof(NISConfig));
        sampler_ = CreateLinearSampler(device_.Get());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFilterSize / 4;
        td.Height = kPhaseCount;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
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
    LogMessage(LogLevel::Info, "NisUpscalerD3D11 initialized successfully with compiled Compute Shader.");
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

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = GetCompatibleUnormFormat(desc.Format);

    if (desc.ArraySize > 1) {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.FirstArraySlice = (arrayIndex >= 0 && static_cast<UINT>(arrayIndex) < desc.ArraySize) ? static_cast<UINT>(arrayIndex) : 0;
        srvDesc.Texture2DArray.ArraySize = 1;
        srvDesc.Texture2DArray.MipLevels = desc.MipLevels ? desc.MipLevels : 1;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
    } else {
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels ? desc.MipLevels : 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(device_->CreateShaderResourceView(tex, &srvDesc, srv.GetAddressOf()))) {
        srvCache_[key] = srv;
    } else {
        srvCache_[key] = nullptr;
    }
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

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0) {
        uavCache_[key] = nullptr;
        return nullptr;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = GetCompatibleUnormFormat(desc.Format);

    if (desc.ArraySize > 1) {
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Texture2DArray.FirstArraySlice = (arrayIndex >= 0 && static_cast<UINT>(arrayIndex) < desc.ArraySize) ? static_cast<UINT>(arrayIndex) : 0;
        uavDesc.Texture2DArray.ArraySize = 1;
        uavDesc.Texture2DArray.MipSlice = 0;
    } else {
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
    }

    ComPtr<ID3D11UnorderedAccessView> uav;
    if (SUCCEEDED(device_->CreateUnorderedAccessView(tex, &uavDesc, uav.GetAddressOf()))) {
        uavCache_[key] = uav;
    } else {
        uavCache_[key] = nullptr;
    }
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

    if (g_fallback.device != device_.Get()) {
        g_fallback.Reset();
        g_smaaPass.Reset();
        g_fallback.device = device_.Get();
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    D3D11_TEXTURE2D_DESC dstDesc{};
    dst->GetDesc(&dstDesc);

    if (!g_screenshotShm.GetRaw()) {
        g_screenshotShm.OpenOrCreate();
    }
    SharedState* shm = g_screenshotShm.GetRaw();
    const bool smaaRequested = (shm && (shm->flags & kFlagSmaaEnabled) != 0);

    // RUTA 1: Zero-Copy Direct SRV (si es 2D simple sin MSAA ni ArraySlice > 0 cuando SMAA esta activo)
    ComPtr<ID3D11ShaderResourceView> activeSrcSrv;
    if (srcDesc.SampleDesc.Count <= 1 && !(smaaRequested && srcDesc.ArraySize > 1)) {
        activeSrcSrv = GetOrCreateSrv(src, desc.srcArrayIndex);
    }

    // Si la lectura directa no es posible (ej: MSAA o Texture2DArray para SMAA), usar fallback staging 2D
    if (!activeSrcSrv) {
        DXGI_FORMAT srcUnormFmt = GetCompatibleUnormFormat(srcDesc.Format);
        if (!g_fallback.srcTex || g_fallback.srcW != srcDesc.Width || g_fallback.srcH != srcDesc.Height || g_fallback.srcFmt != srcUnormFmt) {
            g_fallback.srcTex.Reset();
            g_fallback.srcSrv.Reset();

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

            if (FAILED(device_->CreateTexture2D(&td, nullptr, g_fallback.srcTex.GetAddressOf())) ||
                FAILED(device_->CreateShaderResourceView(g_fallback.srcTex.Get(), nullptr, g_fallback.srcSrv.GetAddressOf()))) {
                LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error en fallback staging SRV.");
                return false;
            }
            g_fallback.srcW = srcDesc.Width;
            g_fallback.srcH = srcDesc.Height;
            g_fallback.srcFmt = srcUnormFmt;
        }

        const UINT srcSlice = (desc.srcArrayIndex >= 0 && static_cast<UINT>(desc.srcArrayIndex) < srcDesc.ArraySize)
                              ? static_cast<UINT>(desc.srcArrayIndex) : 0;
        const UINT srcSub = D3D11CalcSubresource(0, srcSlice, srcDesc.MipLevels);

        if (srcDesc.SampleDesc.Count > 1) {
            ctx->ResolveSubresource(g_fallback.srcTex.Get(), 0, src, srcSub, srcUnormFmt);
        } else {
            ctx->CopySubresourceRegion(g_fallback.srcTex.Get(), 0, 0, 0, 0, src, srcSub, nullptr);
        }
        activeSrcSrv = g_fallback.srcSrv;
    }

    // RUTA 2: Zero-Copy Direct UAV (escritura directa en destino si soporta UAV)
    ComPtr<ID3D11UnorderedAccessView> activeDstUav = GetOrCreateUav(dst, desc.dstArrayIndex);
    const bool directWriteOk = (activeDstUav != nullptr);

    // Si el destino no admite UAV directo (ej: swapchain sRGB), usar fallback staging
    if (!directWriteOk) {
        DXGI_FORMAT dstUnormFmt = GetCompatibleUnormFormat(dstDesc.Format);
        if (!g_fallback.dstTex || g_fallback.dstW != dstDesc.Width || g_fallback.dstH != dstDesc.Height || g_fallback.dstFmt != dstUnormFmt) {
            g_fallback.dstTex.Reset();
            g_fallback.dstUav.Reset();

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

            if (FAILED(device_->CreateTexture2D(&td, nullptr, g_fallback.dstTex.GetAddressOf())) ||
                FAILED(device_->CreateUnorderedAccessView(g_fallback.dstTex.Get(), nullptr, g_fallback.dstUav.GetAddressOf()))) {
                LogMessage(LogLevel::Error, "NisUpscalerD3D11: Error en fallback staging UAV.");
                return false;
            }
            g_fallback.dstW = dstDesc.Width;
            g_fallback.dstH = dstDesc.Height;
            g_fallback.dstFmt = dstUnormFmt;
        }
        activeDstUav = g_fallback.dstUav;
    }

    // Paso Pre-Upscale Opcional (Laboratorio): SMAA 1.0x antes de NIS 2.0x
    ID3D11ShaderResourceView* srvForNis = activeSrcSrv.Get();
    bool smaaAppliedThisFrame = false;

    if (smaaRequested) {
        ViewportRect srcR = desc.srcRect;
        if (srcR.width == 0 || srcR.height == 0) {
            srcR.x = 0;
            srcR.y = 0;
            srcR.width = srcDesc.Width;
            srcR.height = srcDesc.Height;
        }

        D3D11State smaaState{};
        StoreD3D11State(ctx, smaaState);

        ID3D11ShaderResourceView* smaaSrv = g_smaaPass.Apply(
            ctx,
            activeSrcSrv.Get(),
            srcDesc.Width,
            srcDesc.Height,
            srcR.x,
            srcR.y,
            srcR.width,
            srcR.height,
            srcDesc.Format,
            desc.projCenterX,
            desc.projCenterY,
            desc.radius
        );

        RestoreD3D11State(ctx, smaaState);

        if (smaaSrv && smaaSrv != activeSrcSrv.Get()) {
            srvForNis = smaaSrv;
            smaaAppliedThisFrame = true;
            if (shm) shm->smaaStatus = 1;
        } else {
            if (shm) shm->smaaStatus = -1;
        }
    } else {
        if (shm) shm->smaaStatus = 0;
    }

    if (!Upscale(ctx, srvForNis, activeDstUav.Get(),
                 srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height, desc)) {
        return false;
    }

    // Si usamos staging en destino, transferir resultado al swapchain
    if (!directWriteOk) {
        const UINT dstSlice = (desc.dstArrayIndex >= 0 && static_cast<UINT>(desc.dstArrayIndex) < dstDesc.ArraySize)
                              ? static_cast<UINT>(desc.dstArrayIndex) : 0;
        const UINT dstSub = D3D11CalcSubresource(0, dstSlice, dstDesc.MipLevels);

        if (desc.dstRect.width > 0 && desc.dstRect.height > 0 &&
            (desc.dstRect.width < dstDesc.Width || desc.dstRect.height < dstDesc.Height)) {
            D3D11_BOX srcBox = {
                desc.dstRect.x,
                desc.dstRect.y,
                0,
                desc.dstRect.x + desc.dstRect.width,
                desc.dstRect.y + desc.dstRect.height,
                1
            };
            ctx->CopySubresourceRegion(dst, dstSub, desc.dstRect.x, desc.dstRect.y, 0, g_fallback.dstTex.Get(), 0, &srcBox);
        } else {
            ctx->CopySubresourceRegion(dst, dstSub, 0, 0, 0, g_fallback.dstTex.Get(), 0, nullptr);
        }
    }

    // Verificar si el usuario solicito captura comparativa sincrona (Ctrl + F12) en el ojo izquierdo
    if (shm && shm->screenshotRequest != 0 && desc.srcArrayIndex == 0 && desc.srcRect.x < (srcDesc.Width / 4)) {
        shm->screenshotRequest = 0;

        std::filesystem::path outDir = (shm->screenshotDir[0] != L'\0')
                                           ? std::filesystem::path(shm->screenshotDir)
                                           : (std::filesystem::path(GetLogFilePath()).parent_path() / L"Screenshots");
        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t timeBuf[64]{};
        swprintf_s(timeBuf, L"%04u%02u%02u_%02u%02u%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        const std::wstring gameStem = GetCleanProcessStem();
        const uint32_t out1xW = (desc.srcRect.width > 0) ? desc.srcRect.width : srcDesc.Width;
        const uint32_t out1xH = (desc.srcRect.height > 0) ? desc.srcRect.height : srcDesc.Height;
        const uint32_t out2xW = (desc.dstRect.width > 0) ? desc.dstRect.width : dstDesc.Width;
        const uint32_t out2xH = (desc.dstRect.height > 0) ? desc.dstRect.height : dstDesc.Height;

        ID3D11Texture2D* srcCaptureTex = ((srcDesc.SampleDesc.Count > 1 || (smaaRequested && srcDesc.ArraySize > 1)) && g_fallback.srcTex)
                                             ? g_fallback.srcTex.Get() : src;
        const int srcCaptureSlice = (srcCaptureTex == g_fallback.srcTex.Get()) ? 0 : desc.srcArrayIndex;

        ID3D11Texture2D* dstCaptureTex = (!directWriteOk && g_fallback.dstTex) ? g_fallback.dstTex.Get() : dst;
        const int dstCaptureSlice = (dstCaptureTex == g_fallback.dstTex.Get()) ? 0 : desc.dstArrayIndex;

        wchar_t file1x[220]{};
        swprintf_s(file1x, L"VRPacing_%s_%s_1_Native_1x_%ux%u.png", timeBuf, gameStem.c_str(), out1xW, out1xH);
        const bool ok1 = CaptureTextureRegionToPng(device_.Get(), ctx, srcCaptureTex, srcCaptureSlice, desc.srcRect, (outDir / file1x).wstring());

        bool ok2 = false;
        if (smaaAppliedThisFrame && g_smaaPass.GetOutputTexture()) {
            wchar_t fileSmaa1x[220]{};
            wchar_t fileSmaa2x[220]{};
            swprintf_s(fileSmaa1x, L"VRPacing_%s_%s_2_SMAA_1x_%ux%u.png", timeBuf, gameStem.c_str(), out1xW, out1xH);
            swprintf_s(fileSmaa2x, L"VRPacing_%s_%s_3_SMAA_NIS_2x_%ux%u.png", timeBuf, gameStem.c_str(), out2xW, out2xH);

            CaptureTextureRegionToPng(device_.Get(), ctx, g_smaaPass.GetOutputTexture(), 0, desc.srcRect, (outDir / fileSmaa1x).wstring());
            ok2 = CaptureTextureRegionToPng(device_.Get(), ctx, dstCaptureTex, dstCaptureSlice, desc.dstRect, (outDir / fileSmaa2x).wstring());
        } else {
            wchar_t file2x[220]{};
            swprintf_s(file2x, L"VRPacing_%s_%s_2_NIS_2x_%ux%u.png", timeBuf, gameStem.c_str(), out2xW, out2xH);
            ok2 = CaptureTextureRegionToPng(device_.Get(), ctx, dstCaptureTex, dstCaptureSlice, desc.dstRect, (outDir / file2x).wstring());
        }

        if (ok1 && ok2) {
            shm->screenshotSavedCount++;
            LogMessage(LogLevel::Info, "Comparison screenshot set saved in Screenshots folder.");
        } else {
            LogMessage(LogLevel::Warn, "Comparison screenshot capture failed for current swapchain format.");
        }
    }

    return true;
}

} // namespace vrpacing

#endif
