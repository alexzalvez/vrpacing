#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "vrpacing/hz_tracker.h"
#include "vrpacing/mask_geometry.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {
namespace {

PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrDestroySession g_nextDestroySession = nullptr;
PFN_xrEnumerateViewConfigurationViews g_nextEnumerateViewConfigurationViews = nullptr;
PFN_xrCreateSwapchain g_nextCreateSwapchain = nullptr;
PFN_xrDestroySwapchain g_nextDestroySwapchain = nullptr;
PFN_xrEnumerateSwapchainFormats g_nextEnumerateSwapchainFormats = nullptr;
PFN_xrEnumerateSwapchainImages g_nextEnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_nextWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_nextReleaseSwapchainImage = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
PFN_xrWaitFrame g_nextWaitFrame = nullptr;
PFN_xrGetVisibilityMaskKHR g_nextGetVisibilityMaskKHR = nullptr;
PFN_xrGetSystemProperties g_nextGetSystemProperties = nullptr;
PFN_xrEnumerateInstanceExtensionProperties g_nextEnumerateInstanceExtensionProperties = nullptr;

struct SwapchainDimensions {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ProxySwapchain {
    XrSwapchain realSwapchain = XR_NULL_HANDLE;
    std::vector<ComPtr<ID3D11Texture2D>> appTextures;
    std::vector<ComPtr<ID3D11Texture2D>> realTextures;
    std::queue<uint32_t> acquiredIndices;
    uint32_t appWidth = 0;
    uint32_t appHeight = 0;
    uint32_t realWidth = 0;
    uint32_t realHeight = 0;
    uint32_t arraySize = 1;
    int64_t format = 0;
};

std::mutex g_layerMutex;
std::unordered_map<XrSwapchain, ProxySwapchain> g_swapchains;
std::unordered_map<XrSwapchain, SwapchainDimensions> g_chainDims;

RuntimeConfig g_cfg;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;

ComPtr<ID3D11Device> g_d3d11Device;
ComPtr<ID3D11DeviceContext> g_d3d11Context;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenXR Layer inicializada.");
    });
}

XrResult XRAPI_CALL Impl_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    EnsureInit();
    if (!createInfo || !session) return XR_ERROR_VALIDATION_FAILURE;

    const auto* next = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* d3d = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            if (d3d->device) {
                g_d3d11Device = d3d->device;
                g_d3d11Device->GetImmediateContext(g_d3d11Context.ReleaseAndGetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    if (!g_upscaler.Initialize(g_d3d11Device.Get())) {
                        g_upscaleStatus = -2;
                    }
                }
                LogMessage(LogLevel::Info, "OpenXR Layer: ID3D11Device capturado en xrCreateSession.");
            }
            break;
        }
        next = next->next;
    }

    return g_nextCreateSession(instance, createInfo, session);
}

XrResult XRAPI_CALL Impl_xrDestroySession(XrSession session) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains.clear();
    g_chainDims.clear();
    return g_nextDestroySession(session);
}

