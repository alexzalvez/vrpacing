#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "minhook/include/MinHook.h"
#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"
#include "openvr/hook_openvr.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_openVrHandlingFrames{ false };

namespace {

using VRGetGenericInterfaceFn = void* (*)(const char*, int*);
VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;

using SubmitFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using WaitGetPosesFn = vr::EVRCompositorError (*)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);

struct HookSlotRegistry {
    uint32_t verifiedSlotIdx = 0;
    std::unordered_map<void*, void*> targetToTrampoline;
    std::unordered_map<void*, void*> instanceToTarget;
};

// Separacion de cerrojos:
// - g_installMutex: serializa la instalacion en MinHook (adquirido en InstallOpenVRHooksWithStatus y
//   en HookCompositorInstance desde HookedVRGetGenericInterface; nunca adquirido en los callbacks por frame HookedWaitGetPoses/HookedSubmit).
// - g_registryMutex: protege solo lecturas/escrituras en memoria de HookSlotRegistry y
//   g_loggedDiagInterfaces (nunca mantenido durante llamadas a MinHook, D3D11 ni LogMessage).
// - g_gfxMutex: protege la inicializacion de recursos D3D11 en HookedSubmit.
std::mutex g_installMutex;
std::mutex g_registryMutex;
std::mutex g_gfxMutex;

HookSlotRegistry g_waitGetPosesReg{ 2, {}, {} };
HookSlotRegistry g_submitReg{ 5, {}, {} };
std::unordered_set<std::string> g_loggedDiagInterfaces;
std::atomic<bool> g_openVrHooksInstalled{ false };

RuntimeConfig g_cfg;
FrameLimiterWin g_limiter;
HzTracker g_hzTracker;
NisUpscalerD3D11 g_upscaler;
SharedMemoryClient g_shm;

uint32_t g_lastInW = 0;
uint32_t g_lastInH = 0;
uint32_t g_lastOutW = 0;
uint32_t g_lastOutH = 0;
uint32_t g_lastFormat = 0;
int32_t g_upscaleStatus = 0;
int32_t g_lastHResult = 0;

std::atomic<uint64_t> g_submitFrameCounter{ 0 };
std::atomic<bool> g_phaseResetRequested{ false };

float g_projCenters[2][2] = { { 0.5f, 0.5f }, { 0.5f, 0.5f } };
bool g_projCentersCalculated = false;

struct OpenVROutputTexture {
    ComPtr<ID3D11Texture2D> texture;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct OpenVRContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    OpenVROutputTexture sharedOutput;
};

OpenVRContext g_ovrCtx;

std::string PtrToHex(const void* ptr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%p", ptr);
    return std::string(buf);
}

// Lista explicita de versiones de IVRCompositor verificadas (vtbl[2] = WaitGetPoses, vtbl[5] = Submit).
bool GetVerifiedCompositorSlots(const char* name, uint32_t& outWaitSlot, uint32_t& outSubmitSlot) {
    if (!name) return false;
    if (std::strcmp(name, "IVRCompositor_022") == 0 ||
        std::strcmp(name, "IVRCompositor_026") == 0) {
        outWaitSlot = 2;
        outSubmitSlot = 5;
        return true;
    }
    return false;
}

template <typename FnType>
FnType ResolveSlotTrampoline(const HookSlotRegistry& reg, void* self) {
    if (!self) return nullptr;

    std::lock_guard<std::mutex> lock(g_registryMutex);

    auto itInst = reg.instanceToTarget.find(self);
    if (itInst != reg.instanceToTarget.end()) {
        auto itTramp = reg.targetToTrampoline.find(itInst->second);
        if (itTramp != reg.targetToTrampoline.end()) {
            return reinterpret_cast<FnType>(itTramp->second);
        }
    }

    void** vtbl = *reinterpret_cast<void***>(self);
    if (vtbl) {
        void* candidateTarget = vtbl[reg.verifiedSlotIdx];
        auto itTramp = reg.targetToTrampoline.find(candidateTarget);
        if (itTramp != reg.targetToTrampoline.end()) {
            return reinterpret_cast<FnType>(itTramp->second);
        }
    }

    return nullptr;
}

