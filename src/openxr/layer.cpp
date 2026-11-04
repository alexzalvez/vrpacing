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
#include <d3d12.h>
#include <wrl/client.h>

#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "common/visibility_mask.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "gfx/vrs_foveated_d3d12.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_openXrD3D11Active{ false };
std::atomic<bool> g_openXrSessionActive{ false };

namespace {

PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrDestroySession g_nextDestroySession = nullptr;
PFN_xrDestroyInstance g_nextDestroyInstance = nullptr;
PFN_xrEnumerateViewConfigurationViews g_nextEnumerateViewConfigurationViews = nullptr;
PFN_xrCreateSwapchain g_nextCreateSwapchain = nullptr;
PFN_xrDestroySwapchain g_nextDestroySwapchain = nullptr;
PFN_xrEnumerateSwapchainImages g_nextEnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquireSwapchainImage = nullptr;
PFN_xrWaitSwapchainImage g_nextWaitSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage g_nextReleaseSwapchainImage = nullptr;
PFN_xrEndFrame g_nextEndFrame = nullptr;
PFN_xrWaitFrame g_nextWaitFrame = nullptr;
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
std::atomic<bool> g_maskWasActiveInSession{ false };

std::atomic<bool> g_vdxrBridgeStandby{ false };
std::atomic<bool> g_vdxrBridgeActive{ false };
std::atomic<uint32_t> g_vdxrWarmupFrames{ 0 };

RuntimeConfig g_cfg;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;

ComPtr<ID3D11Device> g_d3d11Device;
ComPtr<ID3D11DeviceContext> g_d3d11Context;
ComPtr<ID3D12Device> g_d3d12Device;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;
uint32_t g_nativeViewW = 0;
uint32_t g_nativeViewH = 0;
uint32_t g_vrsTargetEyeW = 0;
uint32_t g_vrsTargetEyeH = 0;
bool g_vrsLastEnabled = false;
int g_vrsLastDebugMode = -1;

std::atomic<uint64_t> g_visMaskCallsD3D11{ 0 };

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

bool IsOpenCompositeActive() {
    return GetModuleHandleW(L"openvr_api.dll") != nullptr;
}

void EnsureHookDllLoadedForVdxrBridge() {
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&EnsureHookDllLoadedForVdxrBridge), &hSelf)) {
        return;
    }

    wchar_t layerPath[MAX_PATH]{};
    if (!GetModuleFileNameW(hSelf, layerPath, MAX_PATH)) {
        return;
    }

    std::wstring dir(layerPath);
    const auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir.resize(pos);
    }

    const std::wstring hookPath = dir + L"\\VRPacingHook.dll";
    HMODULE hHook = LoadLibraryW(hookPath.c_str());
    if (hHook) {
        using RefreshFn = void(__cdecl*)();
        auto fn = reinterpret_cast<RefreshFn>(GetProcAddress(hHook, "VRPacingHook_RefreshHooks"));
        if (fn) {
            fn();
        }
        LogMessage(LogLevel::Info, "OpenXR Layer (Pure VDXR Bridge): Linked VRPacingHook.dll for post-render D3D11 NIS upscaling.");
    }
}

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenXR Layer initialized.");
    });
}

