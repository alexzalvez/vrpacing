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
    int openvrMode = 2;  // [Z2] Release default: 2 (replaces only when original=EMPTY unless openvrForce!=0)
    int openvrForce = 0; // [Z2] 0=passthrough when original=MESH, 1=force replace even when original=MESH
    float radiusX = 1.0f;
    float radiusY = 1.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    uint32_t segments = 96;
    bool flipWinding = false;
};

struct FfrConfig {
    bool enabled = false;
    bool openxrEnabled = false;
    float innerRadius = 0.60f;
    int debugMode = 0;
    float outerRadius = 1.00f;
};

struct Vrs12Config {
    int debugMode = 1;
    std::string extraFormats = "";
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
    Vrs12Config vrs12;
    PacingConfig pacing;
    LoggingConfig logging;
};

inline RuntimeConfig DefaultConfig() {
    return RuntimeConfig{};
}

} // namespace vrpacing