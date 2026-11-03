#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/config_json.h"
#include "common/frame_limiter.h"
#include "common/hz_tracker.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "common/visibility_mask.h"
#include "gfx/nis_upscaler.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "gfx/vrs_foveated_d3d12.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

namespace {

RuntimeConfig g_cfg;
SharedMemoryClient g_shm;
FrameLimiter g_limiter;

PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrCreateInstance g_nextCreateInstance = nullptr;
PFN_xrDestroyInstance g_nextDestroyInstance = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrDestroySession g_nextDestroySession = nullptr;
PFN_xrCreateSwapchain g_nextCreateSwapchain = nullptr;
PFN_xrDestroySwapchain g_nextDestroySwapchain = nullptr;
PFN_xrEnumerateSwapchainImages g_nextEnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_nextWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_nextReleaseSwapchainImage = nullptr;
PFN_xrWaitFrame g_nextWaitFrame = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
PFN_xrEnumerateViewConfigurationViews g_nextEnumerateViewConfigurationViews = nullptr;
PFN_xrGetVisibilityMaskKHR g_nextGetVisibilityMaskKHR = nullptr;
PFN_xrLocateViews g_nextLocateViews = nullptr;
PFN_xrCreateReferenceSpace g_nextCreateReferenceSpace = nullptr;
PFN_xrDestroySpace g_nextDestroySpace = nullptr;
PFN_xrPollEvent g_nextPollEvent = nullptr;

std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_openXrD3D11Active{ false };
std::atomic<bool> g_vdxrBridgeStandby{ false };
std::atomic<bool> g_vdxrBridgeActive{ false };
std::atomic<bool> g_openXrSessionActive{ false };
std::atomic<uint32_t> g_vdxrWarmupFrames{ 0 };

ComPtr<ID3D11Device> g_d3d11Device;
ComPtr<ID3D11DeviceContext> g_d3d11Context;
ComPtr<ID3D12Device> g_d3d12Device;

struct SwapchainData {
    uint32_t originalW = 0;
    uint32_t originalH = 0;
    uint32_t upscaledW = 0;
    uint32_t upscaledH = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::vector<ComPtr<ID3D11Texture2D>> originalTextures;
    std::vector<ComPtr<ID3D11Texture2D>> upscaledTextures;
    std::vector<ComPtr<ID3D11ShaderResourceView>> srvs;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> uavs;
    NisUpscaler upscaler;
    bool upscalerInitialized = false;
    uint32_t lastAcquiredIndex = 0;
    bool isDepthOrAux = false;
};

std::mutex g_layerMutex;
std::unordered_map<XrSwapchain, SwapchainData> g_swapchains;
std::unordered_map<XrSwapchain, std::vector<XrSwapchainImageD3D11KHR>> g_appReturnedImages;

struct EyeTangents {
    float minX = -1.0f;
    float maxX = 1.0f;
    float minY = -1.0f;
    float maxY = 1.0f;
    bool valid = false;
};
EyeTangents g_eyeTangents[2];

struct CachedMaskMesh {
    std::vector<XrVector2f> vertices;
    std::vector<uint32_t> indices;
    bool valid = false;
};
CachedMaskMesh g_cachedMasks[2];
std::atomic<bool> g_pendingMaskEvent[2]{ false, false };
XrSession g_activeSession = XR_NULL_HANDLE;

std::atomic<int64_t> g_lastPredictedDisplayTime{ 0 };
std::atomic<uint32_t> g_visMaskCallsD3D11{ 0 };

uint32_t g_lastInW = 0, g_lastInH = 0, g_lastOutW = 0, g_lastOutH = 0;
uint32_t g_lastFormat = 0;
std::string g_upscaleStatus = "Idle";
HRESULT g_lastHResult = S_OK;

uint32_t g_vrsTargetEyeW = 0;
uint32_t g_vrsTargetEyeH = 0;
bool g_vrsLastEnabled = false;
int g_vrsLastDebugMode = -1;

bool IsCurrentProcessBlacklisted() {
    static int s_cached = -1;
    if (s_cached != -1) return s_cached == 1;

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const wchar_t* fileName = wcsrchr(exePath, L'\\');
    fileName = fileName ? fileName + 1 : exePath;

    if (_wcsicmp(fileName, L"vrserver.exe") == 0 ||
        _wcsicmp(fileName, L"vrcompositor.exe") == 0 ||
        _wcsicmp(fileName, L"vrmonitor.exe") == 0 ||
        _wcsicmp(fileName, L"VirtualDesktop.Service.exe") == 0 ||
        _wcsicmp(fileName, L"VirtualDesktop.Streamer.exe") == 0 ||
        _wcsicmp(fileName, L"VRPacingTray.exe") == 0) {
        s_cached = 1;
        return true;
    }
    s_cached = 0;
    return false;
}

bool IsOpenCompositeActive() {
    return GetModuleHandleW(L"openvr_api.dll") != nullptr;
}

void EnsureHookDllLoadedForVdxrBridge() {
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&EnsureHookDllLoadedForVdxrBridge), &hSelf)) {
        return;
    }
    wchar_t selfDir[MAX_PATH]{};
    GetModuleFileNameW(hSelf, selfDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(selfDir, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    }
    wchar_t hookDllPath[MAX_PATH]{};
    swprintf_s(hookDllPath, L"%s\\VRPacingHook.dll", selfDir);
    HMODULE hHook = LoadLibraryW(hookDllPath);
    if (hHook) {
        LogMessage(LogLevel::Info, "OpenXR Layer: Loaded VRPacingHook.dll for VDXR Bridge runtime.");
    } else {
        LogMessage(LogLevel::Warn, "OpenXR Layer: Failed to load VRPacingHook.dll for VDXR Bridge runtime.");
    }
}