void PopulateTangentsIfNeeded(XrSession session, XrViewConfigurationType viewConfigurationType, uint32_t viewIndex,
                              float& minX, float& maxX, float& minY, float& maxY) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
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

    float minX = -0.85f, maxX = 0.85f, minY = -0.85f, maxY = 0.85f;
    PopulateTangentsIfNeeded(session, viewConfigurationType, viewIndex, minX, maxX, minY, maxY);

    VisibilityMaskKind kind{};
    bool validKind = true;
    if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR) {
        kind = VisibilityMaskKind::VisibleTriangles;
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR) {
        kind = VisibilityMaskKind::HiddenTriangles;
    } else if (visibilityMaskType == XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR) {
        kind = VisibilityMaskKind::LineLoop;
    } else {
        validKind = false;
    }

    if (!g_cfg.mask.enabled) {
        if (!g_maskWasActiveInSession.load(std::memory_order_relaxed)) {
            if (g_nextGetVisibilityMaskKHR) {
                return g_nextGetVisibilityMaskKHR(session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
            }
            visibilityMask->vertexCountOutput = 0;
            visibilityMask->indexCountOutput = 0;
            return XR_SUCCESS;
        }

        if (!validKind || kind == VisibilityMaskKind::LineLoop) {
            visibilityMask->vertexCountOutput = 0;
            visibilityMask->indexCountOutput = 0;
            return XR_SUCCESS;
        }

        GeneratedVisibilityMask resetMask = GenerateResetMaskTangentSpace(kind, minX, maxX, minY, maxY);
        visibilityMask->vertexCountOutput = static_cast<uint32_t>(resetMask.vertices.size());
        visibilityMask->indexCountOutput = static_cast<uint32_t>(resetMask.indices.size());
        if (visibilityMask->vertexCapacityInput == 0 || visibilityMask->indexCapacityInput == 0 ||
            !visibilityMask->vertices || !visibilityMask->indices) {
            return XR_SUCCESS;
        }
        if (visibilityMask->vertexCapacityInput < visibilityMask->vertexCountOutput ||
            visibilityMask->indexCapacityInput < visibilityMask->indexCountOutput) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        for (size_t i = 0; i < resetMask.vertices.size(); ++i) {
            visibilityMask->vertices[i] = { resetMask.vertices[i].x, resetMask.vertices[i].y };
        }
        for (size_t i = 0; i < resetMask.indices.size(); ++i) {
            visibilityMask->indices[i] = resetMask.indices[i];
        }
        return XR_SUCCESS;
    }

    if (!validKind) {
        visibilityMask->vertexCountOutput = 0;
        visibilityMask->indexCountOutput = 0;
        return XR_SUCCESS;
    }

    g_maskWasActiveInSession.store(true, std::memory_order_relaxed);

    constexpr uint32_t kSeg = 256;
    uint32_t reqVerts = 0;
    uint32_t reqIndices = 0;
    GetCircularMaskCounts(kind, kSeg, reqVerts, reqIndices);

    visibilityMask->vertexCountOutput = reqVerts;
    visibilityMask->indexCountOutput = reqIndices;

    if (visibilityMask->vertexCapacityInput == 0 || visibilityMask->indexCapacityInput == 0 ||
        !visibilityMask->vertices || !visibilityMask->indices) {
        return XR_SUCCESS;
    }

    if (visibilityMask->vertexCapacityInput < reqVerts || visibilityMask->indexCapacityInput < reqIndices) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    GeneratedVisibilityMask gen = GenerateCircularMaskTangentSpace(kind, minX, maxX, minY, maxY, kSeg);
    for (size_t i = 0; i < gen.vertices.size(); ++i) {
        visibilityMask->vertices[i] = { gen.vertices[i].x, gen.vertices[i].y };
    }
    for (size_t i = 0; i < gen.indices.size(); ++i) {
        visibilityMask->indices[i] = gen.indices[i];
    }

    static std::atomic<bool> s_loggedEye[2]{ false, false };
    if (viewIndex < 2 && !s_loggedEye[viewIndex].exchange(true)) {
        LogMessage(LogLevel::Info, "OpenXR Layer: Pre-Render Circular Visibility Mask (r=1.00, 256 seg) applied to eye " +
                   std::to_string(viewIndex) + " Tangents=[" + std::to_string(minX) + ", " + std::to_string(maxX) +
                   ", " + std::to_string(minY) + ", " + std::to_string(maxY) + "]");
    }

    return XR_SUCCESS;
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
                std::lock_guard<std::mutex> lock(g_layerMutex);
                const bool wasValid = g_eyeTangents[i].valid;
                const bool changed = wasValid && (
                                     std::abs(g_eyeTangents[i].minX - l) > 0.005f ||
                                     std::abs(g_eyeTangents[i].maxX - r) > 0.005f ||
                                     std::abs(g_eyeTangents[i].minY - d) > 0.005f ||
                                     std::abs(g_eyeTangents[i].maxY - u) > 0.005f);
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
        {
            std::lock_guard<std::mutex> lock(g_layerMutex);
            g_activeSession = *session;
            g_eyeTangents[0].valid = false;
            g_eyeTangents[1].valid = false;
            g_maskWasActiveInSession.store(false, std::memory_order_relaxed);
        }
        g_vrsTargetEyeW = 0;
        g_vrsTargetEyeH = 0;
        g_vrsLastEnabled = false;
        g_vrsLastDebugMode = -1;
        if (g_d3d11Device && !IsCurrentProcessBlacklisted()) {
            g_openXrSessionActive.store(true, std::memory_order_relaxed);
        } else if (!IsCurrentProcessBlacklisted()) {
            g_openXrSessionActive.store(true, std::memory_order_relaxed);
            g_vdxrBridgeStandby.store(true, std::memory_order_relaxed);
        }
    }
    return res;
}

