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
    uint32_t appWidth = 0;
    uint32_t appHeight = 0;
    uint32_t realWidth = 0;
    uint32_t realHeight = 0;
};

struct SwapchainProxy {
    XrSwapchain real = XR_NULL_HANDLE;
    uint32_t appWidth = 0;
    uint32_t appHeight = 0;
    uint32_t realWidth = 0;
    uint32_t realHeight = 0;
    uint32_t arraySize = 1;
    std::vector<ComPtr<ID3D11Texture2D>> realTextures;
    std::vector<ComPtr<ID3D11Texture2D>> appTextures;
    std::vector<XrSwapchainImageD3D11KHR> appImages;
    std::queue<uint32_t> acquiredIndices;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, SwapchainProxy> g_swapchains;

ComPtr<ID3D11Device> g_d3d11Device;
ComPtr<ID3D11DeviceContext> g_d3d11Context;
NisUpscalerD3D11 g_upscaler;

FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
RuntimeConfig g_cfg = DefaultConfig();
bool g_init = false;

void EnsureInit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_init) return;
    InitLogging();
    g_cfg = LoadConfig();
    g_limiter.Initialize();
    g_init = true;
}

bool GetSwapchainDimensions(XrSwapchain swapchain, SwapchainDimensions* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(reinterpret_cast<uint64_t>(swapchain));
    if (it == g_swapchains.end()) return false;
    out->appWidth = it->second.appWidth;
    out->appHeight = it->second.appHeight;
    out->realWidth = it->second.realWidth;
    out->realHeight = it->second.realHeight;
    return true;
}

XrResult XRAPI_CALL Impl_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    EnsureInit();
    if (!createInfo || !session) return XR_ERROR_VALIDATION_FAILURE;

    const auto* header = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (header) {
        if (header->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(header);
            if (binding->device) {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_d3d11Device = binding->device;
                g_d3d11Device->GetImmediateContext(g_d3d11Context.ReleaseAndGetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    g_upscaler.Initialize(g_d3d11Device.Get());
                }
            }
            break;
        }
        header = reinterpret_cast<const XrBaseInStructure*>(header->next);
    }

    return g_nextCreateSession(instance, createInfo, session);
}

