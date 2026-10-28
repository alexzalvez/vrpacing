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
#include <cwctype>
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
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_openXrD3D11Active{ false };

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
PFN_xrGetSystemProperties g_nextGetSystemProperties = nullptr;
PFN_xrEnumerateInstanceExtensionProperties g_nextEnumerateInstanceExtensionProperties = nullptr;
PFN_xrGetVisibilityMaskKHR g_nextGetVisibilityMaskKHR = nullptr;
PFN_xrLocateViews g_nextLocateViews = nullptr;
PFN_xrPollEvent g_nextPollEvent = nullptr;
PFN_xrCreateReferenceSpace g_nextCreateReferenceSpace = nullptr;
PFN_xrDestroySpace g_nextDestroySpace = nullptr;

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

struct EyeTangents {
    float minX = -0.85f;
    float maxX = 0.85f;
    float minY = -0.85f;
    float maxY = 0.85f;
    bool valid = false;
};

XrSession g_activeSession = XR_NULL_HANDLE;
EyeTangents g_eyeTangents[2];
std::atomic<bool> g_pendingMaskEvent[2]{ false, false };
std::atomic<XrTime> g_lastPredictedDisplayTime{ 1 };

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
uint32_t g_nativeViewW = 0;
uint32_t g_nativeViewH = 0;

bool IsCurrentProcessBlacklisted() {
    static int s_cached = -1;
    if (s_cached != -1) return (s_cached == 1);

    wchar_t modPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modPath, MAX_PATH);
    std::wstring p(modPath);
    const auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(pos + 1);
    std::transform(p.begin(), p.end(), p.begin(), ::towlower);

    if (p == L"cemu.exe" || p == L"bettervr_launcher.exe" || p == L"ofxrbridgetray.exe" || p == L"ofxrbridge.exe") {
        s_cached = 1;
        return true;
    }
    s_cached = 0;
    return false;
}

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenXR Layer inicializada.");
    });
}