XrResult XRAPI_CALL Impl_xrDestroySession(XrSession session) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    if (g_d3d11Device && !IsCurrentProcessBlacklisted()) {
        if (g_d3d11Context) {
            VrsFoveatedD3D11::Instance().DisableVRS(g_d3d11Context.Get(), true);
        }
        VrsFoveatedD3D11::Instance().Shutdown("DestroySession");
        VrsFoveatedD3D11::Instance().ClearExcludedTextures();
    }
    if (g_d3d12Device && !IsCurrentProcessBlacklisted()) {
        VrsFoveatedD3D12::Instance().Shutdown("DestroySession");
        VrsFoveatedD3D12::Instance().ClearSwapchainTextures();
    }
    g_vrsTargetEyeW = 0;
    g_vrsTargetEyeH = 0;
    g_vrsLastEnabled = false;
    g_vrsLastDebugMode = -1;
    g_swapchains.clear();
    g_openXrD3D11Active.store(false, std::memory_order_relaxed);
    g_openXrSessionActive.store(false, std::memory_order_relaxed);
    g_vdxrBridgeStandby.store(false, std::memory_order_relaxed);
    g_vdxrBridgeActive.store(false, std::memory_order_relaxed);
    g_vdxrWarmupFrames.store(0, std::memory_order_relaxed);
    if (g_shm.IsOpenXrD3D11ActiveForCurrentProcess()) {
        g_shm.SetOpenXrD3D11Pid(0);
    }
    if (g_activeSession == session) {
        g_activeSession = XR_NULL_HANDLE;
    }
    return g_nextDestroySession ? g_nextDestroySession(session) : XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrDestroyInstance(XrInstance instance) {
    if (g_d3d11Context) {
        VrsFoveatedD3D11::Instance().DisableVRS(g_d3d11Context.Get(), true);
    }
    VrsFoveatedD3D11::Instance().Shutdown("DestroyInstance");
    VrsFoveatedD3D11::Instance().ClearExcludedTextures();
    if (g_d3d12Device) {
        VrsFoveatedD3D12::Instance().Shutdown("DestroyInstance");
    }
    if (g_nextDestroyInstance) {
        return g_nextDestroyInstance(instance);
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
            LogMessage(LogLevel::Info, "OpenXR ViewConfig[" + std::to_string(i) + "]: Native=" +
                       std::to_string(views[i].recommendedImageRectWidth) + "x" +
                       std::to_string(views[i].recommendedImageRectHeight));
        }
    }

    return res;
}