XrResult XRAPI_CALL Impl_xrDestroySession(XrSession session) {
    if (g_nextDestroySession) {
        return g_nextDestroySession(session);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type,
                                                          uint32_t capacity, uint32_t* count, XrViewConfigurationView* views) {
    return g_nextEnumerateViewConfigurationViews(instance, systemId, type, capacity, count, views);
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainFormats(XrSession session, uint32_t capacity, uint32_t* count, int64_t* formats) {
    return g_nextEnumerateSwapchainFormats(session, capacity, count, formats);
}

XrResult XRAPI_CALL Impl_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    EnsureInit();
    if (!createInfo || !swapchain) return XR_ERROR_VALIDATION_FAILURE;

    const bool isColor = (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                         (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    const bool shouldUpscale = g_cfg.upscaling.enabled && isColor && (g_cfg.upscaling.scale > 1.0f);

    if (!shouldUpscale) {
        return g_nextCreateSwapchain(session, createInfo, swapchain);
    }

    XrSwapchainCreateInfo realInfo = *createInfo;
    const double s = g_cfg.upscaling.scale;
    realInfo.width = static_cast<uint32_t>(std::max(1.0, std::floor(createInfo->width * s)));
    realInfo.height = static_cast<uint32_t>(std::max(1.0, std::floor(createInfo->height * s)));

    const uint64_t px = static_cast<uint64_t>(realInfo.width) * static_cast<uint64_t>(realInfo.height);
    if (px > g_cfg.upscaling.maxOutputPixels) {
        const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(px));
        realInfo.width = static_cast<uint32_t>(std::max(1.0, std::floor(realInfo.width * factor)));
        realInfo.height = static_cast<uint32_t>(std::max(1.0, std::floor(realInfo.height * factor)));
    }

    realInfo.usageFlags |= XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;

    XrSwapchain real = XR_NULL_HANDLE;
    XrResult res = g_nextCreateSwapchain(session, &realInfo, &real);
    if (XR_FAILED(res)) return res;

    uint32_t realCount = 0;
    res = g_nextEnumerateSwapchainImages(real, 0, &realCount, nullptr);
    if (XR_FAILED(res) || realCount == 0) {
        g_nextDestroySwapchain(real);
        return res;
    }

    std::vector<XrSwapchainImageD3D11KHR> realImages(realCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    res = g_nextEnumerateSwapchainImages(
        real,
        realCount,
        &realCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(realImages.data())
    );
    if (XR_FAILED(res)) {
        g_nextDestroySwapchain(real);
        return res;
    }

    if (!g_d3d11Device && realImages[0].texture) {
        realImages[0].texture->GetDevice(g_d3d11Device.ReleaseAndGetAddressOf());
        if (g_d3d11Device) {
            g_d3d11Device->GetImmediateContext(g_d3d11Context.ReleaseAndGetAddressOf());
            if (!g_upscaler.IsInitialized()) {
                g_upscaler.Initialize(g_d3d11Device.Get());
            }
        }
    }

    if (!g_d3d11Device) {
        LogMessage(LogLevel::Error, "Impl_xrCreateSwapchain: ID3D11Device no disponible para crear texturas proxy.");
        *swapchain = real;
        return XR_SUCCESS;
    }

    D3D11_TEXTURE2D_DESC td{};
    realImages[0].texture->GetDesc(&td);

    D3D11_TEXTURE2D_DESC appDesc = td;
    appDesc.Width = createInfo->width;
    appDesc.Height = createInfo->height;
    appDesc.MipLevels = createInfo->mipCount > 0 ? createInfo->mipCount : 1;
    appDesc.ArraySize = createInfo->arraySize > 0 ? createInfo->arraySize : td.ArraySize;
    appDesc.SampleDesc.Count = createInfo->sampleCount > 0 ? createInfo->sampleCount : 1;
    appDesc.SampleDesc.Quality = 0;
    appDesc.Usage = D3D11_USAGE_DEFAULT;
    appDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) {
        appDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    }
    appDesc.CPUAccessFlags = 0;
    appDesc.MiscFlags = 0;

    SwapchainProxy proxy{};
    proxy.real = real;
    proxy.appWidth = createInfo->width;
    proxy.appHeight = createInfo->height;
    proxy.realWidth = realInfo.width;
    proxy.realHeight = realInfo.height;
    proxy.arraySize = appDesc.ArraySize;

    proxy.realTextures.reserve(realCount);
    proxy.appTextures.reserve(realCount);
    proxy.appImages.resize(realCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});

    for (uint32_t i = 0; i < realCount; ++i) {
        proxy.realTextures.push_back(realImages[i].texture);

        ComPtr<ID3D11Texture2D> appTex;
        HRESULT hr = g_d3d11Device->CreateTexture2D(&appDesc, nullptr, appTex.GetAddressOf());
        if (FAILED(hr)) {
            LogMessage(LogLevel::Error, "Impl_xrCreateSwapchain: Error al crear textura intermedia D3D11.");
            g_nextDestroySwapchain(real);
            return XR_ERROR_RUNTIME_FAILURE;
        }

        proxy.appTextures.push_back(appTex);
        proxy.appImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        proxy.appImages[i].next = nullptr;
        proxy.appImages[i].texture = appTex.Get();
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_swapchains[reinterpret_cast<uint64_t>(real)] = std::move(proxy);
    }

    *swapchain = real;
    LogMessage(LogLevel::Info, "OpenXR Swapchain proxy creado: " +
               std::to_string(createInfo->width) + "x" + std::to_string(createInfo->height) +
               " -> " + std::to_string(realInfo.width) + "x" + std::to_string(realInfo.height));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(reinterpret_cast<uint64_t>(swapchain));
    if (it == g_swapchains.end()) {
        return g_nextEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
    }

    const auto& proxy = it->second;
    const uint32_t count = static_cast<uint32_t>(proxy.appImages.size());

    if (imageCountOutput) {
        *imageCountOutput = count;
    }

    if (imageCapacityInput == 0) {
        return XR_SUCCESS;
    }

    if (imageCapacityInput < count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    if (!images) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    auto* outD3D = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < count; ++i) {
        outD3D[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        outD3D[i].next = nullptr;
        outD3D[i].texture = proxy.appTextures[i].Get();
    }

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    XrResult res = g_nextAcquireSwapchainImage(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(res) && index) {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(swapchain));
        if (it != g_swapchains.end()) {
            it->second.acquiredIndices.push(*index);
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    return g_nextWaitSwapchainImage(swapchain, waitInfo);
}

XrResult XRAPI_CALL Impl_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    uint32_t index = 0;
    bool isScaled = false;
    ComPtr<ID3D11Texture2D> srcTex;
    ComPtr<ID3D11Texture2D> dstTex;
    uint32_t arraySize = 1;
    uint32_t appWidth = 0;
    uint32_t appHeight = 0;
    uint32_t realWidth = 0;
    uint32_t realHeight = 0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(swapchain));
        if (it != g_swapchains.end() && !it->second.acquiredIndices.empty()) {
            isScaled = true;
            index = it->second.acquiredIndices.front();
            it->second.acquiredIndices.pop();

            if (index < it->second.appTextures.size() && index < it->second.realTextures.size()) {
                srcTex = it->second.appTextures[index];
                dstTex = it->second.realTextures[index];
                arraySize = it->second.arraySize;
                appWidth = it->second.appWidth;
                appHeight = it->second.appHeight;
                realWidth = it->second.realWidth;
                realHeight = it->second.realHeight;
            }
        }
    }

    if (isScaled && srcTex && dstTex && g_d3d11Context) {
        UpscaleDispatchDesc desc{};
        desc.srcRect = { 0, 0, appWidth, appHeight };
        desc.dstRect = { 0, 0, realWidth, realHeight };
        desc.projCenterX = 0.5f;
        desc.projCenterY = 0.5f;
        desc.radius = g_cfg.upscaling.radius;
        desc.sharpness = g_cfg.upscaling.sharpness;
        desc.maskEnabled = g_cfg.mask.enabled;
        desc.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

        if (arraySize > 1) {
            for (uint32_t slice = 0; slice < arraySize; ++slice) {
                desc.srcArrayIndex = static_cast<int>(slice);
                desc.dstArrayIndex = static_cast<int>(slice);
                g_upscaler.Upscale(g_d3d11Context.Get(), srcTex.Get(), dstTex.Get(), desc);
            }
        } else {
            desc.srcArrayIndex = -1;
            desc.dstArrayIndex = -1;
            g_upscaler.Upscale(g_d3d11Context.Get(), srcTex.Get(), dstTex.Get(), desc);
        }
    }

    return g_nextReleaseSwapchainImage(swapchain, releaseInfo);
}