XrResult XRAPI_CALL Impl_xrGetVisibilityMaskKHR(
    XrSession session,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewIndex,
    XrVisibilityMaskTypeKHR visibilityMaskType,
    XrVisibilityMaskKHR* visibilityMask) {
    EnsureInit();
    if (!visibilityMask) return XR_ERROR_VALIDATION_FAILURE;

    g_shm.SyncConfig(g_cfg);
    if (!g_cfg.mask.enabled || IsCurrentProcessBlacklisted()) {
        if (g_nextGetVisibilityMaskKHR) {
            return g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
        }
        visibilityMask->vertexCountOutput = 0;
        visibilityMask->indexCountOutput = 0;
        return XR_SUCCESS;
    }

    constexpr uint32_t kSeg = 256;
    uint32_t reqVerts = 0;
    uint32_t reqIndices = 0;

    if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR) {
        reqVerts = kSeg + 1;
        reqIndices = kSeg * 6; // Doble cara (CCW + CW)
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR) {
        reqVerts = kSeg * 2;
        reqIndices = kSeg * 12; // Doble cara (CCW + CW)
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR) {
        reqVerts = kSeg;
        reqIndices = kSeg;
    } else {
        visibilityMask->vertexCountOutput = 0;
        visibilityMask->indexCountOutput = 0;
        return XR_SUCCESS;
    }

    visibilityMask->vertexCountOutput = reqVerts;
    visibilityMask->indexCountOutput = reqIndices;

    if (visibilityMask->vertexCapacityInput == 0 || visibilityMask->indexCapacityInput == 0 ||
        !visibilityMask->vertices || !visibilityMask->indices) {
        return XR_SUCCESS;
    }

    if (visibilityMask->vertexCapacityInput < reqVerts || visibilityMask->indexCapacityInput < reqIndices) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    float minX = -0.85f, maxX = 0.85f, minY = -0.85f, maxY = 0.85f;
    {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        if (viewIndex < 2 && g_eyeTangents[viewIndex].valid) {
            minX = g_eyeTangents[viewIndex].minX;
            maxX = g_eyeTangents[viewIndex].maxX;
            minY = g_eyeTangents[viewIndex].minY;
            maxY = g_eyeTangents[viewIndex].maxY;
        } else if (session != XR_NULL_HANDLE && g_nextCreateReferenceSpace && g_nextLocateViews && g_nextDestroySpace) {
            XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
            XrSpace viewSpace = XR_NULL_HANDLE;
            if (XR_SUCCEEDED(g_nextCreateReferenceSpace(session, &spaceInfo, &viewSpace)) && viewSpace != XR_NULL_HANDLE) {
                XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
                locateInfo.viewConfigurationType = viewConfigurationType;
                locateInfo.displayTime = g_lastPredictedDisplayTime.load(std::memory_order_relaxed);
                locateInfo.space = viewSpace;

                XrViewState viewState{ XR_TYPE_VIEW_STATE };
                uint32_t viewCount = 0;
                XrView tmpViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
                if (XR_SUCCEEDED(g_nextLocateViews(session, &locateInfo, &viewState, 2, &viewCount, tmpViews))) {
                    for (uint32_t v = 0; v < (std::min)(viewCount, 2u); ++v) {
                        const float l = std::tan(tmpViews[v].fov.angleLeft);
                        const float r = std::tan(tmpViews[v].fov.angleRight);
                        const float d = std::tan(tmpViews[v].fov.angleDown);
                        const float u = std::tan(tmpViews[v].fov.angleUp);
                        if ((r - l) > 0.1f && (u - d) > 0.1f) {
                            g_eyeTangents[v].minX = l;
                            g_eyeTangents[v].maxX = r;
                            g_eyeTangents[v].minY = d;
                            g_eyeTangents[v].maxY = u;
                            g_eyeTangents[v].valid = true;
                        }
                    }
                    if (viewIndex < 2 && g_eyeTangents[viewIndex].valid) {
                        minX = g_eyeTangents[viewIndex].minX;
                        maxX = g_eyeTangents[viewIndex].maxX;
                        minY = g_eyeTangents[viewIndex].minY;
                        maxY = g_eyeTangents[viewIndex].maxY;
                    }
                }
                g_nextDestroySpace(viewSpace);
            }
        }
    }

    const float cx = 0.5f * (minX + maxX);
    const float cy = 0.5f * (minY + maxY);
    const float rx = 0.5f * (maxX - minX);
    const float ry = 0.5f * (maxY - minY);
    constexpr float kTwoPi = 6.283185307179586f;

    if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR) {
        visibilityMask->vertices[0] = { cx, cy };
        for (uint32_t i = 0; i < kSeg; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(kSeg)) * kTwoPi;
            visibilityMask->vertices[i + 1] = { cx + rx * std::cos(a), cy + ry * std::sin(a) };
            const uint32_t next = ((i + 1) % kSeg) + 1;
            const uint32_t base = i * 6;
            // CCW
            visibilityMask->indices[base + 0] = 0;
            visibilityMask->indices[base + 1] = i + 1;
            visibilityMask->indices[base + 2] = next;
            // CW
            visibilityMask->indices[base + 3] = 0;
            visibilityMask->indices[base + 4] = next;
            visibilityMask->indices[base + 5] = i + 1;
        }
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR) {
        for (uint32_t i = 0; i < kSeg; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(kSeg)) * kTwoPi;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            const float maxComp = (std::max)(std::abs(ca), std::abs(sa));
            const float outerScale = (maxComp > 0.0001f) ? (2.0f / maxComp) : 2.5f;

            visibilityMask->vertices[i * 2 + 0] = { cx + rx * ca, cy + ry * sa };
            visibilityMask->vertices[i * 2 + 1] = { cx + rx * outerScale * ca, cy + ry * outerScale * sa };

            const uint32_t next = (i + 1) % kSeg;
            const uint32_t i0 = i * 2;
            const uint32_t i1 = i * 2 + 1;
            const uint32_t n0 = next * 2;
            const uint32_t n1 = next * 2 + 1;
            const uint32_t base = i * 12;

            // CCW
            visibilityMask->indices[base + 0] = i0;
            visibilityMask->indices[base + 1] = i1;
            visibilityMask->indices[base + 2] = n0;
            visibilityMask->indices[base + 3] = n0;
            visibilityMask->indices[base + 4] = i1;
            visibilityMask->indices[base + 5] = n1;
            // CW
            visibilityMask->indices[base + 6] = i0;
            visibilityMask->indices[base + 7] = n0;
            visibilityMask->indices[base + 8] = i1;
            visibilityMask->indices[base + 9] = n0;
            visibilityMask->indices[base + 10] = n1;
            visibilityMask->indices[base + 11] = i1;
        }
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR) {
        for (uint32_t i = 0; i < kSeg; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(kSeg)) * kTwoPi;
            visibilityMask->vertices[i] = { cx + rx * std::cos(a), cy + ry * std::sin(a) };
            visibilityMask->indices[i] = i;
        }
    }

    LogMessage(LogLevel::Info, "OpenXR Layer: Pre-Render Circular Visibility Mask (r=1.00, 256 seg, CW+CCW) aplicada al ojo " +
               std::to_string(viewIndex) + " Type=" + std::to_string(visibilityMaskType) +
               " Tangents=[" + std::to_string(minX) + ", " + std::to_string(maxX) + ", " +
               std::to_string(minY) + ", " + std::to_string(maxY) + "]");

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrLocateViews(
    XrSession session,
    const XrViewLocateInfo* viewLocateInfo,
    XrViewState* viewState,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrView* views) {
    XrResult res = g_nextLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(res) && views && viewCountOutput) {
        const uint32_t count = (std::min)(*viewCountOutput, 2u);
        for (uint32_t i = 0; i < count; ++i) {
            const float l = std::tan(views[i].fov.angleLeft);
            const float r = std::tan(views[i].fov.angleRight);
            const float d = std::tan(views[i].fov.angleDown);
            const float u = std::tan(views[i].fov.angleUp);
            if ((r - l) > 0.1f && (u - d) > 0.1f) {
                std::lock_guard<std::mutex> lock(g_layerMutex);
                const bool changed = !g_eyeTangents[i].valid ||
                                     std::abs(g_eyeTangents[i].minX - l) > 0.002f ||
                                     std::abs(g_eyeTangents[i].maxX - r) > 0.002f ||
                                     std::abs(g_eyeTangents[i].minY - d) > 0.002f ||
                                     std::abs(g_eyeTangents[i].maxY - u) > 0.002f;
                g_eyeTangents[i].minX = l;
                g_eyeTangents[i].maxX = r;
                g_eyeTangents[i].minY = d;
                g_eyeTangents[i].maxY = u;
                g_eyeTangents[i].valid = true;
                if (changed && g_cfg.mask.enabled) {
                    g_pendingMaskEvent[i].store(true, std::memory_order_relaxed);
                }
            }
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    if (eventData && g_activeSession != XR_NULL_HANDLE && !IsCurrentProcessBlacklisted()) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            if (g_pendingMaskEvent[eye].exchange(false, std::memory_order_relaxed)) {
                auto* ev = reinterpret_cast<XrEventDataVisibilityMaskChangedKHR*>(eventData);
                ev->type = XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR;
                ev->next = nullptr;
                ev->session = g_activeSession;
                ev->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                ev->viewIndex = eye;
                LogMessage(LogLevel::Info, "OpenXR Layer: Evento XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR enviado para ojo " + std::to_string(eye));
                return XR_SUCCESS;
            }
        }
    }
    return g_nextPollEvent ? g_nextPollEvent(instance, eventData) : XR_EVENT_UNAVAILABLE;
}