XrResult XRAPI_CALL Impl_xrEnumerateViewConfigurationViews(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrViewConfigurationView* views) {
    EnsureInit();
    XrResult res = g_nextEnumerateViewConfigurationViews(instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
    if (XR_FAILED(res) || !views || viewCapacityInput == 0) return res;

    g_shm.SyncConfig(g_cfg);

    static std::atomic<bool> s_viewsLogged{ false };
    if (!s_viewsLogged.exchange(true) && viewCountOutput) {
        for (uint32_t i = 0; i < *viewCountOutput; ++i) {
            LogMessage(LogLevel::Info, "OpenXR ViewConfig[" + std::to_string(i) + "]: Nativo=" +
                       std::to_string(views[i].recommendedImageRectWidth) + "x" +
                       std::to_string(views[i].recommendedImageRectHeight));
        }
    }

    return res;
}

XrResult XRAPI_CALL Impl_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    EnsureInit();
    if (!createInfo || !swapchain) return XR_ERROR_VALIDATION_FAILURE;

    g_shm.SyncConfig(g_cfg);

    const bool isColor = (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                         (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;

    if (!isColor || !g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        XrResult r = g_nextCreateSwapchain(session, createInfo, swapchain);
        if (XR_SUCCEEDED(r)) {
            std::lock_guard<std::mutex> lock(g_layerMutex);
            g_chainDims[*swapchain] = { createInfo->width, createInfo->height };
        }
        return r;
    }

    const uint32_t appW = createInfo->width;
    const uint32_t appH = createInfo->height;
    const uint32_t arrSize = createInfo->arraySize ? createInfo->arraySize : 1;
    const double s = g_cfg.upscaling.scale;
    uint32_t realW = static_cast<uint32_t>((std::max)(1.0, std::floor(appW * s)));
    uint32_t realH = static_cast<uint32_t>((std::max)(1.0, std::floor(appH * s)));

    const uint64_t px = static_cast<uint64_t>(realW) * static_cast<uint64_t>(realH);
    if (px > g_cfg.upscaling.maxOutputPixels) {
        const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(px));
        realW = static_cast<uint32_t>((std::max)(1.0, std::floor(realW * factor)));
        realH = static_cast<uint32_t>((std::max)(1.0, std::floor(realH * factor)));
    }

    g_lastInW = appW;
    g_lastInH = appH;
    g_lastOutW = realW;
    g_lastOutH = realH;
    g_lastFormat = static_cast<uint32_t>(createInfo->format);

    XrSwapchainCreateInfo realDesc = *createInfo;
    realDesc.width = realW;
    realDesc.height = realH;
    realDesc.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;

    if (realDesc.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        realDesc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else if (realDesc.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        realDesc.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    XrSwapchain real = XR_NULL_HANDLE;
    XrResult r = g_nextCreateSwapchain(session, &realDesc, &real);
    if (XR_FAILED(r)) {
        g_upscaleStatus = -1;
        g_lastHResult = r;
        LogMessage(LogLevel::Error, "OpenXR Layer: Error al crear swapchain nativo escalado.");
        return r;
    }

    if (!g_d3d11Device) {
        *swapchain = real;
        return XR_SUCCESS;
    }

    D3D11_TEXTURE2D_DESC appDesc{};
    appDesc.Width = appW;
    appDesc.Height = appH;
    appDesc.MipLevels = 1;
    appDesc.ArraySize = arrSize;
    appDesc.Format = static_cast<DXGI_FORMAT>(createInfo->format);
    appDesc.SampleDesc.Count = createInfo->sampleCount ? createInfo->sampleCount : 1;
    appDesc.Usage = D3D11_USAGE_DEFAULT;
    appDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (isColor && (appDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || appDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM)) {
        appDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    } else if (isColor && (appDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || appDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM)) {
        appDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }

    uint32_t imgCount = 0;
    g_nextEnumerateSwapchainImages(real, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> realImgs(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    g_nextEnumerateSwapchainImages(real, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(realImgs.data()));

    ProxySwapchain proxy{};
    proxy.realSwapchain = real;
    proxy.appWidth = appW;
    proxy.appHeight = appH;
    proxy.realWidth = realW;
    proxy.realHeight = realH;
    proxy.arraySize = arrSize;
    proxy.format = createInfo->format;

    for (uint32_t i = 0; i < imgCount; ++i) {
        proxy.realTextures.push_back(realImgs[i].texture);

        ComPtr<ID3D11Texture2D> appTex;
        HRESULT hr = g_d3d11Device->CreateTexture2D(&appDesc, nullptr, appTex.GetAddressOf());
        if (FAILED(hr)) {
            g_upscaleStatus = -1;
            g_lastHResult = hr;
            LogMessage(LogLevel::Error, "OpenXR Layer: Error al crear textura intermedia D3D11.");
            g_nextDestroySwapchain(real);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        proxy.appTextures.push_back(appTex);
    }

    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains[real] = std::move(proxy);
    g_chainDims[real] = { appW, appH };
    *swapchain = real;

    LogMessage(LogLevel::Info, "OpenXR Proxy Swapchain creado: " +
               std::to_string(appW) + "x" + std::to_string(appH) +
               " (ArraySize=" + std::to_string(arrSize) + ") -> " +
               std::to_string(realW) + "x" + std::to_string(realH));

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrDestroySwapchain(XrSwapchain swapchain) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains.erase(swapchain);
    g_chainDims.erase(swapchain);
    return g_nextDestroySwapchain(swapchain);
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput, uint32_t* formatCountOutput, int64_t* formats) {
    return g_nextEnumerateSwapchainFormats(session, formatCapacityInput, formatCountOutput, formats);
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
    }

    const auto& proxy = it->second;
    const uint32_t count = static_cast<uint32_t>(proxy.appTextures.size());
    if (imageCountOutput) *imageCountOutput = count;
    if (imageCapacityInput == 0 || !images) return XR_SUCCESS;

    auto* d3dImgs = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < (std::min)(imageCapacityInput, count); ++i) {
        d3dImgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        d3dImgs[i].texture = proxy.appTextures[i].Get();
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextAcquireSwapchainImage(swapchain, acquireInfo, index);
    }

    uint32_t realIdx = 0;
    XrResult r = g_nextAcquireSwapchainImage(it->second.realSwapchain, acquireInfo, &realIdx);
    if (XR_SUCCEEDED(r) && index) {
        *index = realIdx;
        it->second.acquiredIndices.push(realIdx);
    }
    return r;
}

XrResult XRAPI_CALL Impl_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextWaitSwapchainImage(swapchain, waitInfo);
    }
    return g_nextWaitSwapchainImage(it->second.realSwapchain, waitInfo);
}

XrResult XRAPI_CALL Impl_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextReleaseSwapchainImage(swapchain, releaseInfo);
    }

    auto& proxy = it->second;
    if (proxy.acquiredIndices.empty()) {
        return g_nextReleaseSwapchainImage(proxy.realSwapchain, releaseInfo);
    }

    const uint32_t idx = proxy.acquiredIndices.front();
    proxy.acquiredIndices.pop();

    if (g_d3d11Context && idx < proxy.appTextures.size() && idx < proxy.realTextures.size()) {
        const uint32_t numSlices = (proxy.arraySize > 0) ? proxy.arraySize : 1;

        static std::atomic<bool> s_firstFrameLogged{ false };
        if (!s_firstFrameLogged.exchange(true)) {
            LogMessage(LogLevel::Info, "OpenXR [Release] Primer frame: App=" +
                       std::to_string(proxy.appWidth) + "x" + std::to_string(proxy.appHeight) +
                       " ArraySize=" + std::to_string(numSlices) +
                       " FMT=" + std::to_string(proxy.format) +
                       " -> Real=" + std::to_string(proxy.realWidth) + "x" + std::to_string(proxy.realHeight) +
                       " Scale=" + std::to_string(g_cfg.upscaling.scale));
        }

        bool allSlicesOk = true;
        for (uint32_t slice = 0; slice < numSlices; ++slice) {
            UpscaleDispatchDesc desc{};
            desc.srcRect = { 0, 0, proxy.appWidth, proxy.appHeight };
            desc.dstRect = { 0, 0, proxy.realWidth, proxy.realHeight };
            desc.srcArrayIndex = static_cast<int>(slice);
            desc.dstArrayIndex = static_cast<int>(slice);
            desc.projCenterX = 0.5f;
            desc.projCenterY = 0.5f;
            desc.radius = g_cfg.upscaling.radius;
            desc.sharpness = g_cfg.upscaling.sharpness;
            desc.maskEnabled = g_cfg.mask.enabled;
            desc.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

            if (!g_upscaler.Upscale(g_d3d11Context.Get(), proxy.appTextures[idx].Get(), proxy.realTextures[idx].Get(), desc)) {
                allSlicesOk = false;
            }
        }

        g_upscaleStatus = allSlicesOk ? 1 : -3;
    }

    return g_nextReleaseSwapchainImage(proxy.realSwapchain, releaseInfo);
}