void EnsureInit() {
    if (!g_initialized.exchange(true)) {
        g_cfg = LoadConfig();
        InitLogging("VRPacingLayer");
        LogMessage(LogLevel::Info, "OpenXR Layer: Initialized.");
        g_shm.OpenOrCreate();
        g_shm.SyncConfig(g_cfg);
    }
}

void QueryFovTangentsFallback(XrSession session, XrViewConfigurationType viewConfigurationType,
                             uint32_t viewIndex, float& minX, float& maxX, float& minY, float& maxY) {
    if (viewIndex < 2 && g_eyeTangents[viewIndex].valid) {
        minX = g_eyeTangents[viewIndex].minX;
        maxX = g_eyeTangents[viewIndex].maxX;
        minY = g_eyeTangents[viewIndex].minY;
        maxY = g_eyeTangents[viewIndex].maxY;
        return;
    }

    if (session != XR_NULL_HANDLE && g_nextCreateReferenceSpace && g_nextLocateViews && g_nextDestroySpace) {
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

XrResult XRAPI_CALL Impl_xrGetVisibilityMaskKHR(
    XrSession session,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewIndex,
    XrVisibilityMaskTypeKHR visibilityMaskType,
    XrVisibilityMaskKHR* visibilityMask) {
    EnsureInit();
    if (!visibilityMask) return XR_ERROR_VALIDATION_FAILURE;

    if (g_d3d12Device) {
        VrsFoveatedD3D12::Instance().RecordVisibilityMaskCall();
    } else {
        g_visMaskCallsD3D11.fetch_add(1, std::memory_order_relaxed);
    }

    g_shm.SyncConfig(g_cfg);

    if (IsCurrentProcessBlacklisted() || IsOpenCompositeActive()) {
        if (IsOpenCompositeActive()) {
            static std::atomic<bool> s_ocLogged{ false };
            if (!s_ocLogged.exchange(true)) {
                LogMessage(LogLevel::Info, "OpenXR Layer: OpenComposite detected (openvr_api.dll). Fixed Foveated Rendering (VRS) and Circular Mask automatically excluded; NIS and Pacing active.");
            }
        }
        if (g_nextGetVisibilityMaskKHR) {
            return g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
        }
        visibilityMask->vertexCountOutput = 0;
        visibilityMask->indexCountOutput = 0;
        return XR_SUCCESS;
    }

    if (!g_cfg.mask.enabled) {
        if (g_nextGetVisibilityMaskKHR) {
            return g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
        }
        visibilityMask->vertexCountOutput = 0;
        visibilityMask->indexCountOutput = 0;
        return XR_SUCCESS;
    }

    const uint32_t eyeIdx = (viewIndex == 0) ? 0 : 1;
    auto& cached = g_cachedMasks[eyeIdx];

    if (!cached.valid) {
        float minX = -1.0f, maxX = 1.0f, minY = -1.0f, maxY = 1.0f;
        QueryFovTangentsFallback(session, viewConfigurationType, viewIndex, minX, maxX, minY, maxY);

        std::vector<MaskVec2> verts;
        std::vector<uint32_t> inds;
        GenerateOpenXrVisibilityMask(
            VisibilityMaskKind::HiddenTriangles,
            minX, maxX, minY, maxY,
            g_cfg.mask.radiusX, g_cfg.mask.radiusY,
            g_cfg.mask.centerX, g_cfg.mask.centerY,
            g_cfg.mask.segments,
            verts, inds
        );

        cached.vertices.resize(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            cached.vertices[i].x = verts[i].x;
            cached.vertices[i].y = verts[i].y;
        }
        cached.indices = inds;
        cached.valid = true;
    }

    visibilityMask->vertexCountOutput = static_cast<uint32_t>(cached.vertices.size());
    visibilityMask->indexCountOutput = static_cast<uint32_t>(cached.indices.size());

    if (visibilityMask->vertexCapacityInput > 0 && visibilityMask->vertices) {
        const uint32_t count = (std::min)(visibilityMask->vertexCapacityInput, visibilityMask->vertexCountOutput);
        std::memcpy(visibilityMask->vertices, cached.vertices.data(), count * sizeof(XrVector2f));
    }
    if (visibilityMask->indexCapacityInput > 0 && visibilityMask->indices) {
        const uint32_t count = (std::min)(visibilityMask->indexCapacityInput, visibilityMask->indexCountOutput);
        std::memcpy(visibilityMask->indices, cached.indices.data(), count * sizeof(uint32_t));
    }

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrEnumerateViewConfigurationViews(
    XrInstance instance,
    XrSystemId systemId,
    XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrViewConfigurationView* views) {
    EnsureInit();
    if (!g_nextEnumerateViewConfigurationViews) return XR_ERROR_HANDLE_INVALID;

    XrResult res = g_nextEnumerateViewConfigurationViews(instance, systemId, viewConfigurationType,
                                                        viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(res) && views && viewCountOutput && !IsCurrentProcessBlacklisted()) {
        g_shm.SyncConfig(g_cfg);
        if (g_cfg.upscaling.enabled && g_cfg.upscaling.scale > 1.0f) {
            for (uint32_t i = 0; i < *viewCountOutput; ++i) {
                const uint32_t origW = views[i].recommendedImageRectWidth;
                const uint32_t origH = views[i].recommendedImageRectHeight;
                const float s = g_cfg.upscaling.scale;
                views[i].recommendedImageRectWidth = static_cast<uint32_t>(std::lround(origW / s));
                views[i].recommendedImageRectHeight = static_cast<uint32_t>(std::lround(origH / s));
            }
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrLocateViews(
    XrSession session,
    const XrViewLocateInfo* viewLocateInfo,
    XrViewState* viewState,
    uint32_t viewCapacityInput,
    uint32_t* viewCountOutput,
    XrView* views) {
    if (!g_nextLocateViews) return XR_ERROR_HANDLE_INVALID;
    XrResult res = g_nextLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
    if (XR_SUCCEEDED(res) && views && viewCountOutput && !IsCurrentProcessBlacklisted() && !IsOpenCompositeActive()) {
        const uint32_t count = (std::min)(*viewCountOutput, 2u);
        for (uint32_t i = 0; i < count; ++i) {
            const float l = std::tan(views[i].fov.angleLeft);
            const float r = std::tan(views[i].fov.angleRight);
            const float d = std::tan(views[i].fov.angleDown);
            const float u = std::tan(views[i].fov.angleUp);

            if ((r - l) > 0.1f && (u - d) > 0.1f) {
                auto& cur = g_eyeTangents[i];
                if (!cur.valid ||
                    std::abs(cur.minX - l) > 0.005f || std::abs(cur.maxX - r) > 0.005f ||
                    std::abs(cur.minY - d) > 0.005f || std::abs(cur.maxY - u) > 0.005f) {
                    cur.minX = l;
                    cur.maxX = r;
                    cur.minY = d;
                    cur.maxY = u;
                    cur.valid = true;
                    g_cachedMasks[i].valid = false;
                    g_pendingMaskEvent[i].store(true, std::memory_order_relaxed);
                }
            }
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) {
    if (eventData && g_activeSession != XR_NULL_HANDLE && !IsCurrentProcessBlacklisted() && !IsOpenCompositeActive()) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            if (g_pendingMaskEvent[eye].exchange(false, std::memory_order_relaxed)) {
                auto* ev = reinterpret_cast<XrEventDataVisibilityMaskChangedKHR*>(eventData);
                ev->type = XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR;
                ev->next = nullptr;
                ev->session = g_activeSession;
                ev->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                ev->viewIndex = eye;
                return XR_SUCCESS;
            }
        }
    }
    return g_nextPollEvent ? g_nextPollEvent(instance, eventData) : XR_EVENT_UNAVAILABLE;
}

XrResult XRAPI_CALL Impl_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session) {
    EnsureInit();
    if (!createInfo || !session || !g_nextCreateSession) return XR_ERROR_VALIDATION_FAILURE;

    g_cfg = LoadConfig();
    g_d3d11Device.Reset();
    g_d3d11Context.Reset();
    g_d3d12Device.Reset();
    g_vdxrBridgeStandby.store(false, std::memory_order_relaxed);
    g_vdxrBridgeActive.store(false, std::memory_order_relaxed);
    g_vdxrWarmupFrames.store(0, std::memory_order_relaxed);

    const auto* next = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
    while (next) {
        if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            const auto* d3d = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
            if (d3d->device) {
                g_d3d11Device = d3d->device;
                g_d3d11Device->GetImmediateContext(g_d3d11Context.ReleaseAndGetAddressOf());
                LogMessage(LogLevel::Info, "OpenXR Layer: Captured ID3D11Device in xrCreateSession.");

                if (!IsCurrentProcessBlacklisted()) {
                    g_openXrD3D11Active.store(true, std::memory_order_relaxed);
                    g_shm.OpenOrCreate();
                    g_shm.SetOpenXrD3D11Pid(GetCurrentProcessId());
                }
            }
            break;
        } else if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto* d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
            if (d3d12->device) {
                g_d3d12Device = d3d12->device;
                LogMessage(LogLevel::Info, "OpenXR Layer: Captured ID3D12Device in xrCreateSession.");
                if (!IsCurrentProcessBlacklisted() && !IsOpenCompositeActive()) {
                    VrsFoveatedD3D12::Instance().OnSessionCreated(g_d3d12Device.Get(), g_cfg.vrs12.extraFormats);
                }
            }
            break;
        }
        next = next->next;
    }

    if (!g_d3d11Device) {
        g_openXrD3D11Active.store(false, std::memory_order_relaxed);
        LogMessage(LogLevel::Info, "OpenXR Layer: Non-D3D11 session (DX12/Vulkan) detected. Pure VDXR Bridge standby active.");
    }

    XrResult res = g_nextCreateSession(instance, createInfo, session);
    if (XR_SUCCEEDED(res) && session) {
        g_activeSession = *session;
        if (!g_d3d11Device && !IsCurrentProcessBlacklisted()) {
            g_vdxrBridgeStandby.store(true, std::memory_order_relaxed);
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrDestroySession(XrSession session) {
    if (g_activeSession == session) {
        g_activeSession = XR_NULL_HANDLE;
    }
    g_openXrSessionActive.store(false, std::memory_order_relaxed);
    g_vdxrBridgeStandby.store(false, std::memory_order_relaxed);
    g_vdxrBridgeActive.store(false, std::memory_order_relaxed);
    g_vdxrWarmupFrames.store(0, std::memory_order_relaxed);
    g_openXrD3D11Active.store(false, std::memory_order_relaxed);

    VrsFoveatedD3D11::Instance().Shutdown("DestroySession");
    VrsFoveatedD3D12::Instance().Shutdown("DestroySession");

    {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        g_swapchains.clear();
        g_appReturnedImages.clear();
        g_d3d11Device.Reset();
        g_d3d11Context.Reset();
        g_d3d12Device.Reset();
        g_cachedMasks[0].valid = false;
        g_cachedMasks[1].valid = false;
        g_eyeTangents[0].valid = false;
        g_eyeTangents[1].valid = false;
        g_vrsTargetEyeW = 0;
        g_vrsTargetEyeH = 0;
        g_vrsLastEnabled = false;
        g_vrsLastDebugMode = -1;
    }

    return g_nextDestroySession ? g_nextDestroySession(session) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Impl_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    if (!g_nextCreateSwapchain) return XR_ERROR_HANDLE_INVALID;
    if (IsCurrentProcessBlacklisted() || !g_d3d11Device || !createInfo || !swapchain) {
        return g_nextCreateSwapchain(session, createInfo, swapchain);
    }

    g_shm.SyncConfig(g_cfg);

    const bool isProjection = (createInfo->usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                              !(createInfo->usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

    const float s = (g_cfg.upscaling.enabled && isProjection) ? g_cfg.upscaling.scale : 1.0f;
    const uint32_t inW = createInfo->width;
    const uint32_t inH = createInfo->height;
    const uint32_t outW = (s > 1.0f) ? static_cast<uint32_t>(std::lround(inW * s)) : inW;
    const uint32_t outH = (s > 1.0f) ? static_cast<uint32_t>(std::lround(inH * s)) : inH;

    XrSwapchainCreateInfo modifiedInfo = *createInfo;
    if (s > 1.0f) {
        modifiedInfo.width = outW;
        modifiedInfo.height = outH;
    }

    XrResult res = g_nextCreateSwapchain(session, &modifiedInfo, swapchain);
    if (XR_SUCCEEDED(res) && swapchain && *swapchain != XR_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        auto& sc = g_swapchains[*swapchain];
        sc.originalW = inW;
        sc.originalH = inH;
        sc.upscaledW = outW;
        sc.upscaledH = outH;
        sc.format = static_cast<DXGI_FORMAT>(createInfo->format);
        sc.isDepthOrAux = !isProjection;
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrDestroySwapchain(XrSwapchain swapchain) {
    {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        g_swapchains.erase(swapchain);
        g_appReturnedImages.erase(swapchain);
    }
    return g_nextDestroySwapchain ? g_nextDestroySwapchain(swapchain) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainImages(
    XrSwapchain swapchain,
    uint32_t imageCapacityInput,
    uint32_t* imageCountOutput,
    XrSwapchainImageBaseHeader* images) {
    if (!g_nextEnumerateSwapchainImages) return XR_ERROR_HANDLE_INVALID;
    if (IsCurrentProcessBlacklisted() || !g_d3d11Device) {
        if (g_d3d12Device && images && imageCapacityInput > 0) {
            auto* d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
            for (uint32_t i = 0; i < imageCapacityInput; ++i) {
                if (d12Images[i].texture) {
                    VrsFoveatedD3D12::Instance().RegisterSwapchainTexture(d12Images[i].texture);
                }
            }
        }
        return g_nextEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
    }

    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        return g_nextEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
    }

    auto& sc = it->second;
    const bool needsUpscaling = (sc.upscaledW > sc.originalW && sc.upscaledH > sc.originalH && !sc.isDepthOrAux);

    if (imageCapacityInput == 0) {
        return g_nextEnumerateSwapchainImages(swapchain, 0, imageCountOutput, nullptr);
    }

    std::vector<XrSwapchainImageD3D11KHR> actualImages(imageCapacityInput, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    XrResult res = g_nextEnumerateSwapchainImages(
        swapchain,
        imageCapacityInput,
        imageCountOutput,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(actualImages.data())
    );

    if (XR_FAILED(res)) return res;

    const uint32_t count = *imageCountOutput;
    sc.upscaledTextures.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        sc.upscaledTextures[i] = actualImages[i].texture;
        VrsFoveatedD3D11::Instance().RegisterExcludedTexture(actualImages[i].texture);
    }

    if (needsUpscaling) {
        sc.originalTextures.resize(count);
        sc.srvs.resize(count);
        sc.uavs.resize(count);

        D3D11_TEXTURE2D_DESC appDesc{};
        sc.upscaledTextures[0]->GetDesc(&appDesc);
        appDesc.Width = sc.originalW;
        appDesc.Height = sc.originalH;
        appDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        appDesc.MiscFlags = 0;

        for (uint32_t i = 0; i < count; ++i) {
            HRESULT hr = g_d3d11Device->CreateTexture2D(&appDesc, nullptr, sc.originalTextures[i].ReleaseAndGetAddressOf());
            if (FAILED(hr)) return XR_ERROR_RUNTIME_FAILURE;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = appDesc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            hr = g_d3d11Device->CreateShaderResourceView(sc.originalTextures[i].Get(), &srvDesc, sc.srvs[i].ReleaseAndGetAddressOf());
            if (FAILED(hr)) return XR_ERROR_RUNTIME_FAILURE;

            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = appDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            hr = g_d3d11Device->CreateUnorderedAccessView(sc.upscaledTextures[i].Get(), &uavDesc, sc.uavs[i].ReleaseAndGetAddressOf());
            if (FAILED(hr)) return XR_ERROR_RUNTIME_FAILURE;
        }

        if (!sc.upscalerInitialized) {
            sc.upscaler.Initialize(g_d3d11Device.Get(), sc.originalW, sc.originalH, sc.upscaledW, sc.upscaledH);
            sc.upscalerInitialized = true;
        }

        auto* outD3D11 = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < count; ++i) {
            outD3D11[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            outD3D11[i].next = nullptr;
            outD3D11[i].texture = sc.originalTextures[i].Get();
        }
    } else {
        auto* outD3D11 = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (uint32_t i = 0; i < count; ++i) {
            outD3D11[i] = actualImages[i];
        }
    }

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrAcquireSwapchainImage(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquireInfo,
    uint32_t* index) {
    if (!g_nextAcquireSwapchainImage) return XR_ERROR_HANDLE_INVALID;
    XrResult res = g_nextAcquireSwapchainImage(swapchain, acquireInfo, index);
    if (XR_SUCCEEDED(res) && index && !IsCurrentProcessBlacklisted() && g_d3d11Device) {
        std::lock_guard<std::mutex> lock(g_layerMutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            it->second.lastAcquiredIndex = *index;
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* waitInfo) {
    return g_nextWaitSwapchainImage ? g_nextWaitSwapchainImage(swapchain, waitInfo) : XR_ERROR_HANDLE_INVALID;
}

XrResult XRAPI_CALL Impl_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo) {
    if (!g_nextReleaseSwapchainImage) return XR_ERROR_HANDLE_INVALID;
    if (IsCurrentProcessBlacklisted() || !g_d3d11Device) {
        return g_nextReleaseSwapchainImage(swapchain, releaseInfo);
    }

    std::unique_lock<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        auto& sc = it->second;
        if (sc.upscalerInitialized && sc.originalW < sc.upscaledW) {
            const uint32_t idx = sc.lastAcquiredIndex;
            if (idx < sc.srvs.size() && idx < sc.uavs.size()) {
                g_shm.SyncConfig(g_cfg);

                if (g_vrsLastEnabled) {
                    VrsFoveatedD3D11::Instance().DisableVRS(g_d3d11Context.Get(), true);
                }

                ID3D11ShaderResourceView* nullSrv = nullptr;
                ID3D11UnorderedAccessView* nullUav = nullptr;

                sc.upscaler.Dispatch(
                    g_d3d11Context.Get(),
                    sc.srvs[idx].Get(),
                    sc.uavs[idx].Get(),
                    g_cfg.upscaling.sharpness
                );

                g_d3d11Context->CSSetShaderResources(0, 1, &nullSrv);
                UINT initCounts = 0;
                g_d3d11Context->CSSetUnorderedAccessViews(0, 1, &nullUav, &initCounts);

                g_lastInW = sc.originalW;
                g_lastInH = sc.originalH;
                g_lastOutW = sc.upscaledW;
                g_lastOutH = sc.upscaledH;
                g_lastFormat = static_cast<uint32_t>(sc.format);
                g_upscaleStatus = "Active (NIS)";
            }
        }
    }
    lock.unlock();

    return g_nextReleaseSwapchainImage(swapchain, releaseInfo);
}

XrResult XRAPI_CALL Impl_xrWaitFrame(XrSession session, const XrFrameWaitInfo* frameWaitInfo, XrFrameState* frameState) {
    if (!g_nextWaitFrame) return XR_ERROR_HANDLE_INVALID;
    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    const auto* info = frameWaitInfo;
    auto* state = frameState;

    static int64_t s_lastWaitExitQpc = 0;
    static int64_t s_qpcFreq = 0;
    static uint32_t s_warmupFrames = 0;
    static uint32_t s_sswHalfRateHits = 0;
    static double s_panelHz = 90.0;
    static bool s_sswHalfRateActive = false;

    if (s_qpcFreq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s_qpcFreq = f.QuadPart;
    }

    const bool manualSubRate = (g_cfg.pacing.frameGenDivisor > 1.05);
    if (manualSubRate && s_sswHalfRateActive) {
        s_sswHalfRateActive = false;
        s_sswHalfRateHits = 0;
    }

    const double effectiveHz = (!manualSubRate && s_sswHalfRateActive) ? (s_panelHz * 0.5) : s_panelHz;
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled && s_warmupFrames >= 30) {
        LimitInputs lim{};
        lim.hz = hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        g_limiter.WaitBeforeRuntime();
    }

    const wchar_t* apiLabel = g_d3d11Device ? L"OpenXR (VDXR)" : L"OpenXR (VDXR Bridge)";
    g_shm.UpdateTelemetry(apiLabel, hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);

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
                    s_warmupFrames = 0;
                    s_sswHalfRateHits = 0;
                    s_sswHalfRateActive = false;
                }
            }
        }
    }

    if (s_qpcFreq > 0) {
        if (s_lastWaitExitQpc > 0) {
            const double dt = static_cast<double>(afterWaitQpc.QuadPart - s_lastWaitExitQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.250) {
                s_warmupFrames = 0;
                s_sswHalfRateHits = 0;
                s_sswHalfRateActive = false;
            } else if (dt > 0.004 && dt < 0.050) {
                if (s_warmupFrames < 30) {
                    s_warmupFrames++;
                }

                if (!manualSubRate && !s_sswHalfRateActive) {
                    const double measuredHz = 1.0 / dt;
                    const double halfPanel = s_panelHz * 0.5;
                    if (std::abs(measuredHz - halfPanel) <= (halfPanel * 0.035)) {
                        s_sswHalfRateHits++;
                        if (s_sswHalfRateHits >= 40) {
                            s_sswHalfRateActive = true;
                            s_sswHalfRateHits = 0;
                            LogMessage(LogLevel::Info, "OpenXR Pacing: SSW half-rate lock detected (" +
                                       std::to_string(static_cast<int>(std::lround(halfPanel))) + " Hz / " +
                                       std::to_string(static_cast<int>(std::lround(s_panelHz))) + " Hz panel).");
                        }
                    } else if (measuredHz > (halfPanel * 1.10)) {
                        s_sswHalfRateHits = 0;
                    } else if (s_sswHalfRateHits > 0) {
                        s_sswHalfRateHits--;
                    }
                }
            }
        }
        s_lastWaitExitQpc = afterWaitQpc.QuadPart;
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) {
    if (!g_nextEndFrame) return XR_ERROR_HANDLE_INVALID;
    if (!g_d3d11Device) {
        if (g_d3d12Device && !IsCurrentProcessBlacklisted() && frameEndInfo && frameEndInfo->layerCount > 0 && frameEndInfo->layers) {
            uint32_t projEyeW = 0;
            uint32_t projEyeH = 0;
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                const auto* layer = frameEndInfo->layers[i];
                if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    if (proj->viewCount >= 2 && proj->views) {
                        const int32_t extW = proj->views[0].subImage.imageRect.extent.width;
                        const int32_t extH = proj->views[0].subImage.imageRect.extent.height;
                        if (extW > 0 && extH > 0) {
                            projEyeW = static_cast<uint32_t>(extW);
                            projEyeH = static_cast<uint32_t>(extH);
                            break;
                        }
                    }
                }
            }
            if (projEyeW > 0 && projEyeH > 0) {
                const bool ffr12Enabled = !IsOpenCompositeActive() && (g_cfg.ffr.openxrEnabled || (g_cfg.vrs12.debugMode > 0));
                const int dbg12 = g_cfg.vrs12.debugMode;
                if (projEyeW != g_vrsTargetEyeW || projEyeH != g_vrsTargetEyeH) {
                    LogMessage(LogLevel::Info, "VRS12: OpenXR target eye " + std::to_string(projEyeW) + "x" + std::to_string(projEyeH) + " (from projection layer)");
                }
                if (projEyeW != g_vrsTargetEyeW || projEyeH != g_vrsTargetEyeH || ffr12Enabled != g_vrsLastEnabled || dbg12 != g_vrsLastDebugMode) {
                    g_vrsTargetEyeW = projEyeW;
                    g_vrsTargetEyeH = projEyeH;
                    g_vrsLastEnabled = ffr12Enabled;
                    g_vrsLastDebugMode = dbg12;
                    VrsFoveatedD3D12::Instance().UpdateTarget(g_d3d12Device.Get(), ffr12Enabled, projEyeW, projEyeH, dbg12);
                }
            }
            VrsFoveatedD3D12::Instance().EndFrame();
        }

        if (g_vdxrBridgeStandby.load(std::memory_order_relaxed) && !g_vdxrBridgeActive.load(std::memory_order_relaxed)) {
            const uint32_t frames = g_vdxrWarmupFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            if (frames >= 60) {
                if (GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll") ||
                    GetModuleHandleW(L"LibOVRRT64_1.dll") ||
                    GetModuleHandleW(L"LibPVRRT64_1_X.dll")) {
                    g_vdxrBridgeStandby.store(false, std::memory_order_relaxed);
                    g_openXrSessionActive.store(true, std::memory_order_relaxed);
                    g_vdxrBridgeActive.store(true, std::memory_order_relaxed);
                    EnsureHookDllLoadedForVdxrBridge();
                } else if (frames >= 300) {
                    g_vdxrBridgeStandby.store(false, std::memory_order_relaxed);
                }
            }
        }
        return g_nextEndFrame(session, frameEndInfo);
    }

    if (IsCurrentProcessBlacklisted() || !frameEndInfo || frameEndInfo->layerCount == 0 || !frameEndInfo->layers) {
        return g_nextEndFrame(session, frameEndInfo);
    }

    if (g_d3d11Device && g_d3d11Context) {
        uint32_t projEyeW = 0;
        uint32_t projEyeH = 0;
        for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
            const auto* layer = frameEndInfo->layers[i];
            if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                if (proj->viewCount >= 2 && proj->views) {
                    const int32_t extW = proj->views[0].subImage.imageRect.extent.width;
                    const int32_t extH = proj->views[0].subImage.imageRect.extent.height;
                    if (extW > 0 && extH > 0) {
                        projEyeW = static_cast<uint32_t>(extW);
                        projEyeH = static_cast<uint32_t>(extH);
                        break;
                    }
                }
            }
        }

        if (projEyeW > 0 && projEyeH > 0) {
            const bool ffrEnabled = !IsOpenCompositeActive() && (g_cfg.ffr.openxrEnabled || (g_cfg.ffr.debugMode > 0));
            const int dbgMode = g_cfg.ffr.debugMode;

            if (projEyeW != g_vrsTargetEyeW || projEyeH != g_vrsTargetEyeH) {
                LogMessage(LogLevel::Info, "VRS: OpenXR target eye " + std::to_string(projEyeW) + "x" + std::to_string(projEyeH) + " (from projection layer)");
            }

            if (projEyeW != g_vrsTargetEyeW || projEyeH != g_vrsTargetEyeH || ffrEnabled != g_vrsLastEnabled || dbgMode != g_vrsLastDebugMode) {
                g_vrsTargetEyeW = projEyeW;
                g_vrsTargetEyeH = projEyeH;
                g_vrsLastEnabled = ffrEnabled;
                g_vrsLastDebugMode = dbgMode;
                VrsFoveatedD3D11::Instance().UpdateTarget(g_d3d11Device.Get(), g_d3d11Context.Get(), ffrEnabled, projEyeW, projEyeH, dbgMode);
            }
        }
        VrsFoveatedD3D11::Instance().EndFrame();
    }

    std::vector<const XrCompositionLayerBaseHeader*> remapped(frameEndInfo->layerCount);
    std::vector<XrCompositionLayerProjection> projCopies;
    std::vector<std::vector<XrCompositionLayerProjectionView>> viewsCopies;
    std::vector<XrCompositionLayerQuad> quadCopies;
    projCopies.reserve(frameEndInfo->layerCount);
    viewsCopies.reserve(frameEndInfo->layerCount);
    quadCopies.reserve(frameEndInfo->layerCount);

    std::lock_guard<std::mutex> lock(g_layerMutex);

    for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
        const auto* layer = frameEndInfo->layers[i];
        if (!layer) {
            remapped[i] = nullptr;
            continue;
        }

        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            projCopies.push_back(*proj);
            auto& newProj = projCopies.back();

            if (proj->views && proj->viewCount > 0) {
                viewsCopies.emplace_back(proj->views, proj->views + proj->viewCount);
                auto& newViews = viewsCopies.back();

                for (uint32_t v = 0; v < proj->viewCount; ++v) {
                    auto it = g_swapchains.find(newViews[v].subImage.swapchain);
                    if (it != g_swapchains.end()) {
                        const auto& sc = it->second;
                        if (sc.originalW < sc.upscaledW) {
                            const float s = static_cast<float>(sc.upscaledW) / static_cast<float>(sc.originalW);
                            newViews[v].subImage.imageRect.offset.x = static_cast<int32_t>(std::lround(newViews[v].subImage.imageRect.offset.x * s));
                            newViews[v].subImage.imageRect.offset.y = static_cast<int32_t>(std::lround(newViews[v].subImage.imageRect.offset.y * s));
                            newViews[v].subImage.imageRect.extent.width = static_cast<int32_t>(std::lround(newViews[v].subImage.imageRect.extent.width * s));
                            newViews[v].subImage.imageRect.extent.height = static_cast<int32_t>(std::lround(newViews[v].subImage.imageRect.extent.height * s));
                        }
                    }
                }
                newProj.views = newViews.data();
            }
            remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&newProj);
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
            quadCopies.push_back(*quad);
            auto& newQuad = quadCopies.back();

            auto it = g_swapchains.find(newQuad.subImage.swapchain);
            if (it != g_swapchains.end()) {
                const auto& sc = it->second;
                if (sc.originalW < sc.upscaledW) {
                    const float s = static_cast<float>(sc.upscaledW) / static_cast<float>(sc.originalW);
                    newQuad.subImage.imageRect.offset.x = static_cast<int32_t>(std::lround(newQuad.subImage.imageRect.offset.x * s));
                    newQuad.subImage.imageRect.offset.y = static_cast<int32_t>(std::lround(newQuad.subImage.imageRect.offset.y * s));
                    newQuad.subImage.imageRect.extent.width = static_cast<int32_t>(std::lround(newQuad.subImage.imageRect.extent.width * s));
                    newQuad.subImage.imageRect.extent.height = static_cast<int32_t>(std::lround(newQuad.subImage.imageRect.extent.height * s));
                }
            }
            remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&newQuad);
        } else {
            remapped[i] = layer;
        }
    }

    XrFrameEndInfo modifiedEndInfo = *frameEndInfo;
    modifiedEndInfo.layers = remapped.data();

    return g_nextEndFrame(session, &modifiedEndInfo);
}