// Requiere que el llamador mantenga g_installMutex.
// Nunca mantiene g_registryMutex durante MH_CreateHook, MH_EnableHook, MH_RemoveHook ni LogMessage.
bool RegisterAndEnableSlotHookLocked(HookSlotRegistry& reg,
                                     const char* ifaceName,
                                     const char* methodName,
                                     void* iface,
                                     uint32_t slotIdx,
                                     void* detourFn) {
    if (!iface || !detourFn) return false;

    void** vtbl = *reinterpret_cast<void***>(iface);
    if (!vtbl || !vtbl[slotIdx]) return false;

    void* targetFn = vtbl[slotIdx];

    void* existingTrampoline = nullptr;
    bool newlyAssociated = false;
    {
        std::lock_guard<std::mutex> regLock(g_registryMutex);
        auto itExisting = reg.targetToTrampoline.find(targetFn);
        if (itExisting != reg.targetToTrampoline.end()) {
            existingTrampoline = itExisting->second;
            newlyAssociated = (reg.instanceToTarget.find(iface) == reg.instanceToTarget.end());
            reg.instanceToTarget[iface] = targetFn;
        }
    }

    if (existingTrampoline) {
        if (newlyAssociated) {
            LogMessage(LogLevel::Info, "OpenVR Hook: Associated " + std::string(ifaceName) + "::" + methodName +
                       " [iface=" + PtrToHex(iface) + ", vtbl[" + std::to_string(slotIdx) + "]=" + PtrToHex(targetFn) +
                       "] with existing verified trampoline=" + PtrToHex(existingTrampoline));
        }
        return true;
    }

    void* trampoline = nullptr;
    const MH_STATUS stCreate = MH_CreateHook(targetFn, detourFn, &trampoline);
    if (stCreate != MH_OK || !trampoline) {
        LogMessage(LogLevel::Error, "OpenVR Hook: MH_CreateHook failed for " + std::string(ifaceName) + "::" + methodName +
                   " [iface=" + PtrToHex(iface) + ", vtbl[" + std::to_string(slotIdx) + "]=" + PtrToHex(targetFn) +
                   "] status=" + std::string(MH_StatusToString(stCreate)) + ". Leaving unhooked (no fallback).");
        return false;
    }

    {
        std::lock_guard<std::mutex> regLock(g_registryMutex);
        reg.verifiedSlotIdx = slotIdx;
        reg.targetToTrampoline[targetFn] = trampoline;
        reg.instanceToTarget[iface] = targetFn;
    }

    const MH_STATUS stEnable = MH_EnableHook(targetFn);
    if (stEnable != MH_OK && stEnable != MH_ERROR_ENABLED) {
        {
            std::lock_guard<std::mutex> regLock(g_registryMutex);
            reg.targetToTrampoline.erase(targetFn);
            reg.instanceToTarget.erase(iface);
        }
        MH_RemoveHook(targetFn);
        LogMessage(LogLevel::Error, "OpenVR Hook: MH_EnableHook failed for " + std::string(ifaceName) + "::" + methodName +
                   " [target=" + PtrToHex(targetFn) + "] status=" + std::string(MH_StatusToString(stEnable)) + ". Rolled back.");
        return false;
    }

    LogMessage(LogLevel::Info, "OpenVR Hook installed: " + std::string(ifaceName) + "::" + methodName +
               " [iface=" + PtrToHex(iface) + ", vtbl[" + std::to_string(slotIdx) + "]=" + PtrToHex(targetFn) +
               ", trampoline=" + PtrToHex(trampoline) + "]");
    return true;
}

bool IsOpenXrLayerHandlingD3D11() {
    HMODULE hLayer = GetModuleHandleW(L"VRPacingLayer.dll");
    if (hLayer) {
        using IsActiveFn = bool(__cdecl*)();
        auto fn = reinterpret_cast<IsActiveFn>(GetProcAddress(hLayer, "VRPacingLayer_IsActiveD3D11"));
        if (fn && fn()) {
            return true;
        }
    }
    return g_shm.IsOpenXrD3D11ActiveForCurrentProcess();
}