XrResult XRAPI_CALL Impl_xrDestroySwapchain(XrSwapchain swapchain) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_swapchains.erase(reinterpret_cast<uint64_t>(swapchain));
    }
    return g_nextDestroySwapchain(swapchain);
}

XrResult XRAPI_CALL Impl_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    EnsureInit();

    if (g_cfg.pacing.enabled) {
        const auto hz = g_hzTracker.GetEffectiveHz().value_or(90.0);
        LimitInputs lim{};
        lim.hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        g_limiter.SetTargetFps(vrpacing::ComputeTargetFps(lim));
        g_limiter.WaitBeforeRuntime();
    }

    const XrResult res = g_nextWaitFrame(session, info, state);
    if (XR_SUCCEEDED(res) && state && state->predictedDisplayPeriod > 0) {
        g_hzTracker.PushPeriodNs(state->predictedDisplayPeriod);
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (!frameEndInfo || frameEndInfo->layerCount == 0 || !frameEndInfo->layers) {
        return g_nextEndFrame(session, frameEndInfo);
    }

    std::vector<const XrCompositionLayerBaseHeader*> remapped(frameEndInfo->layerCount);
    std::vector<XrCompositionLayerProjection> projCopies;
    std::vector<std::vector<XrCompositionLayerProjectionView>> viewCopies;

    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = frameEndInfo->layers[i];
        if (base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            projCopies.push_back(*proj);
            viewCopies.emplace_back(proj->views, proj->views + proj->viewCount);

            auto& views = viewCopies.back();
            for (auto& v : views) {
                SwapchainDimensions dims{};
                if (GetSwapchainDimensions(v.subImage.swapchain, &dims)) {
                    const double sx = dims.appWidth ? static_cast<double>(dims.realWidth) / dims.appWidth : 1.0;
                    const double sy = dims.appHeight ? static_cast<double>(dims.realHeight) / dims.appHeight : 1.0;
                    v.subImage.imageRect.offset.x = static_cast<int32_t>(std::lround(v.subImage.imageRect.offset.x * sx));
                    v.subImage.imageRect.offset.y = static_cast<int32_t>(std::lround(v.subImage.imageRect.offset.y * sy));
                    v.subImage.imageRect.extent.width = static_cast<int32_t>(std::lround(v.subImage.imageRect.extent.width * sx));
                    v.subImage.imageRect.extent.height = static_cast<int32_t>(std::lround(v.subImage.imageRect.extent.height * sy));
                }
            }

            projCopies.back().views = views.data();
            remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projCopies.back());
        } else {
            remapped[i] = base;
        }
    }

    XrFrameEndInfo copy = *frameEndInfo;
    copy.layers = remapped.data();
    return g_nextEndFrame(session, &copy);
}