XrResult XRAPI_CALL Impl_xrDestroyInstance(XrInstance instance) {
    if (g_nextDestroyInstance) {
        XrResult res = g_nextDestroyInstance(instance);
        g_initialized.store(false);
        return res;
    }
    return XR_ERROR_HANDLE_INVALID;
}

void ResolveProcPointers(XrInstance instance) {
    if (!g_nextGetInstanceProcAddr) return;

    #define RESOLVE(name) g_nextGetInstanceProcAddr(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&g_next##name))
    RESOLVE(CreateInstance);
    RESOLVE(DestroyInstance);
    RESOLVE(CreateSession);
    RESOLVE(DestroySession);
    RESOLVE(CreateSwapchain);
    RESOLVE(DestroySwapchain);
    RESOLVE(EnumerateSwapchainImages);
    RESOLVE(AcquireSwapchainImage);
    RESOLVE(WaitSwapchainImage);
    RESOLVE(ReleaseSwapchainImage);
    RESOLVE(WaitFrame);
    RESOLVE(EndFrame);
    RESOLVE(EnumerateViewConfigurationViews);
    RESOLVE(GetVisibilityMaskKHR);
    RESOLVE(LocateViews);
    RESOLVE(CreateReferenceSpace);
    RESOLVE(DestroySpace);
    RESOLVE(PollEvent);
    #undef RESOLVE
}

} // namespace

} // namespace vrpacing