XrResult XRAPI_CALL Impl_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain) {
    EnsureInit();
    if (!createInfo || !swapchain || !g_nextCreateSwapchain) return XR_ERROR_VALIDATION_FAILURE;

    if (!g_d3d11Device || IsCurrentProcessBlacklisted()) {
        return g_nextCreateSwapchain(session, createInfo, swapchain);
    }

    g_shm.SyncConfig(g_cfg);

    const bool isStatic = (createInfo->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
    const bool isColor = !isStatic &&
                         (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                         (createInfo->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;

    bool isEyeResolution = false;
    if (g_nativeViewW > 0 && g_nativeViewH > 0) {
        const bool heightMatches = (createInfo->height >= (g_nativeViewH * 4 / 5)) &&
                                   (createInfo->height <= (g_nativeViewH * 6 / 5));
        const bool widthMatchesSingle = (createInfo->width >= (g_nativeViewW * 4 / 5)) &&
                                        (createInfo->width <= (g_nativeViewW * 6 / 5));
        const bool widthMatchesSbs = (createInfo->width >= (g_nativeViewW * 8 / 5)) &&
                                     (createInfo->width <= (g_nativeViewW * 12 / 5));
        isEyeResolution = heightMatches && (widthMatchesSingle || widthMatchesSbs);
    } else {
        isEyeResolution = (createInfo->width >= 1400 && createInfo->height >= 1400);
    }

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
        LogMessage(LogLevel::Error, "OpenXR Layer: Failed to create native upscaled swapchain.");
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
            LogMessage(LogLevel::Error, "OpenXR Layer: Failed to create intermediate D3D11 texture. HR=" + std::to_string(hr));
            g_nextDestroySwapchain(real);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        proxy.appTextures.push_back(appTex);
    }

    std::lock_guard<std::mutex> lock(g_layerMutex);
    g_swapchains[real] = std::move(proxy);
    *swapchain = real;

    LogMessage(LogLevel::Info, "OpenXR Proxy Swapchain created: " +
               std::to_string(appW) + "x" + std::to_string(appH) +
               " (ArraySize=" + std::to_string(arrSize) + ") -> " +
               std::to_string(realW) + "x" + std::to_string(realH));

    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrDestroySwapchain(XrSwapchain swapchain) {
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        for (auto& tex : it->second.appTextures) {
            VrsFoveatedD3D11::Instance().UnregisterExcludedTexture(tex.Get());
        }
        for (auto& tex : it->second.realTextures) {
            VrsFoveatedD3D11::Instance().UnregisterExcludedTexture(tex.Get());
        }
        g_swapchains.erase(it);
    }
    g_upscaler.ClearCache();
    return g_nextDestroySwapchain ? g_nextDestroySwapchain(swapchain) : XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images) {
    if (!g_nextEnumerateSwapchainImages) return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(g_layerMutex);
    auto it = g_swapchains.find(swapchain);
    if (it == g_swapchains.end()) {
        XrResult r_enum = g_nextEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
        if (XR_SUCCEEDED(r_enum) && images && imageCapacityInput > 0) {
            if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                auto* realD3d = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
                for (uint32_t j = 0; j < imageCapacityInput; ++j) {
                    if (realD3d[j].texture) {
                        VrsFoveatedD3D11::Instance().RegisterExcludedTexture(realD3d[j].texture);
                    }
                }
            } else if (images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
                auto* realD3d12 = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
                for (uint32_t j = 0; j < imageCapacityInput; ++j) {
                    if (realD3d12[j].texture) {
                        VrsFoveatedD3D12::Instance().RegisterSwapchainTexture(realD3d12[j].texture);
                    }
                }
            }
        }
        return r_enum;
    }

    const auto& proxy = it->second;
    const uint32_t count = static_cast<uint32_t>(proxy.appTextures.size());
    if (imageCountOutput) *imageCountOutput = count;
    if (imageCapacityInput == 0 || !images) return XR_SUCCESS;

    auto* d3dImgs = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
    for (uint32_t i = 0; i < (std::min)(imageCapacityInput, count); ++i) {
        d3dImgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        d3dImgs[i].texture = proxy.appTextures[i].Get();
        VrsFoveatedD3D11::Instance().RegisterExcludedTexture(proxy.appTextures[i].Get());
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL Impl_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index) {
    if (!g_nextAcquireSwapchainImage) return XR_ERROR_HANDLE_INVALID;
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
    if (!g_nextWaitSwapchainImage) return XR_ERROR_HANDLE_INVALID;
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
    if (!g_nextReleaseSwapchainImage) return XR_ERROR_HANDLE_INVALID;
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
        const bool isSideBySide = (numSlices == 1) && (proxy.appWidth >= (proxy.appHeight * 3 / 2));

        static std::atomic<bool> s_firstFrameLogged{ false };
        if (!s_firstFrameLogged.exchange(true)) {
            LogMessage(LogLevel::Info, "OpenXR [Release] First frame: App=" +
                       std::to_string(proxy.appWidth) + "x" + std::to_string(proxy.appHeight) +
                       " ArraySize=" + std::to_string(numSlices) +
                       " FMT=" + std::to_string(proxy.format) +
                       " -> Real=" + std::to_string(proxy.realWidth) + "x" + std::to_string(proxy.realHeight) +
                       " Scale=" + std::to_string(g_cfg.upscaling.scale));
        }

        if (g_vrsLastEnabled) {
            VrsFoveatedD3D11::Instance().DisableVRS(g_d3d11Context.Get(), true);
        }
        VrsSuppressScope vrsSuppress;
        bool allSlicesOk = true;

        if (isSideBySide) {
            const uint32_t halfAppW = proxy.appWidth / 2;
            const uint32_t halfRealW = proxy.realWidth / 2;
            for (uint32_t eye = 0; eye < 2; ++eye) {
                UpscaleDispatchDesc desc{};
                desc.srcRect = { eye * halfAppW, 0, halfAppW, proxy.appHeight };
                desc.dstRect = { eye * halfRealW, 0, halfRealW, proxy.realHeight };
                desc.srcArrayIndex = 0;
                desc.dstArrayIndex = 0;
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
        } else {
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
        }

        g_upscaleStatus = allSlicesOk ? 1 : -3;
    }

    return g_nextReleaseSwapchainImage(proxy.realSwapchain, releaseInfo);
}

XrResult XRAPI_CALL Impl_xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    EnsureInit();
    if (!g_nextWaitFrame) return XR_ERROR_HANDLE_INVALID;

    if (IsCurrentProcessBlacklisted()) {
        return g_nextWaitFrame(session, info, state);
    }

    g_shm.SyncConfig(g_cfg);

    static int s_lastMaskState = -1;
    const int curMaskState = g_cfg.mask.enabled ? 1 : 0;
    if (!IsOpenCompositeActive() && s_lastMaskState != -1 && s_lastMaskState != curMaskState) {
        g_pendingMaskEvent[0].store(true, std::memory_order_relaxed);
        g_pendingMaskEvent[1].store(true, std::memory_order_relaxed);
    }
    s_lastMaskState = curMaskState;

    if (!g_d3d11Device && !g_vdxrBridgeActive.load(std::memory_order_relaxed)) {
        XrResult res = g_nextWaitFrame(session, info, state);
        if (XR_SUCCEEDED(res) && state && state->predictedDisplayTime > 0) {
            g_lastPredictedDisplayTime.store(state->predictedDisplayTime, std::memory_order_relaxed);
        }
        return res;
    }

    static int64_t s_qpcFreq = 0;
    static int64_t s_lastWaitExitQpc = 0;
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
    const int32_t vrsStatus = g_d3d12Device ? VrsFoveatedD3D12::Instance().GetVrsStatus()
                                            : VrsFoveatedD3D11::Instance().GetVrsStatus();
    const int32_t maskStatus = g_cfg.mask.enabled ? 1 : 0;
    g_shm.UpdateSessionTelemetry(apiLabel, hz, targetFps,
                                 s_panelHz, s_sswHalfRateActive ? 1u : 0u,
                                 vrsStatus, maskStatus,
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
        if (!layer) continue;

        if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* proj = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            XrCompositionLayerProjection copy = *proj;
            std::vector<XrCompositionLayerProjectionView> vCopy(proj->views, proj->views + proj->viewCount);

            for (uint32_t v = 0; v < copy.viewCount; ++v) {
                auto& view = vCopy[v];
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
            viewsCopies.push_back(std::move(vCopy));
            copy.views = viewsCopies.back().data();
            projCopies.push_back(copy);
            remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projCopies.back());
        } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
            auto it = g_swapchains.find(quad->subImage.swapchain);
            if (it != g_swapchains.end()) {
                XrCompositionLayerQuad qCopy = *quad;
                const auto& p = it->second;
                const double sx = static_cast<double>(p.realWidth) / p.appWidth;
                const double sy = static_cast<double>(p.realHeight) / p.appHeight;

                qCopy.subImage.imageRect.offset.x = static_cast<int32_t>(std::lround(qCopy.subImage.imageRect.offset.x * sx));
                qCopy.subImage.imageRect.offset.y = static_cast<int32_t>(std::lround(qCopy.subImage.imageRect.offset.y * sy));
                qCopy.subImage.imageRect.extent.width = static_cast<int32_t>(std::lround(qCopy.subImage.imageRect.extent.width * sx));
                qCopy.subImage.imageRect.extent.height = static_cast<int32_t>(std::lround(qCopy.subImage.imageRect.extent.height * sy));
                quadCopies.push_back(qCopy);
                remapped[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quadCopies.back());
            } else {
                remapped[i] = layer;
            }
        } else {
            remapped[i] = layer;
        }
    }

    XrFrameEndInfo modified = *frameEndInfo;
    modified.layers = remapped.data();
    return g_nextEndFrame(session, &modified);
}

