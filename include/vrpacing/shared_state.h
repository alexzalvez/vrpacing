#pragma once

#include <cstdint>

namespace vrpacing {

constexpr const wchar_t* kAppVersionString = L"v0.7.0-beta";
constexpr const char* kAppVersionAnsi = "v0.7.0-beta";
constexpr const wchar_t* kSharedStateName = L"Local\\VRPacingShared_v6";

#pragma pack(push, 1)
struct SharedState {
    uint32_t version = 6;
    uint32_t flags = 0;
    uint32_t activePid = 0;
    uint32_t openXrD3D11Pid = 0;
    double frameGenDivisor = 1.0;
    double offsetFps = 0.10;
    double effectiveHz = 0.0;
    double appliedCap = 0.0;
    double avgFps = 0.0;
    double onePercentLowFps = 0.0;
    uint64_t frameCounter = 0;
    wchar_t activeApi[32] = L"Idle";

    // Lab telemetry
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t dxgiFormat = 0;
    int32_t upscaleStatus = 0; // 0=Idle/None, 1=Active OK, -1=Swapchain Fail, -2=Shader Init Fail, -3=UAV/SRV Fail
    int32_t lastHResult = 0;
    wchar_t hookedProcess[64] = L"None";
};
#pragma pack(pop)

enum SharedFlags : uint32_t {
    kFlagEnabled = 1 << 0,
    kFlagUpscalingEnabled = 1 << 1,
    kFlagMaskEnabled = 1 << 2,
    kFlagPacingEnabled = 1 << 3,
    kFlagTelemetryActive = 1 << 4,
};

} // namespace vrpacing