extern "C" {

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    using namespace vrpacing;
    EnsureInit();

    if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;

    // For pre-instance calls (instance == XR_NULL_HANDLE), always delegate to the runtime
    if (instance == XR_NULL_HANDLE) {
        return g_nextGetInstanceProcAddr ? g_nextGetInstanceProcAddr(instance, name, function) : XR_ERROR_HANDLE_INVALID;
    }

    // Lazily resolve if the application queries instance functions before our table is populated
    if (!g_nextCreateSession && g_nextGetInstanceProcAddr) {
        ResolveProcPointers(instance);
    }

    if (strcmp(name, "xrDestroyInstance") == 0 && g_nextDestroyInstance) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroyInstance);
    else if (strcmp(name, "xrCreateSession") == 0 && g_nextCreateSession) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrCreateSession);
    else if (strcmp(name, "xrDestroySession") == 0 && g_nextDestroySession) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroySession);
    else if (strcmp(name, "xrEnumerateViewConfigurationViews") == 0 && g_nextEnumerateViewConfigurationViews) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateViewConfigurationViews);
    else if (strcmp(name, "xrCreateSwapchain") == 0 && g_nextCreateSwapchain) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrCreateSwapchain);
    else if (strcmp(name, "xrDestroySwapchain") == 0 && g_nextDestroySwapchain) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrDestroySwapchain);
    else if (strcmp(name, "xrEnumerateSwapchainImages") == 0 && g_nextEnumerateSwapchainImages) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEnumerateSwapchainImages);
    else if (strcmp(name, "xrAcquireSwapchainImage") == 0 && g_nextAcquireSwapchainImage) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrAcquireSwapchainImage);
    else if (strcmp(name, "xrWaitSwapchainImage") == 0 && g_nextWaitSwapchainImage) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrWaitSwapchainImage);
    else if (strcmp(name, "xrReleaseSwapchainImage") == 0 && g_nextReleaseSwapchainImage) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrReleaseSwapchainImage);
    else if (strcmp(name, "xrEndFrame") == 0 && g_nextEndFrame) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrEndFrame);
    else if (strcmp(name, "xrWaitFrame") == 0 && g_nextWaitFrame) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrWaitFrame);
    else if (strcmp(name, "xrGetVisibilityMaskKHR") == 0 && g_nextGetVisibilityMaskKHR) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetVisibilityMaskKHR);
    else if (strcmp(name, "xrLocateViews") == 0 && g_nextLocateViews) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrLocateViews);
    else if (strcmp(name, "xrPollEvent") == 0 && g_nextPollEvent) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrPollEvent);
    else return g_nextGetInstanceProcAddr ? g_nextGetInstanceProcAddr(instance, name, function) : XR_ERROR_FUNCTION_UNSUPPORTED;

    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                              const XrApiLayerCreateInfo* layerInfo,
                                                              XrInstance* instance) {
    using namespace vrpacing;
    EnsureInit();

    auto nextInfo = layerInfo ? layerInfo->nextInfo : nullptr;
    if (!nextInfo || !nextInfo->nextGetInstanceProcAddr || !nextInfo->nextCreateApiLayerInstance) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrApiLayerCreateInfo localLayerInfo = *layerInfo;
    localLayerInfo.nextInfo = nextInfo->next;

    if (nextInfo->nextGetInstanceProcAddr == &Layer_xrGetInstanceProcAddr) {
        return nextInfo->nextCreateApiLayerInstance(info, &localLayerInfo, instance);
    }

    // Store nextGetInstanceProcAddr BEFORE calling nextCreateApiLayerInstance so any downstream
    // queries during instance creation never see a null g_nextGetInstanceProcAddr.
    g_nextGetInstanceProcAddr = nextInfo->nextGetInstanceProcAddr;

    XrResult res = nextInfo->nextCreateApiLayerInstance(info, &localLayerInfo, instance);
    if (XR_FAILED(res)) return res;

    if (instance && *instance != XR_NULL_HANDLE) {
        ResolveProcPointers(*instance);
    }
    return XR_SUCCESS;
}

} // extern "C"

extern "C" __declspec(dllexport) bool __cdecl VRPacingLayer_IsActiveD3D11() {
    return vrpacing::g_openXrD3D11Active.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) bool __cdecl VRPacingLayer_IsActiveSession() {
    return vrpacing::g_openXrSessionActive.load(std::memory_order_relaxed);
}

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo,
    const char* layerName,
    XrNegotiateApiLayerRequest* apiLayerRequest) {
    (void)layerName;
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
    vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR Layer negotiated successfully with runtime.");

    return XR_SUCCESS;
}

#endif