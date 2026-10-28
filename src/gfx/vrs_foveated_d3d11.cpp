#ifdef _WIN32

#include "gfx/vrs_foveated_d3d11.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/logging_win.h"
#include "minhook/include/MinHook.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    UINT,
    ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*);

using OMSetRenderTargetsAndUAVsFn = void (STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*,
    UINT,
    ID3D11RenderTargetView* const*,
    ID3D11DepthStencilView*,
    UINT,
    UINT,
    ID3D11UnorderedAccessView* const*,
    const UINT*);

OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
OMSetRenderTargetsAndUAVsFn g_origOMSetRenderTargetsAndUAVs = nullptr;
thread_local bool g_inVrsHook = false;

void STDMETHODCALLTYPE Hooked_OMSetRenderTargets(
    ID3D11DeviceContext* self,
    UINT numViews,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView) {
    if (g_origOMSetRenderTargets) {
        g_origOMSetRenderTargets(self, numViews, ppRenderTargetViews, pDepthStencilView);
    }
    if (g_inVrsHook || !self) return;
    g_inVrsHook = true;
    VrsFoveatedD3D11::Instance().OnPostOMSetRenderTargets(self, numViews, ppRenderTargetViews);
    g_inVrsHook = false;
}

void STDMETHODCALLTYPE Hooked_OMSetRenderTargetsAndUAVs(
    ID3D11DeviceContext* self,
    UINT numRTVs,
    ID3D11RenderTargetView* const* ppRenderTargetViews,
    ID3D11DepthStencilView* pDepthStencilView,
    UINT uavStartSlot,
    UINT numUAVs,
    ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    if (g_origOMSetRenderTargetsAndUAVs) {
        g_origOMSetRenderTargetsAndUAVs(self, numRTVs, ppRenderTargetViews, pDepthStencilView,
                                        uavStartSlot, numUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    }
    if (g_inVrsHook || !self) return;
    g_inVrsHook = true;
    if (numRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        VrsFoveatedD3D11::Instance().OnPostOMSetRenderTargets(self, numRTVs, ppRenderTargetViews);
    }
    g_inVrsHook = false;
}

uint8_t DistanceToVRSLevel(float distance) {
    // Anillo 1 (0% a 60%): 1x1 Nativo (coincide con el radio de NIS 0.60)
    if (distance < 0.60f) {
        return 0;
    }
    // Anillo 2 (60% a 100%): 1/2x hasta tocar los 4 bordes del cuadro
    if (distance < 1.00f) {
        return 1;
    }
    // Anillo 3 (> 100%, las 4 esquinas exteriores): 1/16x (4x4)
    return 2;
}

bool ResolutionMatches(int actualSize, int targetSize) {
    if (targetSize <= 0) return false;
    return actualSize >= (targetSize - 4) && actualSize <= (targetSize + 4);
}

std::vector<uint8_t> CreateSingleEyePattern(int width, int height, float projX, float projY) {
    std::vector<uint8_t> data(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(width);
            const float fy = static_cast<float>(y) / static_cast<float>(height);
            const float distance = 2.0f * std::sqrt((fx - projX) * (fx - projX) + (fy - projY) * (fy - projY));
            data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = DistanceToVRSLevel(distance);
        }
    }
    return data;
}

std::vector<uint8_t> CreateCombinedPattern(int width, int height, float leftProjX, float leftProjY, float rightProjX, float rightProjY) {
    std::vector<uint8_t> data(static_cast<size_t>(width) * static_cast<size_t>(height));
    const int halfWidth = (std::max)(1, width / 2);

    for (int y = 0; y < height; ++y) {
        const float fy = static_cast<float>(y) / static_cast<float>(height);
        for (int x = 0; x < halfWidth; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(halfWidth);
            const float distance = 2.0f * std::sqrt((fx - leftProjX) * (fx - leftProjX) + (fy - leftProjY) * (fy - leftProjY));
            data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = DistanceToVRSLevel(distance);
        }
        for (int x = halfWidth; x < width; ++x) {
            const float fx = static_cast<float>(x - halfWidth) / static_cast<float>(halfWidth);
            const float distance = 2.0f * std::sqrt((fx - rightProjX) * (fx - rightProjX) + (fy - rightProjY) * (fy - rightProjY));
            data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = DistanceToVRSLevel(distance);
        }
    }
    return data;
}

} // namespace