XrResult XRAPI_CALL Impl_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    const auto hz = g_hzTracker.GetEffectiveHz().value_or(90.0);
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled) {
        LimitInputs lim{};
        lim.hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        g_limiter.WaitBeforeRuntime();
    }

    g_shm.UpdateTelemetry(L"OpenXR (VDXR)", hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);

    XrResult res = g_nextWaitFrame(session, info, state);
    if (XR_SUCCEEDED(res) && state && state->predictedDisplayPeriod > 0) {
        const double measuredHz = 1e9 / static_cast<double>(state->predictedDisplayPeriod);
        if (measuredHz > 30.0 && measuredHz < 200.0) {
            g_hzTracker.PushHz(std::round(measuredHz));
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (!frameEndInfo || frameEndInfo->layerCount == 0 || !frameEndInfo->layers) {
        return g_nextEndFrame(session, frameEndInfo);
    }

    std::vector<const XrCompositionLayerBaseHeader*> remapped(frameEndInfo->layerCount);
    std::vector<XrCompositionLayerProjection> projCopies;
    projCopies.reserve(frameEndInfo->layerCount);

    std::lock_guard<std::mutex> lock(g_layerMutex);

    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const auto* layer = frameEndInfo->layers[i];
        if (!layer) continue;

        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            XrCompositionLayerProjection copy = *proj;

            for (uint32_t v = 0; v < copy.viewCount; ++v) {
                auto& view = const_cast<XrCompositionLayerProjectionView&>(copy.views[v]);
                auto it = g_swapchains.find(view.subImage.swapchain);
                if (it != g_swapchains.end()) {
                    const auto& p = it->second;
                    const double sx = static_cast<double>(p.realWidth) / p.appWidth;
                    const double sy = static_cast<double>(p.realHeight) / p.appHeight;

                    view.subImage.imageRect.offset.x = static_cast<int32_t>(std::lround(view.subImage.imageRect.offset.x * sx));
                    view.subImage.imageRect.offset.y = static_cast<int32_t>(std::lround(view.subImage.imageRect.offset.y * sy));
                    view.subImage.imageRect.extent.width = static_cast<int32_t>(std::lround(view.subImage.imageRect.extent.width * sx));
                    view.subImage.imageRect.extent.height = static_cast<int32_t>(std::lround(view.subImage.imageRect.extent.height * sy));
                }
            }
            projCopies.push_back(copy);
            remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projCopies.back());
        } else {
            remapped[i] = layer;
        }
    }

    XrFrameEndInfo modified = *frameEndInfo;
    modified.layers = remapped.data();
    return g_nextEndFrame(session, &modified);
}

