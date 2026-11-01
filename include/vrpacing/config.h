#pragma once

#include <cstdint>

namespace vrpacing {

struct UpscalingConfig {
    bool enabled = true;
    float scale = 2.0f;
    float radius = 0.60f;
    float sharpness = 0.0f;
    uint64_t maxOutputPixels = 3296ULL * 3584ULL;
};

struct PacingConfig {
    bool enabled = true;
    double offsetFps = 0.10;
    double frameGenDivisor = 1.0;
    double afrMultiplier = 1.0;
    double hzOverride = 0.0;
};

struct MaskConfig {
    bool enabled = false;
    float radiusX = 1.00f;
    float radiusY = 1.00f;
    uint32_t segments = 256;
};

struct FfrConfig {
    bool enabled = false;
    float innerRadius = 0.60f;
    int debugMode = 0; // [A8] 0=normal, 1=passive, 2=transparent, 3=extreme
};

struct RuntimeConfig {
    bool enabled = true;
    UpscalingConfig upscaling;
    PacingConfig pacing;
    MaskConfig mask;
    FfrConfig ffr;
};

} // namespace vrpacing