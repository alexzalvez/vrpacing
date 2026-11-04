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
#include <vector>

#include "minhook/include/MinHook.h"
#include "common/config_json.h"
#include "common/frame_limiter_win.h"
#include "common/logging_win.h"
#include "common/shared_memory_win.h"
#include "common/visibility_mask.h"
#include "gfx/nis_upscaler_d3d11.h"
#include "gfx/vrs_foveated_d3d11.h"
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"
#include "openvr/hook_openvr.h"
#include "openvr/openvr_slots.h"

using Microsoft::WRL::ComPtr;

namespace vrpacing {

std::atomic<bool> g_openVrHandlingFrames{ false };

namespace {

using VRGetGenericInterfaceFn = void* (*)(const char*, int*);
VRGetGenericInterfaceFn g_origGetGenericInterface = nullptr;

using SubmitFn = vr::EVRCompositorError (*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);
using WaitGetPosesFn = vr::EVRCompositorError (*)(void*, vr::TrackedDevicePose_t*, uint32_t, vr::TrackedDevicePose_t*, uint32_t);

using GetHiddenAreaMeshFn = vr::HiddenAreaMesh_t* (__fastcall*)(void*, vr::HiddenAreaMesh_t*, vr::EVREye, vr::EHiddenAreaMeshType);
using GetHiddenAreaMeshNoTypeFn = vr::HiddenAreaMesh_t* (__fastcall*)(void*, vr::HiddenAreaMesh_t*, vr::EVREye);

using FnTableGetHiddenAreaMeshFn = vr::HiddenAreaMesh_t* (__fastcall*)(vr::HiddenAreaMesh_t*, vr::EVREye, vr::EHiddenAreaMeshType);
using FnTableGetHiddenAreaMeshNoTypeFn = vr::HiddenAreaMesh_t* (__fastcall*)(vr::HiddenAreaMesh_t*, vr::EVREye);

using GetProjectionRawFn = void (__fastcall*)(void*, vr::EVREye, float*, float*, float*, float*);
using FnTableGetProjectionRawFn = void (__fastcall*)(vr::EVREye, float*, float*, float*, float*);

struct VtableSlotRecord {
    uint32_t slot = 0;
    int32_t projRawSlot = -1;
    void* target = nullptr;
    void* trampoline = nullptr;
    bool hamHasType = true;
    std::string versionString;
};

struct HookSlotRegistry {
    std::unordered_map<void**, VtableSlotRecord> vtableToRecord;
    std::unordered_map<void*, void*> targetToTrampoline;
    std::unordered_map<void*, void*> instanceToTarget;
};

std::mutex g_installMutex;
std::mutex g_registryMutex;
std::mutex g_gfxMutex;

HookSlotRegistry g_waitGetPosesReg{};
HookSlotRegistry g_submitReg{};
HookSlotRegistry g_hamReg{};

void* g_fnTableHamTrampoline = nullptr;
void* g_fnTableProjRawFn = nullptr;
void* g_fnTableAssociatedCppIface = nullptr;
int32_t g_fnTableProjRawSlot = -1;
bool g_fnTableHamHasType = true;
std::string g_fnTableVersionStr;

std::atomic<bool> g_hamLatchedPassthrough{ false };
thread_local int g_hamReentrancyDepth = 0;

std::unordered_set<std::string> g_loggedDiagInterfaces;
std::unordered_set<std::string> g_loggedUnsupportedVersions;
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
std::atomic<int> g_hamEntryLogCount{ 0 };
std::atomic<int> g_hamSmallTrisDumpCount{ 0 };

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

struct StaticHamCache {
    bool built[2][3]{};
    std::vector<vr::HmdVector2_t> verts[2][3];
    uint32_t triCounts[2][3]{};
};

StaticHamCache g_hamCache;
std::mutex g_hamMutex;
bool g_hamPassiveLogged[2][3]{};
bool g_hamReplaceLogged[2][3]{};

std::string PtrToHex(const void* ptr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%p", ptr);
    return std::string(buf);
}

// [Z3] Check whether debug logging is explicitly enabled in config
bool IsDebugLoggingEnabled() {
    const std::string& lvl = g_cfg.logging.level;
    return (lvl == "debug" || lvl == "DEBUG" || lvl == "Debug");
}

bool SehCallVtableGHAM(void* tramp, void* self, vr::HiddenAreaMesh_t* outMesh,
                       vr::EVREye eye, vr::EHiddenAreaMeshType type, bool hasType,
                       vr::HiddenAreaMesh_t** outRet, DWORD* outCode) noexcept {
    __try {
        if (hasType) {
            auto fn = reinterpret_cast<GetHiddenAreaMeshFn>(tramp);
            *outRet = fn(self, outMesh, eye, type);
        } else {
            auto fnNoType = reinterpret_cast<GetHiddenAreaMeshNoTypeFn>(tramp);
            *outRet = fnNoType(self, outMesh, eye);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return false;
    }
}

bool SehCallFnTableGHAM(void* tramp, vr::HiddenAreaMesh_t* outMesh,
                        vr::EVREye eye, vr::EHiddenAreaMeshType type, bool hasType,
                        vr::HiddenAreaMesh_t** outRet, DWORD* outCode) noexcept {
    __try {
        if (hasType) {
            auto fn = reinterpret_cast<FnTableGetHiddenAreaMeshFn>(tramp);
            *outRet = fn(outMesh, eye, type);
        } else {
            auto fnNoType = reinterpret_cast<FnTableGetHiddenAreaMeshNoTypeFn>(tramp);
            *outRet = fnNoType(outMesh, eye);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return false;
    }
}

bool SehCallGetProjectionRaw(void* cppSelf, int32_t projRawSlot, void* fnTableProjRaw,
                             vr::EVREye eye, float* l, float* r, float* t, float* b,
                             DWORD* outCode) noexcept {
    __try {
        if (cppSelf && projRawSlot >= 0) {
            void** vtbl = *reinterpret_cast<void***>(cppSelf);
            if (vtbl && vtbl[projRawSlot]) {
                auto fn = reinterpret_cast<GetProjectionRawFn>(vtbl[projRawSlot]);
                fn(cppSelf, eye, l, r, t, b);
                return true;
            }
        }
        if (fnTableProjRaw) {
            auto fnC = reinterpret_cast<FnTableGetProjectionRawFn>(fnTableProjRaw);
            fnC(eye, l, r, t, b);
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (outCode) *outCode = GetExceptionCode();
        return false;
    }
}

void LogSehAndLatchPassthrough(const char* fnName, DWORD code) {
    g_hamLatchedPassthrough.store(true, std::memory_order_relaxed);
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "OpenVR HAM: SEH 0x%08X in %s", static_cast<uint32_t>(code), fnName);
    LogMessage(LogLevel::Error, buf);
}

bool IsOriginalMeshEmpty(const vr::HiddenAreaMesh_t* mesh, vr::EHiddenAreaMeshType type) {
    if (!mesh || !mesh->pVertexData || mesh->unTriangleCount == 0) {
        return true;
    }
    if (type == vr::k_eHiddenAreaMesh_LineLoop) {
        if (mesh->unTriangleCount < 3) return true;
        const float u0 = mesh->pVertexData[0].v[0];
        const float v0 = mesh->pVertexData[0].v[1];
        for (uint32_t i = 1; i < mesh->unTriangleCount; ++i) {
            if (std::abs(mesh->pVertexData[i].v[0] - u0) > 1e-6f ||
                std::abs(mesh->pVertexData[i].v[1] - v0) > 1e-6f) {
                return false;
            }
        }
        return true;
    }

    for (uint32_t t = 0; t < mesh->unTriangleCount; ++t) {
        const auto& p0 = mesh->pVertexData[t * 3 + 0];
        const auto& p1 = mesh->pVertexData[t * 3 + 1];
        const auto& p2 = mesh->pVertexData[t * 3 + 2];
        const bool same01 = (std::abs(p0.v[0] - p1.v[0]) <= 1e-6f) && (std::abs(p0.v[1] - p1.v[1]) <= 1e-6f);
        const bool same02 = (std::abs(p0.v[0] - p2.v[0]) <= 1e-6f) && (std::abs(p0.v[1] - p2.v[1]) <= 1e-6f);
        if (!same01 || !same02) {
            return false;
        }
    }
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

bool IsOpenXrSessionActiveAnyApi() {
    HMODULE hLayer = GetModuleHandleW(L"VRPacingLayer.dll");
    if (hLayer) {
        using IsActiveFn = bool(__cdecl*)();
        auto fn = reinterpret_cast<IsActiveFn>(GetProcAddress(hLayer, "VRPacingLayer_IsActiveSession"));
        if (fn && fn()) {
            return true;
        }
    }
    return IsOpenXrLayerHandlingD3D11();
}

template <typename FnType>
FnType ResolveSlotTrampoline(const HookSlotRegistry& reg, void* self, VtableSlotRecord* outRec = nullptr) {
    if (!self) return nullptr;

    std::lock_guard<std::mutex> lock(g_registryMutex);

    void** vtbl = *reinterpret_cast<void***>(self);
    if (vtbl) {
        auto itVtbl = reg.vtableToRecord.find(vtbl);
        if (itVtbl != reg.vtableToRecord.end()) {
            if (outRec) *outRec = itVtbl->second;
            return reinterpret_cast<FnType>(itVtbl->second.trampoline);
        }
    }

    auto itInst = reg.instanceToTarget.find(self);
    if (itInst != reg.instanceToTarget.end()) {
        auto itTramp = reg.targetToTrampoline.find(itInst->second);
        if (itTramp != reg.targetToTrampoline.end()) {
            return reinterpret_cast<FnType>(itTramp->second);
        }
    }

    return nullptr;
}

bool RegisterAndEnableSlotHookLocked(HookSlotRegistry& reg,
                                     const char* ifaceName,
                                     const char* methodName,
                                     void* iface,
                                     uint32_t slotIdx,
                                     void* detourFn,
                                     bool hamHasType = true,
                                     int32_t projRawSlot = -1) {
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
            newlyAssociated = (reg.vtableToRecord.find(vtbl) == reg.vtableToRecord.end());
            reg.instanceToTarget[iface] = targetFn;
            reg.vtableToRecord[vtbl] = VtableSlotRecord{ slotIdx, projRawSlot, targetFn, existingTrampoline, hamHasType, std::string(ifaceName) };
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
        reg.targetToTrampoline[targetFn] = trampoline;
        reg.instanceToTarget[iface] = targetFn;
        reg.vtableToRecord[vtbl] = VtableSlotRecord{ slotIdx, projRawSlot, targetFn, trampoline, hamHasType, std::string(ifaceName) };
    }

    const MH_STATUS stEnable = MH_EnableHook(targetFn);
    if (stEnable != MH_OK && stEnable != MH_ERROR_ENABLED) {
        {
            std::lock_guard<std::mutex> regLock(g_registryMutex);
            reg.targetToTrampoline.erase(targetFn);
            reg.instanceToTarget.erase(iface);
            reg.vtableToRecord.erase(vtbl);
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
        LogMessage(LogLevel::Info, "OpenVR Hook initialized (mask.openvrMode=" +
                   std::to_string(g_cfg.mask.openvrMode) + ", mask.openvrForce=" +
                   std::to_string(g_cfg.mask.openvrForce) + ").");
    });
}

// [Z2, Z3] Release defaults: LineLoop probe/dump only when debug logging is enabled;
// mode=2 replaces ONLY when original=EMPTY unless mask.openvrForce != 0.
template <typename CallOrigFn>
vr::HiddenAreaMesh_t* HandleHiddenAreaMeshShared(
    void* cppSelf,
    int32_t projRawSlot,
    void* fnTableProjRaw,
    vr::HiddenAreaMesh_t* ret,
    vr::EVREye eye,
    vr::EHiddenAreaMeshType effectiveType,
    bool hamHasType,
    const std::string& versionStr,
    CallOrigFn&& callOrigSeh) {
    EnsureInit();
    const int mode = g_hamLatchedPassthrough.load(std::memory_order_relaxed) ? 0 : g_cfg.mask.openvrMode;

    const int entryNum = g_hamEntryLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (entryNum <= 8) {
        LogMessage(LogLevel::Info, "OpenVR HAM entry#" + std::to_string(entryNum) +
                   " [" + (versionStr.empty() ? "unresolved" : versionStr) +
                   "] eye=" + std::to_string(static_cast<int>(eye)) +
                   " type=" + std::to_string(static_cast<int>(effectiveType)) +
                   " mode=" + std::to_string(mode));
    }

    DWORD sehCode = 0;
    if (IsOpenXrSessionActiveAnyApi() || !ret || mode <= 0) {
        vr::HiddenAreaMesh_t* passthroughRes = ret;
        if (!callOrigSeh(ret, effectiveType, &passthroughRes, &sehCode)) {
            LogSehAndLatchPassthrough("original GHAM", sehCode);
        }
        return passthroughRes;
    }

    if (projRawSlot < 0 && !fnTableProjRaw) {
        LogMessage(LogLevel::Warn, "OpenVR HAM: unknown GetProjectionRaw slot for [" + versionStr + "], falling back to passthrough");
        vr::HiddenAreaMesh_t* passthroughRes = ret;
        if (!callOrigSeh(ret, effectiveType, &passthroughRes, &sehCode)) {
            LogSehAndLatchPassthrough("original GHAM", sehCode);
        }
        return passthroughRes;
    }

    const int eyeIdx = (eye == vr::Eye_Right) ? 1 : 0;
    int typeIdx = 0;
    VisibilityMaskKind maskKind = VisibilityMaskKind::HiddenTriangles;
    if (effectiveType == vr::k_eHiddenAreaMesh_Inverse) {
        typeIdx = 1;
        maskKind = VisibilityMaskKind::VisibleTriangles;
    } else if (effectiveType == vr::k_eHiddenAreaMesh_LineLoop) {
        typeIdx = 2;
        maskKind = VisibilityMaskKind::LineLoop;
    }

    // [Z3] LineLoop probe and vertex dump only when debug logging is enabled
    const bool debugLog = IsDebugLoggingEnabled();
    const bool wantSmallDump = debugLog && (g_hamSmallTrisDumpCount.load(std::memory_order_relaxed) < 2);
    vr::HiddenAreaMesh_t loopMesh{};
    bool loopProbedOk = false;
    if (wantSmallDump && hamHasType && effectiveType != vr::k_eHiddenAreaMesh_LineLoop) {
        vr::HiddenAreaMesh_t* loopRet = &loopMesh;
        if (callOrigSeh(&loopMesh, vr::k_eHiddenAreaMesh_LineLoop, &loopRet, &sehCode)) {
            loopProbedOk = true;
        } else {
            LogSehAndLatchPassthrough("probe LineLoop", sehCode);
            return ret;
        }
    }

    float lu0 = 0.0f, lv0 = 0.0f, lu1 = 0.0f, lv1 = 0.0f;
    uint32_t loopTris = 0;
    if (loopProbedOk) {
        loopTris = loopMesh.unTriangleCount;
        if (loopMesh.pVertexData && loopTris > 0) {
            lu0 = lu1 = loopMesh.pVertexData[0].v[0];
            lv0 = lv1 = loopMesh.pVertexData[0].v[1];
            for (uint32_t i = 1; i < loopTris; ++i) {
                lu0 = (std::min)(lu0, loopMesh.pVertexData[i].v[0]);
                lu1 = (std::max)(lu1, loopMesh.pVertexData[i].v[0]);
                lv0 = (std::min)(lv0, loopMesh.pVertexData[i].v[1]);
                lv1 = (std::max)(lv1, loopMesh.pVertexData[i].v[1]);
            }
        }
    }

    // [Z3] Single call to the original per HAM request in release
    vr::HiddenAreaMesh_t* origRes = ret;
    if (!callOrigSeh(ret, effectiveType, &origRes, &sehCode)) {
        LogSehAndLatchPassthrough("original GHAM", sehCode);
        return ret;
    }

    const bool isEmpty = IsOriginalMeshEmpty(origRes, effectiveType);

    {
        std::lock_guard<std::mutex> lock(g_hamMutex);
        const bool firstForPair = !g_hamPassiveLogged[eyeIdx][typeIdx];
        g_hamPassiveLogged[eyeIdx][typeIdx] = true;

        const uint32_t tris = origRes ? origRes->unTriangleCount : 0;
        const uint32_t vertCount = (effectiveType == vr::k_eHiddenAreaMesh_LineLoop) ? tris : (tris * 3);
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
        if (origRes && origRes->pVertexData && vertCount > 0) {
            u0 = u1 = origRes->pVertexData[0].v[0];
            v0 = v1 = origRes->pVertexData[0].v[1];
            for (uint32_t i = 1; i < vertCount; ++i) {
                const float u = origRes->pVertexData[i].v[0];
                const float v = origRes->pVertexData[i].v[1];
                u0 = (std::min)(u0, u);
                u1 = (std::max)(u1, u);
                v0 = (std::min)(v0, v);
                v1 = (std::max)(v1, v);
            }
        }

        if (firstForPair) {
            LogMessage(LogLevel::Info, std::string("OpenVR HAM original=") + (isEmpty ? "EMPTY" : "MESH"));

            char bboxBuf[128]{};
            std::snprintf(bboxBuf, sizeof(bboxBuf), "[%.4f,%.4f,%.4f,%.4f]", u0, v0, u1, v1);
            LogMessage(LogLevel::Info, "OpenVR HAM [" + versionStr + "] eye=" + std::to_string(eyeIdx) +
                       " type=" + std::to_string(static_cast<int>(effectiveType)) +
                       " tris=" + std::to_string(tris) + " bbox[minX,minY,maxX,maxY]=" + std::string(bboxBuf));
        }

        if (debugLog && origRes && origRes->pVertexData && tris <= 4) {
            const int dumpIdx = g_hamSmallTrisDumpCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (dumpIdx <= 2) {
                if (loopProbedOk) {
                    char lBbox[128]{};
                    std::snprintf(lBbox, sizeof(lBbox), "[%.4f,%.4f,%.4f,%.4f]", lu0, lv0, lu1, lv1);
                    LogMessage(LogLevel::Info, "OpenVR HAM probe LineLoop [" + versionStr + "] eye=" +
                               std::to_string(eyeIdx) + " tris=" + std::to_string(loopTris) +
                               " bbox[minX,minY,maxX,maxY]=" + std::string(lBbox));
                }
                std::string vDump = "OpenVR HAM dump#" + std::to_string(dumpIdx) + " [" + versionStr +
                                    "] eye=" + std::to_string(eyeIdx) + " tris=" + std::to_string(tris) + " verts:";
                for (uint32_t i = 0; i < vertCount; ++i) {
                    char vBuf[64]{};
                    std::snprintf(vBuf, sizeof(vBuf), " (%.4f,%.4f)", origRes->pVertexData[i].v[0], origRes->pVertexData[i].v[1]);
                    vDump += vBuf;
                }
                LogMessage(LogLevel::Info, vDump);
            }
        }
    }

    if (mode == 1) {
        return origRes;
    }

    // [Z2] In mode=2, replace ONLY when original=EMPTY unless mask.openvrForce != 0
    if (!isEmpty && g_cfg.mask.openvrForce == 0) {
        std::lock_guard<std::mutex> lock(g_hamMutex);
        if (!g_hamReplaceLogged[eyeIdx][typeIdx]) {
            g_hamReplaceLogged[eyeIdx][typeIdx] = true;
            LogMessage(LogLevel::Info, "OpenVR HAM passthrough (original=MESH, openvrForce=0) eye=" +
                       std::to_string(eyeIdx) + " type=" + std::to_string(static_cast<int>(effectiveType)));
        }
        return origRes;
    }

    std::lock_guard<std::mutex> lock(g_hamMutex);
    if (!g_hamCache.built[eyeIdx][typeIdx]) {
        float l = -1.0f, r = 1.0f, t = -1.0f, b = 1.0f;
        if (!SehCallGetProjectionRaw(cppSelf, projRawSlot, fnTableProjRaw, eye, &l, &r, &t, &b, &sehCode)) {
            LogSehAndLatchPassthrough("GetProjectionRaw", sehCode);
            return origRes;
        }

        uint32_t triCount = 0;
        std::vector<MaskVec2> uvVerts = GenerateOpenVrFlatTriangleVerticesUV(maskKind, l, r, t, b, 256, triCount);
        g_hamCache.verts[eyeIdx][typeIdx].resize(uvVerts.size());
        for (size_t i = 0; i < uvVerts.size(); ++i) {
            g_hamCache.verts[eyeIdx][typeIdx][i].v[0] = uvVerts[i].x;
            g_hamCache.verts[eyeIdx][typeIdx][i].v[1] = uvVerts[i].y;
        }
        g_hamCache.triCounts[eyeIdx][typeIdx] = triCount;
        g_hamCache.built[eyeIdx][typeIdx] = true;
    }

    if (g_hamCache.verts[eyeIdx][typeIdx].empty()) {
        LogMessage(LogLevel::Warn, "OpenVR HAM mode=2 return: generated vertex cache is empty for eye=" + std::to_string(eyeIdx));
        return origRes;
    }

    ret->pVertexData = g_hamCache.verts[eyeIdx][typeIdx].data();
    ret->unTriangleCount = g_hamCache.triCounts[eyeIdx][typeIdx];

    if (!g_hamReplaceLogged[eyeIdx][typeIdx]) {
        g_hamReplaceLogged[eyeIdx][typeIdx] = true;
        LogMessage(LogLevel::Info, "OpenVR HAM replaced eye=" + std::to_string(eyeIdx) +
                   " type=" + std::to_string(static_cast<int>(effectiveType)) +
                   " tris=" + std::to_string(ret->unTriangleCount));
    }

    return ret;
}

vr::HiddenAreaMesh_t* __fastcall Hooked_GetHiddenAreaMesh(void* self,
                                                          vr::HiddenAreaMesh_t* ret,
                                                          vr::EVREye eye,
                                                          vr::EHiddenAreaMeshType type) {
    VtableSlotRecord rec{};
    auto origFn = ResolveSlotTrampoline<GetHiddenAreaMeshFn>(g_hamReg, self, &rec);
    if (!origFn) {
        if (ret) {
            ret->pVertexData = nullptr;
            ret->unTriangleCount = 0;
        }
        return ret;
    }

    const vr::EHiddenAreaMeshType effectiveType = rec.hamHasType ? type : vr::k_eHiddenAreaMesh_Standard;

    if (g_hamReentrancyDepth > 0) {
        vr::HiddenAreaMesh_t* directRet = ret;
        DWORD dummyCode = 0;
        SehCallVtableGHAM(reinterpret_cast<void*>(origFn), self, ret, eye, effectiveType, rec.hamHasType, &directRet, &dummyCode);
        return directRet;
    }

    ++g_hamReentrancyDepth;
    auto callOrigSeh = [&](vr::HiddenAreaMesh_t* outPtr, vr::EHiddenAreaMeshType t,
                           vr::HiddenAreaMesh_t** outRet, DWORD* outCode) -> bool {
        return SehCallVtableGHAM(reinterpret_cast<void*>(origFn), self, outPtr, eye, t, rec.hamHasType, outRet, outCode);
    };

    vr::HiddenAreaMesh_t* res = HandleHiddenAreaMeshShared(
        self, rec.projRawSlot, nullptr, ret, eye, effectiveType, rec.hamHasType, rec.versionString, callOrigSeh);
    --g_hamReentrancyDepth;
    return res;
}

vr::HiddenAreaMesh_t* __fastcall Hooked_FnTable_GetHiddenAreaMesh(vr::HiddenAreaMesh_t* ret,
                                                                  vr::EVREye eye,
                                                                  vr::EHiddenAreaMeshType type) {
    void* tramp = g_fnTableHamTrampoline;
    if (!tramp) {
        if (ret) {
            ret->pVertexData = nullptr;
            ret->unTriangleCount = 0;
        }
        return ret;
    }

    const vr::EHiddenAreaMeshType effectiveType = g_fnTableHamHasType ? type : vr::k_eHiddenAreaMesh_Standard;

    if (g_hamReentrancyDepth > 0) {
        vr::HiddenAreaMesh_t* directRet = ret;
        DWORD dummyCode = 0;
        SehCallFnTableGHAM(tramp, ret, eye, effectiveType, g_fnTableHamHasType, &directRet, &dummyCode);
        return directRet;
    }

    ++g_hamReentrancyDepth;
    auto callOrigSeh = [&](vr::HiddenAreaMesh_t* outPtr, vr::EHiddenAreaMeshType t,
                           vr::HiddenAreaMesh_t** outRet, DWORD* outCode) -> bool {
        return SehCallFnTableGHAM(tramp, outPtr, eye, t, g_fnTableHamHasType, outRet, outCode);
    };

    vr::HiddenAreaMesh_t* res = HandleHiddenAreaMeshShared(
        g_fnTableAssociatedCppIface, g_fnTableProjRawSlot, g_fnTableProjRawFn,
        ret, eye, effectiveType, g_fnTableHamHasType, g_fnTableVersionStr, callOrigSeh);
    --g_hamReentrancyDepth;
    return res;
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

    if (IsOpenXrSessionActiveAnyApi()) {
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
        s_blockFrames = 0;
        s_blockDtSum = 0.0;
        s_sswBlockStreak = 0;
    }

    const bool hasSubmitted3D = g_openVrHandlingFrames.load(std::memory_order_relaxed) && (s_noSubmitStreak < 15);
    const bool panelAlreadyHalf = (s_panelHz <= 60.5);
    const double effectiveHz = (!manualSubRate && !panelAlreadyHalf && s_sswHalfRateActive) ? (s_panelHz * 0.5) : s_panelHz;
    const double hz = (g_cfg.pacing.hzOverride > 0.0) ? g_cfg.pacing.hzOverride : effectiveHz;
    double targetFps = 0.0;

    if (g_cfg.pacing.enabled && s_warmupFrames >= 30) {
        LimitInputs lim{};
        lim.hz = hz;
        lim.offsetFps = g_cfg.pacing.offsetFps;
        lim.frameGenDivisor = (manualSubRate && panelAlreadyHalf) ? 1.0 : g_cfg.pacing.frameGenDivisor;
        lim.afrMultiplier = g_cfg.pacing.afrMultiplier;
        targetFps = ComputeTargetFps(lim);
        g_limiter.SetTargetFps(targetFps);
        if (hasSubmitted3D) {
            g_limiter.WaitBeforeRuntime();
        }
    }

    const int32_t vrsStatus = VrsFoveatedD3D11::Instance().GetVrsStatus();
    const int32_t maskStatus = (g_cfg.mask.openvrMode > 0) ? 1 : 0;
    const double reportedRawHz = panelAlreadyHalf ? (s_panelHz * 2.0) : s_panelHz;
    const uint32_t reportedSsw = (s_sswHalfRateActive || panelAlreadyHalf || manualSubRate) ? 1u : 0u;
    g_shm.UpdateSessionTelemetry(L"OpenVR (SteamVR)", hz, targetFps,
                                 reportedRawHz, reportedSsw,
                                 vrsStatus, maskStatus,
                                 g_lastInW, g_lastInH, g_lastOutW, g_lastOutH,
                                 g_lastFormat, g_upscaleStatus, g_lastHResult);

    vr::EVRCompositorError res = origFn(self, pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount);

    LARGE_INTEGER afterWaitQpc{};
    QueryPerformanceCounter(&afterWaitQpc);

    if (hasSubmitted3D && s_qpcFreq > 0) {
        if (s_lastWaitExitQpc > 0) {
            const double dt = static_cast<double>(afterWaitQpc.QuadPart - s_lastWaitExitQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.250) {
                s_blockFrames = 0;
                s_blockDtSum = 0.0;
                s_sswBlockStreak = 0;
            } else if (dt > 0.003 && dt < 0.100) {
                if (s_warmupFrames < 30) {
                    s_warmupFrames++;
                } else if (!manualSubRate && !panelAlreadyHalf && !s_sswHalfRateActive) {
                    s_blockDtSum += dt;
                    s_blockFrames++;

                    if (s_blockFrames >= 45) {
                        const double blockAvgHz = 45.0 / s_blockDtSum;
                        const double halfPanel = s_panelHz * 0.5;
                        s_blockFrames = 0;
                        s_blockDtSum = 0.0;

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

    if (IsOpenXrSessionActiveAnyApi()) {
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
    VrsFoveatedD3D11::Instance().UpdateTarget(
        g_ovrCtx.device.Get(),
        g_ovrCtx.context.Get(),
        g_cfg.ffr.enabled,
        fullEyeW,
        fullEyeH,
        g_cfg.ffr.debugMode);

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

void LogUnsupportedOnce(const char* cleanName) {
    if (!cleanName) return;
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        if (g_loggedUnsupportedVersions.insert(cleanName).second) {
            shouldLog = true;
        }
    }
    if (shouldLog) {
        LogMessage(LogLevel::Warn, "OpenVR: " + std::string(cleanName) + " not in slot table, passthrough");
    }
}

void HookCompositorInstance(const char* name, void* iface) {
    if (!iface || !name) return;
    const char* cleanName = StripFnTablePrefix(name);
    const OpenVrSlotEntry* entry = LookupOpenVrSlots(cleanName);
    if (!entry || entry->waitSlot < 0 || entry->submitSlot < 0) {
        LogUnsupportedOnce(cleanName);
        return;
    }

    std::lock_guard<std::mutex> installLock(g_installMutex);
    MH_Initialize();

    RegisterAndEnableSlotHookLocked(g_waitGetPosesReg, cleanName, "WaitGetPoses", iface,
                                    static_cast<uint32_t>(entry->waitSlot),
                                    reinterpret_cast<void*>(&HookedWaitGetPoses));
    RegisterAndEnableSlotHookLocked(g_submitReg, cleanName, "Submit", iface,
                                    static_cast<uint32_t>(entry->submitSlot),
                                    reinterpret_cast<void*>(&HookedSubmit));
}

void HookSystemInstance(const char* name, void* iface) {
    if (!iface || !name) return;
    const char* cleanName = StripFnTablePrefix(name);
    const OpenVrSlotEntry* entry = LookupOpenVrSlots(cleanName);
    if (!entry || entry->hamSlot < 0 || entry->projRawSlot < 0) {
        LogUnsupportedOnce(cleanName);
        return;
    }

    std::lock_guard<std::mutex> installLock(g_installMutex);
    MH_Initialize();

    RegisterAndEnableSlotHookLocked(g_hamReg, cleanName, "GetHiddenAreaMesh", iface,
                                    static_cast<uint32_t>(entry->hamSlot),
                                    reinterpret_cast<void*>(&Hooked_GetHiddenAreaMesh),
                                    entry->hamHasType,
                                    entry->projRawSlot);
}

void HookSystemFnTableInstance(const char* cppName, void* fnTablePtr, void* cppIface) {
    if (!cppName || !fnTablePtr) return;
    const OpenVrSlotEntry* entry = LookupOpenVrSlots(cppName);
    if (!entry || entry->hamSlot < 0 || entry->projRawSlot < 0) {
        LogUnsupportedOnce(cppName);
        return;
    }

    const uint32_t slotIdx = static_cast<uint32_t>(entry->hamSlot);
    void** fnTable = reinterpret_cast<void**>(fnTablePtr);
    void* fnTarget = fnTable[slotIdx];

    void* vtblTarget = nullptr;
    if (cppIface) {
        void** vtbl = *reinterpret_cast<void***>(cppIface);
        if (vtbl) {
            vtblTarget = vtbl[slotIdx];
        }
    }

    LogMessage(LogLevel::Info, "FnTable " + std::string(cppName) + " GHAM=" + PtrToHex(fnTarget) +
               " vtblTarget=" + PtrToHex(vtblTarget));

    if (!fnTarget) return;

    if (fnTarget == vtblTarget) {
        LogMessage(LogLevel::Info, "FnTable " + std::string(cppName) + " shared");
        return;
    }

    std::lock_guard<std::mutex> installLock(g_installMutex);
    MH_Initialize();

    if (g_fnTableHamTrampoline != nullptr) {
        return;
    }

    g_fnTableAssociatedCppIface = cppIface;
    g_fnTableProjRawSlot = entry->projRawSlot;
    g_fnTableProjRawFn = (entry->projRawSlot >= 0) ? fnTable[entry->projRawSlot] : nullptr;
    g_fnTableHamHasType = entry->hamHasType;
    g_fnTableVersionStr = std::string("FnTable:") + cppName;

    void* tramp = nullptr;
    if (MH_CreateHook(fnTarget, reinterpret_cast<void*>(&Hooked_FnTable_GetHiddenAreaMesh), &tramp) == MH_OK && tramp) {
        if (MH_EnableHook(fnTarget) == MH_OK) {
            g_fnTableHamTrampoline = tramp;
            LogMessage(LogLevel::Info, "OpenVR Hook installed: FnTable:" + std::string(cppName) +
                       "::GetHiddenAreaMesh [slot[" + std::to_string(slotIdx) + "]=" + PtrToHex(fnTarget) +
                       ", trampoline=" + PtrToHex(tramp) + "]");
        } else {
            MH_RemoveHook(fnTarget);
        }
    }
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
    } else if (sName.rfind("IVRSystem_", 0) == 0) {
        HookSystemInstance(name, iface);
    } else if (sName.rfind("FnTable:IVRSystem_", 0) == 0) {
        const char* cppName = name + 8;
        int dummyErr = 0;
        void* cppIface = g_origGetGenericInterface(cppName, &dummyErr);
        if (cppIface) {
            HookSystemInstance(cppName, cppIface);
        }
        HookSystemFnTableInstance(cppName, iface, cppIface);
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
    LARGE_INTEGER qpcNow{};
    QueryPerformanceCounter(&qpcNow);
    LogMessage(LogLevel::Info, "OpenVR VR_GetGenericInterface hook installed [target=" +
               PtrToHex(reinterpret_cast<const void*>(fn)) + ", trampoline=" + PtrToHex(origFn) +
               ", qpc=" + std::to_string(qpcNow.QuadPart) + "].");
    return true;
}

void InstallOpenVRHooks() {
    (void)InstallOpenVRHooksWithStatus();
}

} // namespace vrpacing

#endif

