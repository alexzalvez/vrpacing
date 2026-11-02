#include "common/visibility_mask.h"

#include <algorithm>
#include <cmath>

namespace vrpacing {

void GetCircularMaskCounts(VisibilityMaskKind kind,
                           uint32_t segments,
                           uint32_t& outVertexCount,
                           uint32_t& outIndexCount) {
    if (kind == VisibilityMaskKind::VisibleTriangles) {
        outVertexCount = segments + 1;
        outIndexCount = segments * 3;
    } else if (kind == VisibilityMaskKind::HiddenTriangles) {
        outVertexCount = segments * 2;
        outIndexCount = segments * 6;
    } else if (kind == VisibilityMaskKind::LineLoop) {
        outVertexCount = segments;
        outIndexCount = segments;
    } else {
        outVertexCount = 0;
        outIndexCount = 0;
    }
}

GeneratedVisibilityMask GenerateCircularMaskTangentSpace(VisibilityMaskKind kind,
                                                         float minX,
                                                         float maxX,
                                                         float minY,
                                                         float maxY,
                                                         uint32_t segments) {
    GeneratedVisibilityMask out{};
    uint32_t reqVerts = 0;
    uint32_t reqIndices = 0;
    GetCircularMaskCounts(kind, segments, reqVerts, reqIndices);
    if (reqVerts == 0 || reqIndices == 0) {
        return out;
    }

    out.vertices.resize(reqVerts);
    out.indices.resize(reqIndices);

    const float cx = 0.5f * (minX + maxX);
    const float cy = 0.5f * (minY + maxY);
    const float rx = 0.5f * (maxX - minX);
    const float ry = 0.5f * (maxY - minY);
    constexpr float kTwoPi = 6.283185307179586f;

    if (kind == VisibilityMaskKind::VisibleTriangles) {
        out.vertices[0] = { cx, cy };
        for (uint32_t i = 0; i < segments; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(segments)) * kTwoPi;
            out.vertices[i + 1] = { cx + rx * std::cos(a), cy + ry * std::sin(a) };
            const uint32_t next = ((i + 1) % segments) + 1;
            const uint32_t base = i * 3;
            out.indices[base + 0] = 0;
            out.indices[base + 1] = i + 1;
            out.indices[base + 2] = next;
        }
    } else if (kind == VisibilityMaskKind::HiddenTriangles) {
        for (uint32_t i = 0; i < segments; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(segments)) * kTwoPi;
            const float ca = std::cos(a);
            const float sa = std::sin(a);
            const float maxComp = (std::max)(std::abs(ca), std::abs(sa));
            const float outerScale = (maxComp > 0.0001f) ? (1.5f / maxComp) : 2.0f;

            out.vertices[i * 2 + 0] = { cx + rx * ca, cy + ry * sa };
            out.vertices[i * 2 + 1] = { cx + rx * outerScale * ca, cy + ry * outerScale * sa };

            const uint32_t next = (i + 1) % segments;
            const uint32_t i0 = i * 2;
            const uint32_t i1 = i * 2 + 1;
            const uint32_t n0 = next * 2;
            const uint32_t n1 = next * 2 + 1;
            const uint32_t base = i * 6;

            out.indices[base + 0] = i0;
            out.indices[base + 1] = i1;
            out.indices[base + 2] = n0;
            out.indices[base + 3] = n0;
            out.indices[base + 4] = i1;
            out.indices[base + 5] = n1;
        }
    } else if (kind == VisibilityMaskKind::LineLoop) {
        for (uint32_t i = 0; i < segments; ++i) {
            const float a = (static_cast<float>(i) / static_cast<float>(segments)) * kTwoPi;
            out.vertices[i] = { cx + rx * std::cos(a), cy + ry * std::sin(a) };
            out.indices[i] = i;
        }
    }

    return out;
}

GeneratedVisibilityMask GenerateResetMaskTangentSpace(VisibilityMaskKind kind,
                                                      float minX,
                                                      float maxX,
                                                      float minY,
                                                      float maxY) {
    GeneratedVisibilityMask out{};
    if (kind == VisibilityMaskKind::VisibleTriangles) {
        out.vertices = {
            { minX * 1.2f, minY * 1.2f },
            { maxX * 1.2f, minY * 1.2f },
            { maxX * 1.2f, maxY * 1.2f },
            { minX * 1.2f, maxY * 1.2f }
        };
        out.indices = { 0, 1, 2, 0, 2, 3 };
    } else if (kind == VisibilityMaskKind::HiddenTriangles) {
        out.vertices = {
            { minX * 2.0f, minY * 2.0f },
            { minX * 2.0f, minY * 2.0f },
            { minX * 2.0f, minY * 2.0f }
        };
        out.indices = { 0, 1, 2 };
    }
    return out;
}

std::vector<MaskVec2> GenerateOpenVrFlatTriangleVerticesUV(VisibilityMaskKind kind,
                                                           float left,
                                                           float right,
                                                           float top,
                                                           float bottom,
                                                           uint32_t segments,
                                                           uint32_t& outTriangleCount) {
    std::vector<MaskVec2> flat{};
    outTriangleCount = 0;

    const float denomX = right - left;
    const float denomY = bottom - top;
    if (std::abs(denomX) < 1e-5f || std::abs(denomY) < 1e-5f) {
        return flat;
    }

    const float minX = (std::min)(left, right);
    const float maxX = (std::max)(left, right);
    const float minY = (std::min)(top, bottom);
    const float maxY = (std::max)(top, bottom);

    GeneratedVisibilityMask indexed = GenerateCircularMaskTangentSpace(kind, minX, maxX, minY, maxY, segments);
    auto toUV = [&](const MaskVec2& p) -> MaskVec2 {
        return { (p.x - left) / denomX, (p.y - top) / denomY };
    };

    if (kind == VisibilityMaskKind::LineLoop) {
        flat.reserve(indexed.vertices.size());
        for (const auto& v : indexed.vertices) {
            flat.push_back(toUV(v));
        }
        outTriangleCount = static_cast<uint32_t>(flat.size());
    } else {
        flat.reserve(indexed.indices.size());
        for (uint32_t idx : indexed.indices) {
            if (idx < indexed.vertices.size()) {
                flat.push_back(toUV(indexed.vertices[idx]));
            }
        }
        outTriangleCount = static_cast<uint32_t>(flat.size() / 3);
    }

    return flat;
}

} // namespace vrpacing