VrsFoveatedD3D11& VrsFoveatedD3D11::Instance() {
    static VrsFoveatedD3D11 s_instance;
    return s_instance;
}

bool VrsFoveatedD3D11::EnsureInitializedLocked(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!dev || !ctx) return false;

    if (device_.Get() != dev) {
        for (int i = 0; i < 2; ++i) {
            singleEyeVRSTex_[i].Reset();
            singleEyeVRSView_[i].Reset();
            singleWidth_[i] = 0;
            singleHeight_[i] = 0;
        }
        combinedVRSTex_.Reset();
        combinedVRSView_.Reset();
        combinedWidth_ = 0;
        combinedHeight_ = 0;
        arrayVRSTex_.Reset();
        arrayVRSView_.Reset();
        arrayWidth_ = 0;
        arrayHeight_ = 0;
        vrsCurrentlyBound_ = false;

        device_ = dev;
        context_ = ctx;

        if (!nvapiLoaded_) {
            if (NvAPI_Initialize() != NVAPI_OK) {
                LogMessage(LogLevel::Warn, "VRS: NvAPI_Initialize fallo. Variable Rate Shading no disponible.");
                nvapiSupported_ = false;
                return false;
            }
            nvapiLoaded_ = true;
        }

        NV_D3D1x_GRAPHICS_CAPS caps{};
        NvAPI_Status status = NvAPI_D3D1x_GetGraphicsCapabilities(dev, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
        if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
            LogMessage(LogLevel::Warn, "VRS: La GPU no soporta NVIDIA Variable Pixel Rate Shading en D3D11.");
            nvapiSupported_ = false;
            return false;
        }

        nvapiSupported_ = true;
        LogMessage(LogLevel::Info, "VRS: NVIDIA NVAPI inicializado correctamente (Anillos: 60% 1x, 100% 1/2x, >100% 1/16x).");
    }

    if (nvapiSupported_ && !hooksInstalled_) {
        InstallContextHooksLocked(ctx);
    }

    return nvapiSupported_;
}

void VrsFoveatedD3D11::InstallContextHooksLocked(ID3D11DeviceContext* ctx) {
    if (hooksInstalled_ || !ctx) return;

    void** vtbl = *reinterpret_cast<void***>(ctx);
    if (!vtbl) return;

    MH_Initialize();

    OMSetRenderTargetsFn origOM = nullptr;
    if (MH_CreateHook(vtbl[33], reinterpret_cast<void*>(&Hooked_OMSetRenderTargets), reinterpret_cast<void**>(&origOM)) == MH_OK) {
        MH_EnableHook(vtbl[33]);
        g_origOMSetRenderTargets = origOM;
    }

    OMSetRenderTargetsAndUAVsFn origOMUAV = nullptr;
    if (MH_CreateHook(vtbl[34], reinterpret_cast<void*>(&Hooked_OMSetRenderTargetsAndUAVs), reinterpret_cast<void**>(&origOMUAV)) == MH_OK) {
        MH_EnableHook(vtbl[34]);
        g_origOMSetRenderTargetsAndUAVs = origOMUAV;
    }

    hooksInstalled_ = true;
    LogMessage(LogLevel::Info, "VRS: Hooks ID3D11DeviceContext instalados (OMSetRenderTargets vtbl[33], vtbl[34]).");
}

void VrsFoveatedD3D11::UpdateTarget(ID3D11Device* device,
                                    ID3D11DeviceContext* context,
                                    bool enabled,
                                    uint32_t eyeWidth,
                                    uint32_t eyeHeight,
                                    VrsTextureMode mode,
                                    float leftProjX,
                                    float leftProjY,
                                    float rightProjX,
                                    float rightProjY) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;

    if (!enabled) {
        if (vrsCurrentlyBound_ && context_) {
            DisableVRS(context_.Get());
        }
        return;
    }

    if (!EnsureInitializedLocked(device, context)) {
        return;
    }

    targetEyeWidth_ = static_cast<int>(eyeWidth);
    targetEyeHeight_ = static_cast<int>(eyeHeight);
    targetMode_ = mode;
    proj_[0][0] = leftProjX;
    proj_[0][1] = leftProjY;
    proj_[1][0] = rightProjX;
    proj_[1][1] = rightProjY;
}