XrResult XRAPI_CALL Impl_xrGetVisibilityMaskKHR(XrSession session, XrViewConfigurationType viewConfigurationType, uint32_t viewIndex,
                                                 XrVisibilityMaskTypeKHR visibilityMaskType, XrVisibilityMaskKHR* visibilityMask) {
    EnsureInit();
    if (!visibilityMask || !g_cfg.mask.enabled || visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR) {
        if (g_nextGetVisibilityMaskKHR) {
            return g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
        }
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    EllipseMaskParams p{};
    p.centerX = g_cfg.mask.centerX;
    p.centerY = g_cfg.mask.centerY;
    p.radiusX = g_cfg.mask.radiusX;
    p.radiusY = g_cfg.mask.radiusY;
    p.segments = g_cfg.mask.segments;
    p.flipWinding = g_cfg.mask.flipWinding;

    const auto mesh = BuildEllipseOutsideMaskIndexed(p);
    visibilityMask->vertexCountOutput = static_cast<uint32_t>(mesh.vertices.size());
    visibilityMask->indexCountOutput = static_cast<uint32_t>(mesh.indices.size());
    if (visibilityMask->vertexCapacityInput == 0 || visibilityMask->indexCapacityInput == 0) return XR_SUCCESS;

    const uint32_t vCopy = (std::min)(visibilityMask->vertexCapacityInput, visibilityMask->vertexCountOutput);
    const uint32_t iCopy = (std::min)(visibilityMask->indexCapacityInput, visibilityMask->indexCountOutput);
    for (uint32_t i = 0; i < vCopy; ++i) {
        visibilityMask->vertices[i].x = mesh.vertices[i].x;
        visibilityMask->vertices[i].y = mesh.vertices[i].y;
    }
    for (uint32_t i = 0; i < iCopy; ++i) {
        visibilityMask->indices[i] = mesh.indices[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties) {
    return g_nextGetSystemProperties(instance, systemId, properties);
}

XrResult XRAPI_CALL Impl_xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput,
                                                               uint32_t* propertyCountOutput, XrExtensionProperties* properties) {
    XrResult res = g_nextEnumerateInstanceExtensionProperties(layerName, propertyCapacityInput, propertyCountOutput, properties);
    if (XR_FAILED(res) || !propertyCountOutput) return res;

    bool hasVisMask = false;
    if (properties) {
        for (uint32_t i = 0; i < *propertyCountOutput; ++i) {
            if (strcmp(properties[i].extensionName, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) == 0) {
                hasVisMask = true;
                break;
            }
        }
    }

    if (!hasVisMask) {
        uint32_t oldCount = *propertyCountOutput;
        *propertyCountOutput = oldCount + 1;
        if (properties && propertyCapacityInput > oldCount) {
            auto& e = properties[oldCount];
            e.type = XR_TYPE_EXTENSION_PROPERTIES;
            e.next = nullptr;
            strcpy_s(e.extensionName, XR_MAX_EXTENSION_NAME_SIZE, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
            e.extensionVersion = XR_KHR_visibility_mask_SPEC_VERSION;
        }
    }
    return XR_SUCCESS;
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

#endif