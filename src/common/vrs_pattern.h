#pragma once
// [B5, C3, B8', Z1] Shared pure ring pattern generator for D3D11 and D3D12 VRS.

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
        return (distance < kVrsMidRadius) ? AbstractShadingRate::R2x2 : AbstractShadingRate::R4x4;
    }
    if (distance < kVrsMidRadius) {
        return AbstractShadingRate::R2x1;
    }
    return AbstractShadingRate::R4x4;
}

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

inline uint8_t AbstractRateToD3D12Byte(AbstractShadingRate r, bool additionalShadingRatesSupported) {
    switch (r) {
        case AbstractShadingRate::R1x1: return 0x0;
        case AbstractShadingRate::R1x2: return 0x1;
        case AbstractShadingRate::R2x1: return 0x4;
        case AbstractShadingRate::R2x2: return 0x5;
        case AbstractShadingRate::R4x4:
        default:
            return additionalShadingRatesSupported ? 0xA : 0x5;
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

inline std::vector<uint8_t> CreateTopLeftAnchoredPatternD3D12(
    uint32_t eyeW, uint32_t eyeH, uint32_t texTilesW, uint32_t texTilesH,
    uint32_t tileSize, int debugMode, bool additionalShadingRatesSupported) {
    std::vector<uint8_t> data(static_cast<size_t>(texTilesW) * static_cast<size_t>(texTilesH), 0);
    if (eyeW == 0 || eyeH == 0 || texTilesW == 0 || texTilesH == 0 || tileSize == 0) {
        return data;
    }

    const float halfEyeW = static_cast<float>(eyeW) * 0.5f;
    const float halfEyeH = static_cast<float>(eyeH) * 0.5f;
    const uint8_t outerByte = AbstractRateToD3D12Byte(DistanceToAbstractRate(2.0f, debugMode), additionalShadingRatesSupported);

    for (uint32_t ty = 0; ty < texTilesH; ++ty) {
        const float py = (static_cast<float>(ty) + 0.5f) * static_cast<float>(tileSize);
        for (uint32_t tx = 0; tx < texTilesW; ++tx) {
            const float px = (static_cast<float>(tx) + 0.5f) * static_cast<float>(tileSize);
            if (px >= static_cast<float>(eyeW) || py >= static_cast<float>(eyeH)) {
                data[static_cast<size_t>(ty) * texTilesW + tx] = outerByte;
            } else {
                const float nx = (px - halfEyeW) / halfEyeW;
                const float ny = (py - halfEyeH) / halfEyeH;
                const float d = std::sqrt(nx * nx + ny * ny);
                const AbstractShadingRate rate = DistanceToAbstractRate(d, debugMode);
                data[static_cast<size_t>(ty) * texTilesW + tx] =
                    AbstractRateToD3D12Byte(rate, additionalShadingRatesSupported);
            }
        }
    }
    return data;
}

// [Z1] Second pattern texture generator for D3D12 DOUBLEWIDE targets:
// Capacity = ceil(2*eyeW*1.5) x ceil(eyeH*1.5), top-left anchored,
// two foveal centers at (eyeW/2, eyeH/2) and (eyeW + eyeW/2, eyeH/2).
// Tiles beyond (2*eyeW, eyeH) receive outermost ring rate.
inline std::vector<uint8_t> CreateTopLeftAnchoredDoubleWidePatternD3D12(
    uint32_t eyeW, uint32_t eyeH, uint32_t texTilesW, uint32_t texTilesH,
    uint32_t tileSize, int debugMode, bool additionalShadingRatesSupported) {
    std::vector<uint8_t> data(static_cast<size_t>(texTilesW) * static_cast<size_t>(texTilesH), 0);
    if (eyeW == 0 || eyeH == 0 || texTilesW == 0 || texTilesH == 0 || tileSize == 0) {
        return data;
    }

    const float fEyeW = static_cast<float>(eyeW);
    const float fTwoEyeW = fEyeW * 2.0f;
    const float fEyeH = static_cast<float>(eyeH);
    const float halfEyeW = fEyeW * 0.5f;
    const float halfEyeH = fEyeH * 0.5f;
    const uint8_t outerByte = AbstractRateToD3D12Byte(DistanceToAbstractRate(2.0f, debugMode), additionalShadingRatesSupported);

    for (uint32_t ty = 0; ty < texTilesH; ++ty) {
        const float py = (static_cast<float>(ty) + 0.5f) * static_cast<float>(tileSize);
        for (uint32_t tx = 0; tx < texTilesW; ++tx) {
            const float px = (static_cast<float>(tx) + 0.5f) * static_cast<float>(tileSize);
            if (px >= fTwoEyeW || py >= fEyeH) {
                data[static_cast<size_t>(ty) * texTilesW + tx] = outerByte;
            } else {
                const float centerX = (px < fEyeW) ? halfEyeW : (fEyeW + halfEyeW);
                const float nx = (px - centerX) / halfEyeW;
                const float ny = (py - halfEyeH) / halfEyeH;
                const float d = std::sqrt(nx * nx + ny * ny);
                const AbstractShadingRate rate = DistanceToAbstractRate(d, debugMode);
                data[static_cast<size_t>(ty) * texTilesW + tx] =
                    AbstractRateToD3D12Byte(rate, additionalShadingRatesSupported);
            }
        }
    }
    return data;
}

} // namespace vrpacing