#pragma once
// [B5] Shared pure ring pattern generator for D3D11 and D3D12 VRS.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vrpacing {

inline constexpr float kVrsInnerRadius = 0.60f;
inline constexpr float kVrsMidRadius = 1.00f;

enum class AbstractShadingRate : uint8_t {
    R1x1 = 0,
    R2x1 = 1,
    R1x2 = 2,
    R2x2 = 3,
    R4x4 = 4
};

inline AbstractShadingRate DistanceToAbstractRate(float distance, int debugMode) {
    if (debugMode == 2) {
        return AbstractShadingRate::R1x1;
    }
    if (distance < kVrsInnerRadius) {
        return AbstractShadingRate::R1x1;
    }
    if (debugMode == 3) {
        return AbstractShadingRate::R4x4;
    }
    if (distance < kVrsMidRadius) {
        return AbstractShadingRate::R2x1;
    }
    return AbstractShadingRate::R4x4;
}

// Legacy index mapping (0=1x1, 1=2x1, 2=4x4) used by D3D11 NVAPI shadingRateTable
inline uint8_t AbstractRateToD3D11TableIndex(AbstractShadingRate r) {
    switch (r) {
        case AbstractShadingRate::R1x1: return 0;
        case AbstractShadingRate::R2x1:
        case AbstractShadingRate::R1x2:
        case AbstractShadingRate::R2x2: return 1;
        case AbstractShadingRate::R4x4:
        default:                        return 2;
    }
}

// D3D12_SHADING_RATE values: 1X1=0x0, 1X2=0x1, 2X1=0x4, 2X2=0x5, 4X4=0xA
inline uint8_t AbstractRateToD3D12Byte(AbstractShadingRate r, bool additionalShadingRatesSupported) {
    switch (r) {
        case AbstractShadingRate::R1x1: return 0x0;
        case AbstractShadingRate::R1x2: return 0x1;
        case AbstractShadingRate::R2x1: return 0x4;
        case AbstractShadingRate::R2x2: return 0x5;
        case AbstractShadingRate::R4x4:
        default:
            return additionalShadingRatesSupported ? 0xA : 0x5; // Clamp 4X4 -> 2X2 if !additionalRates
    }
}

inline std::vector<AbstractShadingRate> GenerateAbstractVrsPattern(
    int rtW, int rtH, int tileW, int tileH, bool doubleWide, int debugMode) {
    if (rtW <= 0 || rtH <= 0 || tileW <= 0 || tileH <= 0) {
        return {};
    }
    const int tilesW = (std::max)(1, (rtW + tileW - 1) / tileW);
    const int tilesH = (std::max)(1, (rtH + tileH - 1) / tileH);

    std::vector<AbstractShadingRate> out(static_cast<size_t>(tilesW) * static_cast<size_t>(tilesH), AbstractShadingRate::R1x1);

    const float eyeW = doubleWide ? (static_cast<float>(rtW) * 0.5f) : static_cast<float>(rtW);
    const float fRtH = static_cast<float>(rtH);
    if (eyeW <= 0.0f || fRtH <= 0.0f) {
        return out;
    }

    for (int y = 0; y < tilesH; ++y) {
        const float py = (std::min)((static_cast<float>(y) + 0.5f) * static_cast<float>(tileH),
                                    static_cast<float>(rtH - 1));
        const float ny = (py / fRtH) * 2.0f - 1.0f;

        for (int x = 0; x < tilesW; ++x) {
            const float px = (std::min)((static_cast<float>(x) + 0.5f) * static_cast<float>(tileW),
                                        static_cast<float>(rtW - 1));
            const float localX = (doubleWide && px >= eyeW) ? (px - eyeW) : px;
            const float nx = (localX / eyeW) * 2.0f - 1.0f;
            const float d = std::sqrt(nx * nx + ny * ny);
            out[static_cast<size_t>(y) * static_cast<size_t>(tilesW) + static_cast<size_t>(x)] =
                DistanceToAbstractRate(d, debugMode);
        }
    }
    return out;
}

inline std::vector<uint8_t> CreateCenteredPatternD3D11(int tilesW, int tilesH, int rtW, int rtH, bool doubleWide, int tileW, int tileH) {
    std::vector<uint8_t> data(static_cast<size_t>(tilesW) * static_cast<size_t>(tilesH), 0);
    std::vector<AbstractShadingRate> abs = GenerateAbstractVrsPattern(rtW, rtH, tileW, tileH, doubleWide, 0);
    const size_t n = (std::min)(data.size(), abs.size());
    for (size_t i = 0; i < n; ++i) {
        data[i] = AbstractRateToD3D11TableIndex(abs[i]);
    }
    return data;
}

inline std::vector<uint8_t> CreateCenteredPatternD3D12(
    int eyeW, int eyeH, int tileSize, int debugMode, bool additionalShadingRatesSupported) {
    std::vector<AbstractShadingRate> abs = GenerateAbstractVrsPattern(eyeW, eyeH, tileSize, tileSize, false, debugMode);
    std::vector<uint8_t> data(abs.size(), 0);
    for (size_t i = 0; i < abs.size(); ++i) {
        data[i] = AbstractRateToD3D12Byte(abs[i], additionalShadingRatesSupported);
    }
    return data;
}

} // namespace vrpacing