void ResolveProcPointers(XrInstance instance) {
    if (!g_nextGetInstanceProcAddr || instance == XR_NULL_HANDLE) return;
    g_nextCreateSession = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSession));
    g_nextDestroySession = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroySession", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySession));
    g_nextDestroyInstance = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroyInstance));
    g_nextEnumerateViewConfigurationViews = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrEnumerateViewConfigurationViews", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextEnumerateViewConfigurationViews));
    g_nextCreateSwapchain = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextCreateSwapchain));
    g_nextDestroySwapchain = nullptr;
    g_nextGetInstanceProcAddr(instance, "xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&g_nextDestroySwapchain));
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

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                              const XrApiLayerCreateInfo* layerInfo,
                                                              XrInstance* instance) {
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

XRAPI_ATTR XrResult XRAPI_CALL Layer_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
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
    else if (strcmp(name, "xrGetVisibilityMaskKHR") == 0) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrGetVisibilityMaskKHR);
    else if (strcmp(name, "xrLocateViews") == 0 && g_nextLocateViews) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrLocateViews);
    else if (strcmp(name, "xrPollEvent") == 0 && g_nextPollEvent) *function = reinterpret_cast<PFN_xrVoidFunction>(Impl_xrPollEvent);
    else return g_nextGetInstanceProcAddr ? g_nextGetInstanceProcAddr(instance, name, function) : XR_ERROR_FUNCTION_UNSUPPORTED;

    return XR_SUCCESS;
}

} // namespace vrpacing

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
