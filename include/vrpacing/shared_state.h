#pragma once

#include <cstdint>

namespace vrpacing {

constexpr const wchar_t* kSharedStateName = L"Local\\VRPacingShared";

#pragma pack(push, 1)
struct SharedState {
    uint32_t version = 2;
    uint32_t flags = 0;
    double frameGenDivisor = 1.0;
    double offsetFps = 0.10;
    double effectiveHz = 0.0;
    double appliedCap = 0.0;
    uint64_t frameCounter = 0;
    wchar_t activeApi[32] = L"Idle";
};
#pragma pack(pop)

enum SharedFlags : uint32_t {
    kFlagEnabled = 1 << 0,
    kFlagUpscalingEnabled = 1 << 1,
    kFlagMaskEnabled = 1 << 2,
    kFlagPacingEnabled = 1 << 3,
};

} // namespace vrpacing