double QueryOpenVRHardwareHz() {
    if (!g_origGetGenericInterface) return 0.0;
    int err = 0;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(g_origGetGenericInterface(vr::IVRSystem_Version, &err));
    if (!sys) return 0.0;
    vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
    const float freq = sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &propErr);
    if (propErr == vr::TrackedProp_Success && freq >= 30.0f && freq <= 200.0f) {
        const double raw = static_cast<double>(freq);
        static constexpr double kKnownHz[] = { 60.0, 72.0, 75.0, 80.0, 90.0, 96.0, 100.0, 120.0, 144.0 };
        for (double khz : kKnownHz) {
            if (std::abs(raw - khz) <= 0.6) {
                return khz;
            }
        }
        return std::round(raw);
    }
    return 0.0;
}

void CalculateOpenVRProjectionCenters() {
    if (g_projCentersCalculated || !g_origGetGenericInterface) return;
    int err = 0;
    auto* sys = reinterpret_cast<vr::IVRSystem*>(g_origGetGenericInterface(vr::IVRSystem_Version, &err));
    if (!sys) return;

    for (int eye = 0; eye < 2; ++eye) {
        float left = -1.0f, right = 1.0f, top = -1.0f, bottom = 1.0f;
        sys->GetProjectionRaw(static_cast<vr::EVREye>(eye), &left, &right, &top, &bottom);

        auto ml = sys->GetEyeToHeadTransform(vr::Eye_Left);
        auto mr = sys->GetEyeToHeadTransform(vr::Eye_Right);
        const float dotForward = ml.m[2][0] * mr.m[2][0] + ml.m[2][1] * mr.m[2][1] + ml.m[2][2] * mr.m[2][2];
        const float clampedDot = (std::max)(-1.0f, (std::min)(1.0f, dotForward));
        const float cantedAngle = std::abs(std::acos(clampedDot) * 0.5f) * (eye == vr::Eye_Right ? -1.0f : 1.0f);
        const float canted = std::tan(cantedAngle);

        if (std::abs(left - right) > 0.001f && std::abs(top - bottom) > 0.001f) {
            g_projCenters[eye][0] = 0.5f * (1.0f + (right + left - 2.0f * canted) / (left - right));
            g_projCenters[eye][1] = 0.5f * (1.0f + (bottom + top) / (top - bottom));
        }
    }
    g_projCentersCalculated = true;
}

void EnsureInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        InitLogging();
        g_cfg = LoadConfig();
        g_cfg.enabled = true;
        g_cfg.upscaling.enabled = true;
        g_cfg.upscaling.scale = 2.0f;
        g_cfg.upscaling.radius = 0.60f;
        g_cfg.upscaling.sharpness = 0.0f;
        g_cfg.pacing.enabled = true;
        g_limiter.Initialize();
        LogMessage(LogLevel::Info, "OpenVR Hook initialized.");
    });
}