void VrsFoveatedD3D11::EndFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentSingleEyeRT_ > 0) {
        if (currentSingleEyeRT_ != singleEyeOrder_.size()) {
            singleEyeOrder_.clear();
            const size_t half = currentSingleEyeRT_ / 2;
            for (size_t i = 0; i < half; ++i) {
                singleEyeOrder_.push_back('L');
            }
            for (size_t i = 0; i < half; ++i) {
                singleEyeOrder_.push_back('R');
            }
            while (singleEyeOrder_.size() < currentSingleEyeRT_) {
                singleEyeOrder_.push_back('L');
            }
        }
    }
    currentSingleEyeRT_ = 0;
}

void VrsFoveatedD3D11::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vrsCurrentlyBound_ && context_) {
        DisableVRS(context_.Get());
    }
    for (int i = 0; i < 2; ++i) {
        singleEyeVRSTex_[i].Reset();
        singleEyeVRSView_[i].Reset();
        singleWidth_[i] = 0;
        singleHeight_[i] = 0;
    }
    combinedVRSTex_.Reset();
    combinedVRSView_.Reset();
    combinedWidth_ = 0;
    combinedHeight_ = 0;
    arrayVRSTex_.Reset();
    arrayVRSView_.Reset();
    arrayWidth_ = 0;
    arrayHeight_ = 0;
    device_.Reset();
    context_.Reset();
    enabled_ = false;
}

void VrsFoveatedD3D11::OnPostOMSetRenderTargets(ID3D11DeviceContext* ctx,
                                                UINT numViews,
                                                ID3D11RenderTargetView* const* renderTargetViews) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!nvapiSupported_ || !device_ || !ctx) {
        return;
    }

    if (!enabled_ || numViews == 0 || !renderTargetViews || !renderTargetViews[0] || targetEyeWidth_ <= 0 || targetEyeHeight_ <= 0) {
        DisableVRS(ctx);
        return;
    }

    D3D11_RENDER_TARGET_VIEW_DESC rtd{};
    renderTargetViews[0]->GetDesc(&rtd);
    if (rtd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
        rtd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
        rtd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS &&
        rtd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY) {
        DisableVRS(ctx);
        return;
    }

    ComPtr<ID3D11Resource> resource;
    renderTargetViews[0]->GetResource(resource.GetAddressOf());
    if (!resource) {
        DisableVRS(ctx);
        return;
    }

    auto* tex = static_cast<ID3D11Texture2D*>(resource.Get());
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // Ignorar shadow maps cuadrados
    if (td.Width == td.Height) {
        DisableVRS(ctx);
        return;
    }

    const int w = static_cast<int>(td.Width);
    const int h = static_cast<int>(td.Height);

    if (ResolutionMatches(w, targetEyeWidth_ * 2) && ResolutionMatches(h, targetEyeHeight_)) {
        ApplyCombinedVRS(ctx, w, h);
    } else if (td.ArraySize == 2 && ResolutionMatches(w, targetEyeWidth_) && ResolutionMatches(h, targetEyeHeight_)) {
        ApplyArrayVRS(ctx, w, h);
    } else if (td.ArraySize == 1 && ResolutionMatches(w, targetEyeWidth_) && ResolutionMatches(h, targetEyeHeight_)) {
        int eye = 0;
        if (currentSingleEyeRT_ < singleEyeOrder_.size()) {
            const char c = singleEyeOrder_[currentSingleEyeRT_];
            eye = (c == 'R' || c == 'r') ? 1 : 0;
        } else {
            eye = static_cast<int>(currentSingleEyeRT_ & 1);
        }
        ++currentSingleEyeRT_;
        ApplySingleEyeVRS(ctx, eye, w, h);
    } else {
        DisableVRS(ctx);
    }
}