XrResult XRAPI_CALL Impl_xrGetVisibilityMaskKHR(
    XrSession session,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewIndex,
    XrVisibilityMaskTypeKHR visibilityMaskType,
    XrVisibilityMaskKHR* visibilityMask) {
    EnsureInit();
    XrResult r = g_nextGetVisibilityMaskKHR ? g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(r) || !visibilityMask || !g_cfg.mask.enabled) return r;

    if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR) {
        EllipseMaskParams params{};
        params.radiusX = g_cfg.upscaling.radius;
        params.radiusY = g_cfg.upscaling.radius;
        params.segments = 32;
        const IndexedMesh mesh = BuildEllipseOutsideMaskIndexed(params);

        visibilityMask->vertexCountOutput = static_cast<uint32_t>(mesh.vertices.size());
        visibilityMask->indexCountOutput = static_cast<uint32_t>(mesh.indices.size());

        if (visibilityMask->vertexCapacityInput >= mesh.vertices.size() && visibilityMask->vertices) {
            for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                visibilityMask->vertices[i].x = mesh.vertices[i].x;
                visibilityMask->vertices[i].y = mesh.vertices[i].y;
            }
        }
        if (visibilityMask->indexCapacityInput >= mesh.indices.size() && visibilityMask->indices) {
            const size_t bytes = mesh.indices.size() * sizeof(uint32_t);
            std::memcpy(visibilityMask->indices, mesh.indices.data(), bytes);
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties) {
    return g_nextGetSystemProperties(instance, systemId, properties);
}