vr::EVRCompositorError HookedWaitGetPoses(void* self,
                                         vr::TrackedDevicePose_t* pRenderPoseArray,
                                         uint32_t unRenderPoseArrayCount,
                                         vr::TrackedDevicePose_t* pGamePoseArray,
                                         uint32_t unGamePoseArrayCount) {
    WaitGetPosesFn origFn = ResolveSlotTrampoline<WaitGetPosesFn>(g_waitGetPosesReg, self);
    if (!origFn) {
        static std::atomic<bool> s_loggedMiss{ false };
        if (!s_loggedMiss.exchange(true)) {
            LogMessage(LogLevel::Error, "CRITICAL: Unresolved trampoline in HookedWaitGetPoses for self=" +
                       PtrToHex(self) + ". Returning VRCompositorError_RequestFailed.");
        }
        return vr::VRCompositorError_RequestFailed;
    }

    if (IsOpenXrLayerHandlingD3D11()) {
        return origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    }

    thread_local bool inWait = false;
    if (inWait) {
        return origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);
    }
    inWait = true;

    EnsureInit();
    g_shm.SyncConfig(g_cfg);
    VrsFoveatedD3D11::Instance().EndFrame();

    static int64_t s_qpcFreq = 0;
    static int64_t s_lastWaitExitQpc = 0;
    static uint32_t s_warmupFrames = 0;
    static uint32_t s_blockFrames = 0;
    static double s_blockDtSum = 0.0;
    static uint32_t s_sswBlockStreak = 0;
    static uint64_t s_lastSubmitCounter = 0;
    static uint32_t s_noSubmitStreak = 0;
    static uint32_t s_hzPollCounter = 0;
    static double s_panelHz = 90.0;
    static bool s_sswHalfRateActive = false;

    if (s_qpcFreq == 0) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        s_qpcFreq = f.QuadPart;
    }

    if (s_hzPollCounter == 0) {
        const double hwHz = QueryOpenVRHardwareHz();
        if (hwHz > 0.0 && std::abs(hwHz - s_panelHz) > 0.5) {
            s_panelHz = hwHz;
            s_warmupFrames = 0;
            s_blockFrames = 0;
            s_blockDtSum = 0.0;
            s_sswBlockStreak = 0;
            s_sswHalfRateActive = false;
        }
    }
    s_hzPollCounter = (s_hzPollCounter + 1) % 180;

    const bool manualSubRate = (g_cfg.pacing.frameGenDivisor > 1.05);
    if (manualSubRate && s_sswHalfRateActive) {
        s_sswHalfRateActive = false;
        s_blockFrames = 0;
        s_blockDtSum = 0.0;
        s_sswBlockStreak = 0;
    }

    const uint64_t curSubmits = g_submitFrameCounter.load(std::memory_order_relaxed);
    if (curSubmits == s_lastSubmitCounter) {
        s_noSubmitStreak++;
    } else {
        s_noSubmitStreak = 0;
        s_lastSubmitCounter = curSubmits;
    }

    if (g_phaseResetRequested.exchange(false, std::memory_order_relaxed) || s_noSubmitStreak >= 15) {
        s_warmupFrames = 0;
        s_blockFrames = 0;
        s_blockDtSum = 0.0;
        s_sswBlockStreak = 0;
        s_sswHalfRateActive = false;
    }

    const bool hasSubmitted3D = g_openVrHandlingFrames.load(std::memory_order_relaxed) && (s_noSubmitStreak < 15);
    const double effectiveHz = (!manualSubRate && s_sswHalfRateActive) ? (s_panelHz * 0.5) : s_panelHz;
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled && hasSubmitted3D && s_warmupFrames >= 30) {
        LimitInputs lim{};
        lim.hz = hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        g_limiter.WaitBeforeRuntime();
    }

    g_shm.UpdateTelemetry(L"OpenVR (SteamVR)", hz, targetFps,
                          g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                          g_lastFormat, g_upscaleStatus, g_lastHResult);

    vr::EVRCompositorError res = origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);

    LARGE_INTEGER afterWaitQpc{};
    QueryPerformanceCounter(&afterWaitQpc);

    if (hasSubmitted3D && s_qpcFreq > 0) {
        if (s_lastWaitExitQpc > 0) {
            const double dt = static_cast<double>(afterWaitQpc.QuadPart - s_lastWaitExitQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.250) {
                s_warmupFrames = 0;
                s_blockFrames = 0;
                s_blockDtSum = 0.0;
                s_sswBlockStreak = 0;
                s_sswHalfRateActive = false;
            } else if (dt > 0.003 && dt < 0.100) {
                if (s_warmupFrames < 30) {
                    s_warmupFrames++;
                } else if (!manualSubRate) {
                    s_blockDtSum += dt;
                    s_blockFrames++;

                    if (s_blockFrames >= 45) {
                        const double blockAvgHz = 45.0 / s_blockDtSum;
                        const double halfPanel = s_panelHz * 0.5;
                        s_blockFrames = 0;
                        s_blockDtSum = 0.0;

                        if (!s_sswHalfRateActive) {
                            if (std::abs(blockAvgHz - halfPanel) <= (halfPanel * 0.035)) {
                                s_sswBlockStreak++;
                                if (s_sswBlockStreak >= 2) {
                                    s_sswHalfRateActive = true;
                                    s_sswBlockStreak = 0;
                                    LogMessage(LogLevel::Info, "OpenVR Pacing: SSW half-rate lock detected (avg " +
                                               std::to_string(blockAvgHz) + " FPS -> " +
                                               std::to_string(static_cast<int>(std::lround(halfPanel))) + " Hz / " +
                                               std::to_string(static_cast<int>(std::lround(s_panelHz))) + " Hz panel).");
                                }
                            } else {
                                s_sswBlockStreak = 0;
                            }
                        } else {
                            if (blockAvgHz < (halfPanel * 0.85) || blockAvgHz > (halfPanel * 1.06)) {
                                s_sswHalfRateActive = false;
                                s_sswBlockStreak = 0;
                            }
                        }
                    }
                }
            }
        }
        s_lastWaitExitQpc = afterWaitQpc.QuadPart;
    } else {
        s_lastWaitExitQpc = 0;
    }

    inWait = false;
    return res;
}