void VrsFoveatedD3D11::EnableVRS(ID3D11DeviceContext* ctx) {
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[2]{};
    for (int i = 0; i < 2; ++i) {
        vsrd[i].enableVariablePixelShadingRate = true;
        std::memset(vsrd[i].shadingRateTable, 0, sizeof(vsrd[i].shadingRateTable));
        vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
        vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
    }

    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd{};
    srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    srd.numViewports = 2;
    srd.pViewports = vsrd;

    if (NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &srd) == NVAPI_OK) {
        vrsCurrentlyBound_ = true;
    }
}

void VrsFoveatedD3D11::DisableVRS(ID3D11DeviceContext* ctx) {
    if (!vrsCurrentlyBound_ || !ctx) {
        return;
    }

    NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[2]{};
    vsrd[0].enableVariablePixelShadingRate = false;
    vsrd[1].enableVariablePixelShadingRate = false;

    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd{};
    srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    srd.numViewports = 2;
    srd.pViewports = vsrd;

    NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &srd);
    NvAPI_D3D11_RSSetShadingRateResourceView(ctx, nullptr);
    vrsCurrentlyBound_ = false;
}

void VrsFoveatedD3D11::ApplySingleEyeVRS(ID3D11DeviceContext* ctx, int eye, int width, int height) {
    SetupSingleEyeVRS(eye, width, height, proj_[eye][0], proj_[eye][1]);
    if (!singleEyeVRSView_[eye]) return;
    if (NvAPI_D3D11_RSSetShadingRateResourceView(ctx, singleEyeVRSView_[eye].Get()) == NVAPI_OK) {
        EnableVRS(ctx);
    }
}

void VrsFoveatedD3D11::ApplyCombinedVRS(ID3D11DeviceContext* ctx, int width, int height) {
    SetupCombinedVRS(width, height, proj_[0][0], proj_[0][1], proj_[1][0], proj_[1][1]);
    if (!combinedVRSView_) return;
    if (NvAPI_D3D11_RSSetShadingRateResourceView(ctx, combinedVRSView_.Get()) == NVAPI_OK) {
        EnableVRS(ctx);
    }
}

void VrsFoveatedD3D11::ApplyArrayVRS(ID3D11DeviceContext* ctx, int width, int height) {
    SetupArrayVRS(width, height, proj_[0][0], proj_[0][1], proj_[1][0], proj_[1][1]);
    if (!arrayVRSView_) return;
    if (NvAPI_D3D11_RSSetShadingRateResourceView(ctx, arrayVRSView_.Get()) == NVAPI_OK) {
        EnableVRS(ctx);
    }
}

