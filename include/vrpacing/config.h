#pragma once

#include <cstdint>
#include <string>

namespace vrpacing {

struct UpscalingConfig {
    bool enabled = true;
    float scale = 2.0f;
    float sharpness = 0.0f;
    float radius = 0.60f;
    uint64_t maxOutputPixels = 32000000ull;
};

struct MaskConfig {
    bool enabled = true;
    float radiusX = 1.0f;
    float radiusY = 1.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    uint32_t segments = 96;
    bool flipWinding = false;
};

struct FfrConfig {
    bool enabled = false;
    float innerRadius = 0.60f;
    float outerRadius = 1.00f;
};

struct PacingConfig {
    bool enabled = true;
    double offsetFps = 0.1;
    double frameGenDivisor = 1.0;
    double afrMultiplier = 1.0;
    double hzOverride = 0.0;
};

struct LoggingConfig {
    std::string level = "info";
};

struct RuntimeConfig {
    bool enabled = true;
    UpscalingConfig upscaling;
    MaskConfig mask;
    FfrConfig ffr;
    PacingConfig pacing;
    LoggingConfig logging;
};

inline RuntimeConfig DefaultConfig() {
    return RuntimeConfig{};
}

} // namespace vrpacing