vr::EVRCompositorError HookedSubmit(void* self,
                                    vr::EVREye eEye,
                                    const vr::Texture_t* pTexture,
                                    const vr::VRTextureBounds_t* pBounds,
                                    vr::EVRSubmitFlags nSubmitFlags) {
    SubmitFn origFn = ResolveSlotTrampoline<SubmitFn>(g_submitReg, self);
    if (!origFn) {
        static std::atomic<bool> s_loggedMiss{ false };
        if (!s_loggedMiss.exchange(true)) {
            LogMessage(LogLevel::Error, "CRITICAL: Unresolved trampoline in HookedSubmit for self=" +
                       PtrToHex(self) + ". Returning VRCompositorError_RequestFailed.");
        }
        return vr::VRCompositorError_RequestFailed;
    }

    if (IsOpenXrLayerHandlingD3D11()) {
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    thread_local bool inSubmit = false;
    if (inSubmit || !pTexture || !pTexture->handle || pTexture->eType != vr::TextureType_DirectX) {
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    EnsureInit();
    g_shm.SyncConfig(g_cfg);

    inSubmit = true;
    auto* inTex = reinterpret_cast<ID3D11Texture2D*>(pTexture->handle);

    {
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        if (!g_ovrCtx.device) {
            inTex->GetDevice(g_ovrCtx.device.GetAddressOf());
            if (g_ovrCtx.device) {
                g_ovrCtx.device->GetImmediateContext(g_ovrCtx.context.GetAddressOf());
                if (!g_upscaler.IsInitialized()) {
                    if (!g_upscaler.Initialize(g_ovrCtx.device.Get())) {
                        g_upscaleStatus = -2;
                    }
                }
            }
        }
    }

    if (!g_ovrCtx.device || !g_ovrCtx.context) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    D3D11_TEXTURE2D_DESC inDesc{};
    inTex->GetDesc(&inDesc);

    static const vr::VRTextureBounds_t defaultBounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    const vr::VRTextureBounds_t* b = pBounds ? pBounds : &defaultBounds;

    const bool isFlippedX = (b->uMin > b->uMax);
    const bool isFlippedY = (b->vMin > b->vMax);
    const float uMin = (std::min)(b->uMin, b->uMax);
    const float uMax = (std::max)(b->uMin, b->uMax);
    const float vMin = (std::min)(b->vMin, b->vMax);
    const float vMax = (std::max)(b->vMin, b->vMax);

    const uint32_t cropX = static_cast<uint32_t>(std::lround(uMin * inDesc.Width));
    const uint32_t cropY = static_cast<uint32_t>(std::lround(vMin * inDesc.Height));
    const uint32_t cropW = static_cast<uint32_t>(std::lround((uMax - uMin) * inDesc.Width));
    const uint32_t cropH = static_cast<uint32_t>(std::lround((vMax - vMin) * inDesc.Height));

    const bool isSideBySide = (inDesc.ArraySize == 1) && ((uMax - uMin) <= 0.60f) && (inDesc.Width >= (inDesc.Height * 4 / 3));
    const uint32_t fullEyeW = isSideBySide ? (inDesc.Width / 2) : inDesc.Width;
    const uint32_t fullEyeH = inDesc.Height;

    // Ignorar mini-buffers de pantallas de carga (< 1000px) para evitar realocar VRAM
    if (fullEyeW < 1000 || fullEyeH < 1000 || cropW < 600 || cropH < 600) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    static uint32_t s_prevFullEyeW = 0;
    static uint32_t s_prevFullEyeH = 0;
    if (s_prevFullEyeW != 0 && (s_prevFullEyeW != fullEyeW || s_prevFullEyeH != fullEyeH)) {
        g_phaseResetRequested.store(true, std::memory_order_relaxed);
    }
    s_prevFullEyeW = fullEyeW;
    s_prevFullEyeH = fullEyeH;

    g_submitFrameCounter.fetch_add(1, std::memory_order_relaxed);
    g_openVrHandlingFrames.store(true, std::memory_order_relaxed);
    g_lastFormat = static_cast<uint32_t>(inDesc.Format);
    g_lastInW = cropW;
    g_lastInH = cropH;

    CalculateOpenVRProjectionCenters();
    VrsTextureMode vrsMode = VrsTextureMode::SingleEye;
    if (inDesc.ArraySize >= 2) {
        vrsMode = VrsTextureMode::TextureArray;
    } else if (isSideBySide) {
        vrsMode = VrsTextureMode::CombinedSideBySide;
    }
    VrsFoveatedD3D11::Instance().UpdateTarget(
        g_ovrCtx.device.Get(),
        g_ovrCtx.context.Get(),
        g_cfg.ffr.enabled,
        fullEyeW,
        fullEyeH,
        vrsMode,
        g_projCenters[0][0], g_projCenters[0][1],
        g_projCenters[1][0], g_projCenters[1][1]);

    if (!g_cfg.upscaling.enabled || (g_cfg.upscaling.scale <= 1.0f)) {
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    const double s = g_cfg.upscaling.scale;
    uint32_t maxOutW = static_cast<uint32_t>((std::max)(1.0, std::floor(fullEyeW * s)));
    uint32_t maxOutH = static_cast<uint32_t>((std::max)(1.0, std::floor(fullEyeH * s)));

    const uint64_t maxPx = static_cast<uint64_t>(maxOutW) * static_cast<uint64_t>(maxOutH);
    if (maxPx > g_cfg.upscaling.maxOutputPixels) {
        const double factor = std::sqrt(static_cast<double>(g_cfg.upscaling.maxOutputPixels) / static_cast<double>(maxPx));
        maxOutW = static_cast<uint32_t>((std::max)(1.0, std::floor(maxOutW * factor)));
        maxOutH = static_cast<uint32_t>((std::max)(1.0, std::floor(maxOutH * factor)));
    }

    const double effScaleX = static_cast<double>(maxOutW) / static_cast<double>(fullEyeW);
    const double effScaleY = static_cast<double>(maxOutH) / static_cast<double>(fullEyeH);
    const uint32_t outW = (std::min)(maxOutW, static_cast<uint32_t>((std::max)(1L, std::lround(cropW * effScaleX))));
    const uint32_t outH = (std::min)(maxOutH, static_cast<uint32_t>((std::max)(1L, std::lround(cropH * effScaleY))));

    g_lastOutW = outW;
    g_lastOutH = outH;

    static std::atomic<bool> s_firstFrameLogged{ false };
    if (!s_firstFrameLogged.exchange(true)) {
        LogMessage(LogLevel::Info, "OpenVR [Submit] First frame: InTex=" + std::to_string(inDesc.Width) + "x" + std::to_string(inDesc.Height) +
                   " FMT=" + std::to_string(inDesc.Format) +
                   " ArraySize=" + std::to_string(inDesc.ArraySize) +
                   " Flags=" + std::to_string(nSubmitFlags) +
                   " Crop=" + std::to_string(cropW) + "x" + std::to_string(cropH) +
                   " -> Out=" + std::to_string(outW) + "x" + std::to_string(outH));
    }

    auto& sharedOut = g_ovrCtx.sharedOutput;

    {
        std::lock_guard<std::mutex> lock(g_gfxMutex);
        if (!sharedOut.texture || sharedOut.width != maxOutW || sharedOut.height != maxOutH || sharedOut.format != inDesc.Format) {
            sharedOut.texture.Reset();

            D3D11_TEXTURE2D_DESC outDesc{};
            outDesc.Width = maxOutW;
            outDesc.Height = maxOutH;
            outDesc.MipLevels = 1;
            outDesc.ArraySize = 1;
            outDesc.Format = inDesc.Format;
            outDesc.SampleDesc.Count = 1;
            outDesc.SampleDesc.Quality = 0;
            outDesc.Usage = D3D11_USAGE_DEFAULT;
            outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            outDesc.CPUAccessFlags = 0;
            outDesc.MiscFlags = 0;

            HRESULT hr = g_ovrCtx.device->CreateTexture2D(&outDesc, nullptr, sharedOut.texture.GetAddressOf());
            if (FAILED(hr)) {
                g_upscaleStatus = -1;
                g_lastHResult = hr;
                LogMessage(LogLevel::Error, "OpenVR Hook: Failed to allocate shared output texture. HR=" + std::to_string(hr));
                inSubmit = false;
                return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
            }
            sharedOut.width = maxOutW;
            sharedOut.height = maxOutH;
            sharedOut.format = inDesc.Format;
            LogMessage(LogLevel::Info, "OpenVR: Shared output texture allocated: " + std::to_string(maxOutW) + "x" + std::to_string(maxOutH));
        }
    }

    ID3D11RenderTargetView* nullRTV[1] = { nullptr };
    g_ovrCtx.context->OMSetRenderTargets(1, nullRTV, nullptr);

    const int eyeIdx = (eEye == vr::Eye_Right) ? 1 : 0;
    UpscaleDispatchDesc dispatch{};
    dispatch.srcRect = { cropX, cropY, cropW, cropH };
    dispatch.dstRect = { 0, 0, outW, outH };
    dispatch.srcArrayIndex = (inDesc.ArraySize > 1) ? eyeIdx : 0;
    dispatch.dstArrayIndex = 0;
    dispatch.projCenterX = g_projCenters[eyeIdx][0];
    dispatch.projCenterY = g_projCenters[eyeIdx][1];
    dispatch.radius = g_cfg.upscaling.radius;
    dispatch.sharpness = g_cfg.upscaling.sharpness;
    dispatch.maskEnabled = false;
    dispatch.maxOutputPixels = g_cfg.upscaling.maxOutputPixels;

    if (!g_upscaler.Upscale(g_ovrCtx.context.Get(), inTex, sharedOut.texture.Get(), dispatch)) {
        g_upscaleStatus = -3;
        inSubmit = false;
        return origFn(self, eEye, pTexture, pBounds, nSubmitFlags);
    }

    g_upscaleStatus = 1;

    const float uMaxOut = static_cast<float>(outW) / static_cast<float>(sharedOut.width);
    const float vMaxOut = static_cast<float>(outH) / static_cast<float>(sharedOut.height);

    vr::VRTextureBounds_t outBounds{};
    outBounds.uMin = isFlippedX ? uMaxOut : 0.0f;
    outBounds.uMax = isFlippedX ? 0.0f : uMaxOut;
    outBounds.vMin = isFlippedY ? vMaxOut : 0.0f;
    outBounds.vMax = isFlippedY ? 0.0f : vMaxOut;

    auto* mutableTex = const_cast<vr::Texture_t*>(pTexture);
    void* origHandle = mutableTex->handle;
    mutableTex->handle = sharedOut.texture.Get();
    const uint32_t clearFlags = static_cast<uint32_t>(nSubmitFlags) & ~static_cast<uint32_t>(vr::Submit_GlRenderBuffer);
    vr::EVRCompositorError res = origFn(self, eEye, mutableTex, &outBounds, static_cast<vr::EVRSubmitFlags>(clearFlags));
    mutableTex->handle = origHandle;

    inSubmit = false;
    return res;
}

void HookCompositorInstance(const char* name, void* iface) {
    if (!iface || !name) return;

    uint32_t waitSlot = 0;
    uint32_t submitSlot = 0;
    if (!GetVerifiedCompositorSlots(name, waitSlot, submitSlot)) {
        LogMessage(LogLevel::Warn, "OpenVR Hook: Unverified IVRCompositor version \"" +
                   std::string(name) + "\". Leaving in passthrough.");
        return;
    }

    std::lock_guard<std::mutex> installLock(g_installMutex);
    MH_Initialize();

    RegisterAndEnableSlotHookLocked(g_waitGetPosesReg, name, "WaitGetPoses", iface, waitSlot,
                                    reinterpret_cast<void*>(&HookedWaitGetPoses));
    RegisterAndEnableSlotHookLocked(g_submitReg, name, "Submit", iface, submitSlot,
                                    reinterpret_cast<void*>(&HookedSubmit));
}

void* HookedVRGetGenericInterface(const char* name, int* error) {
    void* iface = g_origGetGenericInterface(name, error);
    if (!name) {
        return iface;
    }

    bool shouldLogDiag = false;
    {
        std::lock_guard<std::mutex> regLock(g_registryMutex);
        std::string diagKey = std::string(name) + "@" + PtrToHex(iface);
        if (g_loggedDiagInterfaces.find(diagKey) == g_loggedDiagInterfaces.end()) {
            g_loggedDiagInterfaces.insert(diagKey);
            shouldLogDiag = true;
        }
    }

    if (shouldLogDiag) {
        const int errCode = error ? *error : 0;
        LogMessage(LogLevel::Info, "OpenVR [DIAG] VR_GetGenericInterface: name=\"" + std::string(name) +
                   "\" -> iface=" + PtrToHex(iface) + " (err=" + std::to_string(errCode) + ")");
    }

    if (!iface) {
        return nullptr;
    }

    std::string sName(name);
    if (sName.rfind("IVRCompositor_", 0) == 0) {
        HookCompositorInstance(name, iface);
    } else if (sName.rfind("FnTable:IVRCompositor_", 0) == 0) {
        const char* cppName = name + 8;
        int dummyErr = 0;
        void* cppIface = g_origGetGenericInterface(cppName, &dummyErr);
        if (cppIface) {
            HookCompositorInstance(cppName, cppIface);
        }
    }
    return iface;
}

} // namespace

bool InstallOpenVRHooksWithStatus() {
    if (g_openVrHooksInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> installLock(g_installMutex);
    if (g_openVrHooksInstalled.load(std::memory_order_relaxed)) {
        return true;
    }

    HMODULE mod = GetModuleHandleW(L"openvr_api.dll");
    if (!mod) return false;

    auto fn = reinterpret_cast<VRGetGenericInterfaceFn>(GetProcAddress(mod, "VR_GetGenericInterface"));
    if (!fn) {
        LogMessage(LogLevel::Error, "OpenVR Hook: VR_GetGenericInterface export not found in openvr_api.dll.");
        return false;
    }

    const MH_STATUS stInit = MH_Initialize();
    if (stInit != MH_OK && stInit != MH_ERROR_ALREADY_INITIALIZED) {
        LogMessage(LogLevel::Error, "OpenVR Hook: MH_Initialize failed status=" + std::string(MH_StatusToString(stInit)));
        return false;
    }

    void* origFn = nullptr;
    const MH_STATUS stCreate = MH_CreateHook(reinterpret_cast<void*>(fn),
                                             reinterpret_cast<void*>(&HookedVRGetGenericInterface),
                                             &origFn);
    if (stCreate != MH_OK || !origFn) {
        LogMessage(LogLevel::Error, "OpenVR Hook: MH_CreateHook failed on VR_GetGenericInterface status=" +
                   std::string(MH_StatusToString(stCreate)));
        return false;
    }

    g_origGetGenericInterface = reinterpret_cast<VRGetGenericInterfaceFn>(origFn);
    const MH_STATUS stEnable = MH_EnableHook(reinterpret_cast<void*>(fn));
    if (stEnable != MH_OK && stEnable != MH_ERROR_ENABLED) {
        g_origGetGenericInterface = nullptr;
        MH_RemoveHook(reinterpret_cast<void*>(fn));
        LogMessage(LogLevel::Error, "OpenVR Hook: MH_EnableHook failed on VR_GetGenericInterface status=" +
                   std::string(MH_StatusToString(stEnable)));
        return false;
    }

    g_openVrHooksInstalled.store(true, std::memory_order_release);
    LogMessage(LogLevel::Info, "OpenVR VR_GetGenericInterface hook installed [target=" +
               PtrToHex(reinterpret_cast<const void*>(fn)) + ", trampoline=" + PtrToHex(origFn) + "].");
    return true;
}

void InstallOpenVRHooks() {
    (void)InstallOpenVRHooksWithStatus();
}

} // namespace vrpacing

#endif