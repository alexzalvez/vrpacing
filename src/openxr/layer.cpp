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

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "vrpacing/hz_tracker.h"
#include "vrpacing/mask_geometry.h"

namespace vrpacing {
namespace {

PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrEnumerateViewConfigurationViews g_nextEnumerateViewConfigurationViews = nullptr;
PFN_xrCreateSwapchain g_nextCreateSwapchain = nullptr;
PFN_xrEnumerateSwapchainFormats g_nextEnumerateSwapchainFormats = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_nextWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_nextReleaseSwapchainImage = nullptr;
PFN_xrDestroySwapchain g_nextDestroySwapchain = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
PFN_xrWaitFrame g_nextWaitFrame = nullptr;
PFN_xrGetVisibilityMaskKHR g_nextGetVisibilityMaskKHR = nullptr;
PFN_xrGetSystemProperties g_nextGetSystemProperties = nullptr;
PFN_xrEnumerateInstanceExtensionProperties g_nextEnumerateInstanceExtensionProperties = nullptr;

struct SwapchainProxy {
    XrSwapchain real = XR_NULL_HANDLE;
    uint32_t appWidth = 0;
    uint32_t appHeight = 0;
    uint32_t realWidth = 0;
    uint32_t realHeight = 0;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, SwapchainProxy> g_swapchains;
std::atomic<uint64_t> g_fakeSwapchainCounter{0x7000000000000000ull};

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

XrSwapchain ResolveRealSwapchain(XrSwapchain maybeFake) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(reinterpret_cast<uint64_t>(maybeFake));
    if (it == g_swapchains.end()) {
        return maybeFake;
    }
    return it->second.real;
}

bool GetProxy(XrSwapchain maybeFake, SwapchainProxy* out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_swapchains.find(reinterpret_cast<uint64_t>(maybeFake));
    if (it == g_swapchains.end()) return false;
    *out = it->second;
    return true;
}

XrResult XRAPI_CALL Impl_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    EnsureInit();
    return g_nextCreateSession(instance, createInfo, session);
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

    XrSwapchainCreateInfo realInfo = *createInfo;
    if (g_cfg.upscaling.enabled) {
        const double s = (g_cfg.upscaling.scale > 1.0f) ? g_cfg.upscaling.scale : 1.0f;
        realInfo.width = static_cast<uint32_t>(std::max(1.0, std::floor(createInfo->width * s)));
        realInfo.height = static_cast<uint32_t>(std::max(1.0, std::floor(createInfo->height * s)));

        const uint64_t px = static_cast<uint64_t>(realInfo.width) * static_cast<uint64_t>(realInfo.height);
        if (px > g_cfg.upscaling.maxOutputPixels) {
            const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(px));
            realInfo.width = static_cast<uint32_t>(std::max(1.0, std::floor(realInfo.width * factor)));
            realInfo.height = static_cast<uint32_t>(std::max(1.0, std::floor(realInfo.height * factor)));
        }
    }

    XrSwapchain real = XR_NULL_HANDLE;
    XrResult res = g_nextCreateSwapchain(session, &realInfo, &real);
    if (XR_FAILED(res)) return res;

    const XrSwapchain fake = reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(g_fakeSwapchainCounter.fetch_add(1)));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_swapchains[reinterpret_cast<uint64_t>(fake)] = {real, createInfo->width, createInfo->height, realInfo.width, realInfo.height};
    }
    *swapchain = fake;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    return g_nextAcquireSwapchainImage(ResolveRealSwapchain(swapchain), acquireInfo, index);
}

XrResult XRAPI_CALL Impl_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    return g_nextWaitSwapchainImage(ResolveRealSwapchain(swapchain), waitInfo);
}

XrResult XRAPI_CALL Impl_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    // Inserción de NIS en ruta D3D11 (staging -> real) aquí.
    return g_nextReleaseSwapchainImage(ResolveRealSwapchain(swapchain), releaseInfo);
}

XrResult XRAPI_CALL Impl_xrDestroySwapchain(XrSwapchain swapchain) {
    XrSwapchain real = swapchain;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_swapchains.find(reinterpret_cast<uint64_t>(swapchain));
        if (it != g_swapchains.end()) {
            real = it->second.real;
            g_swapchains.erase(it);
        }
    }
    return g_nextDestroySwapchain(real);
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
                SwapchainProxy proxy{};
                if (GetProxy(v.subImage.swapchain, &proxy)) {
                    const double sx = proxy.appWidth ? static_cast<double>(proxy.realWidth) / proxy.appWidth : 1.0;
                    const double sy = proxy.appHeight ? static_cast<double>(proxy.realHeight) / proxy.appHeight : 1.0;
                    v.subImage.swapchain = proxy.real;
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
    g_nextGetInstanceProcAddr(instance, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateViewConfigurationViews));
    g_nextGetInstanceProcAddr(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSwapchain));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateSwapchainFormats));
    g_nextGetInstanceProcAddr(instance, "xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextAcquireSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextWaitSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextReleaseSwapchainImage));
    g_nextGetInstanceProcAddr(instance, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySwapchain));
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
    else if (strcmp(name, "xrEnumerateViewConfigurationViews") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateViewConfigurationViews);
    else if (strcmp(name, "xrCreateSwapchain") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrCreateSwapchain);
    else if (strcmp(name, "xrEnumerateSwapchainFormats") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateSwapchainFormats);
    else if (strcmp(name, "xrAcquireSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrAcquireSwapchainImage);
    else if (strcmp(name, "xrWaitSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrWaitSwapchainImage);
    else if (strcmp(name, "xrReleaseSwapchainImage") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrReleaseSwapchainImage);
    else if (strcmp(name, "xrDestroySwapchain") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroySwapchain);
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

extern "C" {

XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;

    if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(xrGetInstanceProcAddr);
        return XR_SUCCESS;
    }
    if (strcmp(name, "xrCreateApiLayerInstance") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(vrpacing::Layer_xrCreateApiLayerInstance);
        return XR_SUCCESS;
    }
    return vrpacing::Layer_xrGetInstanceProcAddr(instance, name, function);
}

XRAPI_ATTR XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                        const XrApiLayerCreateInfo* layerInfo,
                                                        XrInstance* instance) {
    return vrpacing::Layer_xrCreateApiLayerInstance(info, layerInfo, instance);
}

XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                  const char* /*layerName*/,
                                                                  XrNegotiateApiLayerRequest* apiLayerRequest) {
    if (!loaderInfo || !apiLayerRequest) return XR_ERROR_INITIALIZATION_FAILED;

    apiLayerRequest->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    apiLayerRequest->layerApiVersion = XR_CURRENT_API_VERSION;
    apiLayerRequest->getInstanceProcAddr = xrGetInstanceProcAddr;
    apiLayerRequest->createApiLayerInstance = xrCreateApiLayerInstance;
    return XR_SUCCESS;
}

}

#endif