XrResult XRAPI_CALL Impl_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    EnsureInit();
    if (!createInfo || !session) return XR_ERROR_VALIDATION_FAILURE;

    g_d3d11Device.Reset();
    g_d3d11Context.Reset();

    const auto* next = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* d3d = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            if (d3d->device) {
                g_d3d11Device = d3d->device;
                g_d3d11Device->GetImmediateContext(g_d3d11Context.ReleaseAndGetAddressOf());
                LogMessage(LogLevel::Info, "OpenXR Layer: ID3D11Device capturado en xrCreateSession.");

                if (!IsCurrentProcessBlacklisted()) {
                    g_openXrD3D11Active.store(true, std::memory_order_relaxed);
                    g_shm.OpenOrCreate();
                    g_shm.SetOpenXrD3D11Pid(GetCurrentProcessId());
                }
            }
            break;
        }
        next = next->next;
    }

    if (!g_d3d11Device) {
        g_openXrD3D11Active.store(false, std::memory_order_relaxed);
        LogMessage(LogLevel::Info, "OpenXR Layer: Sesion no-D3D11 detectada (DX12/Vulkan). Passthrough activo.");
    }

    XrResult res = g_nextCreateSession(instance, createInfo, session);
    if (XR_SUCCEEDED(res) && session) {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        g_activeSession = *session;
        g_eyeTangents[0].valid = false;
        g_eyeTangents[1].valid = false;
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrDestroySession(XrSession session) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains.clear();
    g_openXrD3D11Active.store(false, std::memory_order_relaxed);
    if (g_shm.IsOpenXrD3D11ActiveForCurrentProcess()) {
        g_shm.SetOpenXrD3D11Pid(0);
    }
    if (g_activeSession == session) {
        g_activeSession = XR_NULL_HANDLE;
    }
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

    if (viewCountOutput && *viewCountOutput > 0) {
        g_nativeViewW = views[0].recommendedImageRectWidth;
        g_nativeViewH = views[0].recommendedImageRectHeight;
    }

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

    if (!g_d3d11Device || IsCurrentProcessBlacklisted()) {
        return g_nextCreateSwapchain(session, createInfo, swapchain);
    }

    g_shm.SyncConfig(g_cfg);

    const bool isColor = (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                         (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;

    const bool isEyeResolution = (createInfo->arraySize >= 2) ||
                                 (g_nativeViewW > 0 && createInfo->width >= (g_nativeViewW * 7 / 10)) ||
                                 (createInfo->width >= 1200 && createInfo->height >= 1200);

    if (!isColor || !isEyeResolution || !g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        return g_nextCreateSwapchain(session, createInfo, swapchain);
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

    XrSwapchain real = XR_NULL_HANDLE;
    XrResult r = g_nextCreateSwapchain(session, &realDesc, &real);
    if (XR_FAILED(r)) {
        realDesc.usageFlags = createInfo->usageFlags;
        r = g_nextCreateSwapchain(session, &realDesc, &real);
    }

    if (XR_FAILED(r)) {
        g_upscaleStatus = -1;
        g_lastHResult = r;
        LogMessage(LogLevel::Error, "OpenXR Layer: Error al crear swapchain nativo escalado.");
        return r;
    }

    uint32_t imgCount = 0;
    g_nextEnumerateSwapchainImages(real, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> realImgs(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    g_nextEnumerateSwapchainImages(real, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(realImgs.data()));

    D3D11_TEXTURE2D_DESC appDesc{};
    if (imgCount > 0 && realImgs[0].texture) {
        realImgs[0].texture->GetDesc(&appDesc);
    } else {
        appDesc.Format = static_cast<DXGI_FORMAT>(createInfo->format);
    }
    appDesc.Width = appW;
    appDesc.Height = appH;
    appDesc.ArraySize = arrSize;
    appDesc.SampleDesc.Count = createInfo->sampleCount ? createInfo->sampleCount : 1;
    appDesc.SampleDesc.Quality = 0;
    appDesc.MipLevels = 1;
    appDesc.Usage = D3D11_USAGE_DEFAULT;
    appDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    appDesc.CPUAccessFlags = 0;
    appDesc.MiscFlags = 0;

    if (appDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || appDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        appDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    } else if (appDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || appDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        appDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }

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
            LogMessage(LogLevel::Error, "OpenXR Layer: Error al crear textura intermedia D3D11. HR=" + std::to_string(hr));
            g_nextDestroySwapchain(real);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        proxy.appTextures.push_back(appTex);
    }

    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains[real] = std::move(proxy);
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
    g_upscaler.ClearCache();
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
    if (!g_d3d11Device || IsCurrentProcessBlacklisted()) {
        return g_nextAcquireSwapchainImage(swapchain, acquireInfo, index);
    }

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
    if (!g_d3d11Device || IsCurrentProcessBlacklisted()) {
        return g_nextWaitSwapchainImage(swapchain, waitInfo);
    }

    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextWaitSwapchainImage(swapchain, waitInfo);
    }
    return g_nextWaitSwapchainImage(it->second.realSwapchain, waitInfo);
}

XrResult XRAPI_CALL Impl_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    if (!g_d3d11Device || IsCurrentProcessBlacklisted()) {
        return g_nextReleaseSwapchainImage(swapchain, releaseInfo);
    }

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
            desc.maskEnabled = false;
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

    if (IsCurrentProcessBlacklisted() || !g_d3d11Device) {
        return g_nextWaitFrame(session, info, state);
    }

    g_shm.SyncConfig(g_cfg);

    static int s_lastMaskState = -1;
    const int curMaskState = g_cfg.mask.enabled ? 1 : 0;
    if (s_lastMaskState != -1 && s_lastMaskState != curMaskState) {
        g_pendingMaskEvent[0].store(true, std::memory_order_relaxed);
        g_pendingMaskEvent[1].store(true, std::memory_order_relaxed);
    }
    s_lastMaskState = curMaskState;

    static int64_t s_qpcFreq = 0;
    static int64_t s_lastWaitExitQpc = 0;
    static uint32_t s_calibrationFrames = 0;
    static uint32_t s_sswHalfRateHits = 0;
    static uint32_t s_heavyWaitStreak = 0;
    static double s_panelHz = 90.0;
    static bool s_sswHalfRateActive = false;

    if (s_qpcFreq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s_qpcFreq = f.QuadPart;
    }

    const double effectiveHz = s_sswHalfRateActive ? (s_panelHz * 0.5) : s_panelHz;
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled && s_calibrationFrames >= 45) {
        LimitInputs lim{};
        lim.hz = hz;
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

    LARGE_INTEGER beforeWaitQpc{};
    QueryPerformanceCounter(&beforeWaitQpc);

    XrResult res = g_nextWaitFrame(session, info, state);

    LARGE_INTEGER afterWaitQpc{};
    QueryPerformanceCounter(&afterWaitQpc);

    if (XR_SUCCEEDED(res) && state) {
        if (state->predictedDisplayTime > 0) {
            g_lastPredictedDisplayTime.store(state->predictedDisplayTime, std::memory_order_relaxed);
        }
        if (state->predictedDisplayPeriod > 0) {
            const double rawDisplayHz = 1e9 / static_cast<double>(state->predictedDisplayPeriod);
            if (rawDisplayHz > 20.0 && rawDisplayHz < 200.0) {
                const double snappedPanel = HzTracker::SnapToCommonHz(rawDisplayHz).value_or(std::round(rawDisplayHz));
                if (std::abs(snappedPanel - s_panelHz) > 0.5) {
                    s_panelHz = snappedPanel;
                    s_calibrationFrames = 0;
                    s_sswHalfRateHits = 0;
                    s_sswHalfRateActive = false;
                }
            }
        }
    }

    if (s_qpcFreq > 0) {
        const double waitMs = static_cast<double>(afterWaitQpc.QuadPart - beforeWaitQpc.QuadPart) * 1000.0 / static_cast<double>(s_qpcFreq);

        if (s_lastWaitExitQpc > 0) {
            const double dt = static_cast<double>(afterWaitQpc.QuadPart - s_lastWaitExitQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.004 && dt < 0.050) {
                const double measuredHz = 1.0 / dt;
                if (s_calibrationFrames < 45) {
                    s_calibrationFrames++;
                    const double halfPanel = s_panelHz * 0.5;
                    if (waitMs > 2.0 && std::abs(measuredHz - halfPanel) <= (halfPanel * 0.04)) {
                        s_sswHalfRateHits++;
                    }
                    if (s_calibrationFrames == 45) {
                        s_sswHalfRateActive = (s_sswHalfRateHits >= 20);
                    }
                } else {
                    if (waitMs > 5.0) {
                        s_heavyWaitStreak++;
                        if (s_heavyWaitStreak >= 45) {
                            s_heavyWaitStreak = 0;
                            s_calibrationFrames = 0;
                            s_sswHalfRateHits = 0;
                            s_sswHalfRateActive = false;
                        }
                    } else {
                        s_heavyWaitStreak = 0;
                    }
                }
            }
        }
    }
    s_lastWaitExitQpc = afterWaitQpc.QuadPart;
    return res;
}

XrResult XRAPI_CALL Impl_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (!g_d3d11Device || IsCurrentProcessBlacklisted() || !frameEndInfo || frameEndInfo->layerCount == 0 || !frameEndInfo->layers) {
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

XrResult XRAPI_CALL Impl_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* properties) {
    return g_nextGetSystemProperties(instance, systemId, properties);
}

XrResult XRAPI_CALL Impl_xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrExtensionProperties* properties) {
    return g_nextEnumerateInstanceExtensionProperties(layerName, propertyCapacityInput, propertyCountOutput, properties);
}

void ResolveProcPointers(XrInstance instance) {
    g_nextGetInstanceProcAddr(instance, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSession));
    g_nextDestroySession = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySession));
    g_nextGetInstanceProcAddr(instance, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateViewConfigurationViews));
    g_nextGetInstanceProcAddr(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSwapchain));
    g_nextDestroySwapchain = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySwapchain));
    g_nextEnumerateSwapchainFormats = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainFormats", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateSwapchainFormats));
    g_nextEnumerateSwapchainImages = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrEnumerateSwapchainImages", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateSwapchainImages));
    g_nextAcquireSwapchainImage = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextAcquireSwapchainImage));
    g_nextWaitSwapchainImage = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextWaitSwapchainImage));
    g_nextReleaseSwapchainImage = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextReleaseSwapchainImage));
    g_nextEndFrame = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEndFrame));
    g_nextWaitFrame = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextWaitFrame));
    g_nextGetSystemProperties = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextGetSystemProperties));
    g_nextEnumerateInstanceExtensionProperties = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateInstanceExtensionProperties));
    g_nextGetVisibilityMaskKHR = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrGetVisibilityMaskKHR", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextGetVisibilityMaskKHR));
    g_nextLocateViews = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrLocateViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextLocateViews));
    g_nextPollEvent = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrPollEvent", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextPollEvent));
    g_nextCreateReferenceSpace = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateReferenceSpace));
    g_nextDestroySpace = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySpace));
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
    else if (strcmp(name, "xrGetSystemProperties") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetSystemProperties);
    else if (strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateInstanceExtensionProperties);
    else if (strcmp(name, "xrGetVisibilityMaskKHR") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetVisibilityMaskKHR);
    else if (strcmp(name, "xrLocateViews") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrLocateViews);
    else if (strcmp(name, "xrPollEvent") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrPollEvent);
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

extern "C" __declspec(dllexport) bool __cdecl VRPacingLayer_IsActiveD3D11() {
    return vrpacing::g_openXrD3D11Active.load(std::memory_order_relaxed);
}

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