#ifdef _WIN32

#include "d3d11_helper.h"

#include <sstream>
#include <stdexcept>

#include "common/logging_win.h"

namespace {

std::string MapResult(HRESULT result) {
    switch (result) {
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_FAIL: return "E_FAIL";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    default: {
        std::ostringstream s;
        s << "0x" << std::hex << result;
        return s.str();
    }
    }
}

} // namespace

namespace vrpacing {

void CheckResult(const std::string& action, HRESULT result) {
    if (FAILED(result)) {
        const std::string message = "Fallo en " + action + ": " + MapResult(result);
        LogMessage(LogLevel::Error, message);
        throw std::runtime_error(message);
    }
}

ComPtr<ID3D11Buffer> CreateConstantsBuffer(ID3D11Device* device, uint32_t size) {
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = 0;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;
    bd.ByteWidth = size;
    ComPtr<ID3D11Buffer> buffer;
    CheckResult("creando constant buffer", device->CreateBuffer(&bd, nullptr, buffer.GetAddressOf()));
    return buffer;
}

ComPtr<ID3D11SamplerState> CreateLinearSampler(ID3D11Device* device) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = 0;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = 0;
    ComPtr<ID3D11SamplerState> sampler;
    CheckResult("creando sampler linear", device->CreateSamplerState(&sd, sampler.GetAddressOf()));
    return sampler;
}

DXGI_FORMAT TranslateTypelessFormats(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return format;
    }
}

DXGI_FORMAT MakeSrgbFormatsTypeless(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default: return format;
    }
}

bool IsSrgbFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

ComPtr<ID3D11Texture2D> CreateResolveTexture(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (format != DXGI_FORMAT_UNKNOWN) td.Format = format;

    ComPtr<ID3D11Texture2D> resolved;
    CheckResult("creando resolve texture", device->CreateTexture2D(&td, nullptr, resolved.GetAddressOf()));
    return resolved;
}

ComPtr<ID3D11Texture2D> CreatePostProcessTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> post;
    CheckResult("creando post texture", device->CreateTexture2D(&td, nullptr, post.GetAddressOf()));
    return post;
}

void StoreD3D11State(ID3D11DeviceContext* context, D3D11State& state) {
    context->CSGetShader(state.computeShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
    context->CSGetConstantBuffers(0, 1, state.csConstantBuffer.ReleaseAndGetAddressOf());
    context->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, state.csShaderResources);
    context->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, state.csUavs);
}

void RestoreD3D11State(ID3D11DeviceContext* context, const D3D11State& state) {
    context->CSSetShader(state.computeShader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, state.csConstantBuffer.GetAddressOf());
    context->CSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, state.csShaderResources);

    for (int i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
        if (state.csShaderResources[i]) {
            state.csShaderResources[i]->Release();
        }
    }

    UINT initCounts[D3D11_1_UAV_SLOT_COUNT] = {};
    context->CSSetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, state.csUavs, initCounts);

    for (int i = 0; i < D3D11_1_UAV_SLOT_COUNT; ++i) {
        if (state.csUavs[i]) {
            state.csUavs[i]->Release();
        }
    }
}

ComPtr<ID3D11ShaderResourceView> CreateShaderResourceView(ID3D11Device* device, ID3D11Texture2D* texture, int arrayIndex) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = TranslateTypelessFormats(td.Format);

    if (td.ArraySize > 1) {
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvd.Texture2DArray.FirstArraySlice = (arrayIndex >= 0) ? static_cast<UINT>(arrayIndex) : 0;
        srvd.Texture2DArray.ArraySize = (arrayIndex >= 0) ? 1 : td.ArraySize;
        srvd.Texture2DArray.MipLevels = td.MipLevels ? td.MipLevels : 1;
        srvd.Texture2DArray.MostDetailedMip = 0;
    } else {
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = td.MipLevels ? td.MipLevels : 1;
        srvd.Texture2D.MostDetailedMip = 0;
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(texture, &srvd, srv.GetAddressOf());
    if (FAILED(hr)) {
        // Fallback para texturas sin formato typeless
        hr = device->CreateShaderResourceView(texture, nullptr, srv.GetAddressOf());
        CheckResult("creando SRV fallback", hr);
    }
    return srv;
}

ComPtr<ID3D11UnorderedAccessView> CreateUnorderedAccessView(ID3D11Device* device, ID3D11Texture2D* texture, int arrayIndex) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = TranslateTypelessFormats(td.Format);
    if (uavd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        uavd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (uavd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        uavd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    if (td.ArraySize > 1) {
        uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        uavd.Texture2DArray.FirstArraySlice = (arrayIndex >= 0) ? static_cast<UINT>(arrayIndex) : 0;
        uavd.Texture2DArray.ArraySize = (arrayIndex >= 0) ? 1 : td.ArraySize;
        uavd.Texture2DArray.MipSlice = 0;
    } else {
        uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavd.Texture2D.MipSlice = 0;
    }

    ComPtr<ID3D11UnorderedAccessView> uav;
    HRESULT hr = device->CreateUnorderedAccessView(texture, &uavd, uav.GetAddressOf());
    if (FAILED(hr)) {
        hr = device->CreateUnorderedAccessView(texture, nullptr, uav.GetAddressOf());
        CheckResult("creando UAV fallback", hr);
    }
    return uav;
}

} // namespace vrpacing

#endif