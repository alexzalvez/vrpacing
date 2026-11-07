#pragma once

#include <cstdint>

namespace vrpacing {

constexpr wchar_t kSharedStateName[] = L"Local\\VRPacingSharedState";
constexpr wchar_t kAppVersionString[] = L"v0.8.1-beta";
constexpr char kAppVersionAnsi[] = "v0.8.1-beta";

constexpr uint32_t kFlagEnabled = 1 << 0;
constexpr uint32_t kFlagUpscalingEnabled = 1 << 1;
constexpr uint32_t kFlagPacingEnabled = 1 << 2;
constexpr uint32_t kFlagTelemetryActive = 1 << 3;
constexpr uint32_t kFlagFfrEnabled = 1 << 4;
constexpr uint32_t kFlagMaskEnabled = 1 << 5;
constexpr uint32_t kFlagFfrOpenXrEnabled = 1 << 6;

constexpr uint32_t kSharedStateVersion = 8;

struct SharedState {
    uint32_t version;
    uint32_t flags;
    uint32_t activePid;
    uint32_t openXrD3D11Pid;

    wchar_t activeApi[64];
    wchar_t hookedProcess[64];

    double effectiveHz;
    double appliedCap;
    double frameGenDivisor;
    double offsetFps;

    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t outputWidth;
    uint32_t outputHeight;
    uint32_t dxgiFormat;

    int32_t upscaleStatus;
    int32_t lastHResult;

    double avgFps;
    double onePercentLowFps;
    uint64_t frameCounter;

    // Extended session telemetry and circular frametimes buffer
    double rawPanelHz;
    uint32_t sswActive;
    int32_t vrsStatus;
    int32_t maskStatus;
    uint32_t frameTimeWriteIdx;
    float frameTimesMs[256];

    // Synchronous comparison screenshot capture (Ctrl + F12)
    uint32_t screenshotRequest;
    uint32_t screenshotSavedCount;
    wchar_t screenshotDir[260];
};

} // namespace vrpacing


