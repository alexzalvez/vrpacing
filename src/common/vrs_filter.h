#pragma once
// [B4, F8] Shared pure candidate filter for D3D11 and D3D12 VRS.

#include <dxgiformat.h>
#include <cmath>
#include <cstdint>

namespace vrpacing {

inline constexpr float kVrsAspectTolerance = 0.01f;
inline constexpr float kVrsMinScale = 0.75f;
inline constexpr float kVrsMaxScale = 1.50f;
inline constexpr float kVrsSquareAspectTolerance = 0.02f;

inline bool IsWhitelistedVrsColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
    }
}

struct VrsFilterInput {
    uint32_t rtW = 0;
    uint32_t rtH = 0;
    uint32_t rtArraySize = 0;
    uint32_t rtSamples = 0;
    DXGI_FORMAT rtResourceFormat = DXGI_FORMAT_UNKNOWN;
    bool isExtraFormatAllowed = false; // [F8] Optional extra formats list
    bool validViewDim = false;
    uint32_t rtvSlices = 1;
    bool isExcludedSwapchain = false;

    bool hasDsv = false;
    uint32_t dsvW = 0;
    uint32_t dsvH = 0;
    uint32_t dsvArraySize = 0;
    uint32_t dsvSamples = 0;

    uint32_t targetEyeW = 0;
    uint32_t targetEyeH = 0;
    bool allowArrayAndDoubleWide = true;
};

struct VrsFilterOutput {
    bool accepted = false;
    bool doubleWide = false;
    bool isArray = false;
    const char* label = "REJECT_DIM";
};

inline VrsFilterOutput EvaluateVrsCandidate(const VrsFilterInput& in) {
    VrsFilterOutput out{};
    if (in.targetEyeW == 0 || in.targetEyeH == 0) {
        out.label = "REJECT_DIM";
        return out;
    }

    const uint32_t maxArray = in.allowArrayAndDoubleWide ? 2u : 1u;
    if (!in.validViewDim || in.rtW == 0 || in.rtH == 0 || in.rtArraySize == 0 || in.rtArraySize > maxArray) {
        out.label = "REJECT_DIM";
        return out;
    }

    const float targetAspect = static_cast<float>(in.targetEyeW) / static_cast<float>(in.targetEyeH);

    if (std::abs(targetAspect - 1.0f) > kVrsSquareAspectTolerance && in.rtW == in.rtH) {
        out.label = "REJECT_SQUARE";
        return out;
    }

    const float a1 = static_cast<float>(in.rtW) / static_cast<float>(in.rtH);
    const float a2 = (static_cast<float>(in.rtW) * 0.5f) / static_cast<float>(in.rtH);
    const bool doubleWide = in.allowArrayAndDoubleWide && (std::abs(a2 - targetAspect) <= kVrsAspectTolerance);
    const bool single = !doubleWide && (std::abs(a1 - targetAspect) <= kVrsAspectTolerance);

    if (!doubleWide && !single) {
        out.label = "REJECT_ASPECT";
        return out;
    }

    const float eyeW = doubleWide ? (static_cast<float>(in.rtW) * 0.5f) : static_cast<float>(in.rtW);
    const float scale = eyeW / static_cast<float>(in.targetEyeW);
    if (scale < kVrsMinScale || scale > kVrsMaxScale) {
        out.label = "REJECT_SCALE";
        return out;
    }

    if (in.isExcludedSwapchain) {
        out.label = "REJECT_SWAPCHAIN";
        return out;
    }

    // [F8] Check standard whitelist or extraFormats
    if (!IsWhitelistedVrsColorFormat(in.rtResourceFormat) && !in.isExtraFormatAllowed) {
        out.label = "REJECT_FORMAT";
        return out;
    }

    if (!in.hasDsv) {
        out.label = "REJECT_NODSV";
        return out;
    }

    const bool dsvDimMatch = (in.dsvW == in.rtW && in.dsvH == in.rtH && in.dsvSamples == in.rtSamples);
    const bool dsvArrayMatch = (!in.rtArraySize || in.rtArraySize < 2 || in.dsvArraySize >= in.rtArraySize);
    if (!dsvDimMatch || !dsvArrayMatch) {
        out.label = "REJECT_NODSV";
        return out;
    }

    if (doubleWide && in.rtvSlices > 1) {
        out.label = "REJECT_DIM";
        return out;
    }

    const bool isArrayTarget = (!doubleWide && in.rtvSlices >= 2 && in.rtArraySize >= 2);
    out.accepted = true;
    out.doubleWide = doubleWide;
    out.isArray = isArrayTarget;
    out.label = doubleWide ? "DOUBLEWIDE" : (isArrayTarget ? "ARRAY" : "SINGLE");
    return out;
}

} // namespace vrpacing