XrResult XRAPI_CALL Impl_xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrExtensionProperties* properties) {
    return g_nextEnumerateInstanceExtensionProperties(layerName, propertyCapacityInput, propertyCountOutput, properties);
}

void ResolveProcPointers(XrInstance instance) {
    g_nextGetInstanceProcAddr(instance, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSession));
    g_nextGetInstanceProcAddr(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySession));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateViewConfigurationViews));
    g_nextGetInstanceProcAddr(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSwapchain));
    g_nextGetInstanceProcAddr(instance, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySwapchain));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateSwapchainFormats));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateSwapchainImages));
    g_nextGetInstanceProcAddr(instance, "xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextAcquireSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextWaitSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextReleaseSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEndFrame));
    g_nextGetInstanceProcAddr(instance, "xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextWaitFrame));
    g_nextGetInstanceProcAddr(instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextGetVisibilityMaskKHR));
    g_nextGetInstanceProcAddr(instance, "xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextGetSystemProperties));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateInstanceExtensionProperties));
}

} // namespace

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;

    if (strcmp(name, "xrCreateSession") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrCreateSession);
    else if (strcmp(name, "xrDestroySession") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroySession);
    else if (strcmp(name, "xrEnumerateViewConfigurationViews") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateViewConfigurationViews);
    else if (strcmp(name, "xrCreateSwapchain") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrCreateSwapchain);
    else if (strcmp(name, "xrDestroySwapchain") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroySwapchain);
    else if (strcmp(name, "xrEnumerateSwapchainFormats") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateSwapchainFormats);
    else if (strcmp(name, "xrEnumerateSwapchainImages") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateSwapchainImages);
    else if (strcmp(name, "xrAcquireSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrAcquireSwapchainImage);
    else if (strcmp(name, "xrWaitSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrWaitSwapchainImage);
    else if (strcmp(name, "xrReleaseSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrReleaseSwapchainImage);
    else if (strcmp(name, "xrEndFrame") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEndFrame);
    else if (strcmp(name, "xrWaitFrame") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrWaitFrame);
    else if (strcmp(name, "xrGetVisibilityMaskKHR") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetVisibilityMaskKHR);
    else if (strcmp(name, "xrGetSystemProperties") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetSystemProperties);
    else if (strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateInstanceExtensionProperties);
    else return g_nextGetInstanceProcAddr(instance, name, function);

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                              const XrApiLayerCreateInfo* layerInfo,
                                                              XrInstance* instance) {
    auto nextInfo = layerInfo ? layerInfo->nextInfo : nullptr;
    if (!nextInfo || !nextInfo->nextGetInstanceProcAddr || !nextInfo->nextCreateApiLayerInstance) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    g_nextGetInstanceProcAddr = nextInfo->nextGetInstanceProcAddr;
    XrResult res = nextInfo->nextCreateApiLayerInstance(info, layerInfo, instance);
    if (XR_FAILED(res)) return res;

    ResolveProcPointers(*instance);
    return XR_SUCCESS;
}

} // namespace vrpacing

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    const char* layerName,
    XrNegotiateApiLayerRequest* apiLayerRequest) {
    if (!loaderInfo || !apiLayerRequest) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (apiLayerRequest->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        apiLayerRequest->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
        apiLayerRequest->structSize != sizeof(XrNegotiateApiLayerRequest)) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = vrpacing::Layer_xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = vrpacing::Layer_xrCreateApiLayerInstance;

    vrpacing::InitLogging();
    vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR Layer negociada exitosamente con el runtime.");

    return XR_SUCCESS;
}

#endif