void VrsFoveatedD3D11::SetupSingleEyeVRS(int eye, int width, int height, float projX, float projY) {
    const int vrsWidth = (std::max)(1, width / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
    const int vrsHeight = (std::max)(1, height / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

    if (singleEyeVRSView_[eye] && vrsWidth == singleWidth_[eye] && vrsHeight == singleHeight_[eye]) {
        return;
    }

    singleEyeVRSTex_[eye].Reset();
    singleEyeVRSView_[eye].Reset();
    singleWidth_[eye] = vrsWidth;
    singleHeight_[eye] = vrsHeight;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(vrsWidth);
    td.Height = static_cast<UINT>(vrsHeight);
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MipLevels = 1;

    auto data = CreateSingleEyePattern(vrsWidth, vrsHeight, projX, projY);
    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = data.data();
    srd.SysMemPitch = static_cast<UINT>(vrsWidth);

    if (FAILED(device_->CreateTexture2D(&td, &srd, singleEyeVRSTex_[eye].GetAddressOf()))) {
        return;
    }

    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = td.Format;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;

    if (NvAPI_D3D11_CreateShadingRateResourceView(device_.Get(), singleEyeVRSTex_[eye].Get(), &vd, singleEyeVRSView_[eye].GetAddressOf()) == NVAPI_OK) {
        LogMessage(LogLevel::Info, "VRS: Textura SingleEye [" + std::to_string(eye) + "] creada (" +
                   std::to_string(vrsWidth) + "x" + std::to_string(vrsHeight) + " tiles para " +
                   std::to_string(width) + "x" + std::to_string(height) + ").");
    }
}

void VrsFoveatedD3D11::SetupCombinedVRS(int width, int height, float leftProjX, float leftProjY, float rightProjX, float rightProjY) {
    int vrsWidth = (std::max)(2, width / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
    if (vrsWidth & 1) ++vrsWidth;
    int vrsHeight = (std::max)(2, height / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);
    if (vrsHeight & 1) ++vrsHeight;

    if (combinedVRSView_ && vrsWidth == combinedWidth_ && vrsHeight == combinedHeight_) {
        return;
    }

    combinedVRSTex_.Reset();
    combinedVRSView_.Reset();
    combinedWidth_ = vrsWidth;
    combinedHeight_ = vrsHeight;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(vrsWidth);
    td.Height = static_cast<UINT>(vrsHeight);
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MipLevels = 1;

    auto data = CreateCombinedPattern(vrsWidth, vrsHeight, leftProjX, leftProjY, rightProjX, rightProjY);
    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem = data.data();
    srd.SysMemPitch = static_cast<UINT>(vrsWidth);

    if (FAILED(device_->CreateTexture2D(&td, &srd, combinedVRSTex_.GetAddressOf()))) {
        return;
    }

    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = td.Format;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;

    if (NvAPI_D3D11_CreateShadingRateResourceView(device_.Get(), combinedVRSTex_.Get(), &vd, combinedVRSView_.GetAddressOf()) == NVAPI_OK) {
        LogMessage(LogLevel::Info, "VRS: Textura Combined Side-by-Side creada (" +
                   std::to_string(vrsWidth) + "x" + std::to_string(vrsHeight) + " tiles para " +
                   std::to_string(width) + "x" + std::to_string(height) + ").");
    }
}

void VrsFoveatedD3D11::SetupArrayVRS(int width, int height, float leftProjX, float leftProjY, float rightProjX, float rightProjY) {
    const int vrsWidth = (std::max)(1, width / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
    const int vrsHeight = (std::max)(1, height / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

    if (arrayVRSView_ && vrsWidth == arrayWidth_ && vrsHeight == arrayHeight_) {
        return;
    }

    arrayVRSTex_.Reset();
    arrayVRSView_.Reset();
    arrayWidth_ = vrsWidth;
    arrayHeight_ = vrsHeight;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(vrsWidth);
    td.Height = static_cast<UINT>(vrsHeight);
    td.ArraySize = 2;
    td.Format = DXGI_FORMAT_R8_UINT;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MipLevels = 1;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, arrayVRSTex_.GetAddressOf()))) {
        return;
    }

    auto dataLeft = CreateSingleEyePattern(vrsWidth, vrsHeight, leftProjX, 1.0f - leftProjY);
    context_->UpdateSubresource(arrayVRSTex_.Get(), D3D11CalcSubresource(0, 0, 1), nullptr, dataLeft.data(), static_cast<UINT>(vrsWidth), 0);

    auto dataRight = CreateSingleEyePattern(vrsWidth, vrsHeight, rightProjX, 1.0f - rightProjY);
    context_->UpdateSubresource(arrayVRSTex_.Get(), D3D11CalcSubresource(0, 1, 1), nullptr, dataRight.data(), static_cast<UINT>(vrsWidth), 0);

    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = td.Format;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2DARRAY;
    vd.Texture2DArray.MipSlice = 0;
    vd.Texture2DArray.ArraySize = 2;
    vd.Texture2DArray.FirstArraySlice = 0;

    if (NvAPI_D3D11_CreateShadingRateResourceView(device_.Get(), arrayVRSTex_.Get(), &vd, arrayVRSView_.GetAddressOf()) == NVAPI_OK) {
        LogMessage(LogLevel::Info, "VRS: Textura ArraySize=2 creada (" +
                   std::to_string(vrsWidth) + "x" + std::to_string(vrsHeight) + " tiles para " +
                   std::to_string(width) + "x" + std::to_string(height) + ").");
    }
}

} // namespace vrpacing

#endif