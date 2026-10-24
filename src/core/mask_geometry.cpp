#include "vrpacing/mask_geometry.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vrpacing {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec2 ClampToSquareRay(float cx, float cy, float ex, float ey) {
    const float dx = ex - cx;
    const float dy = ey - cy;

    if (std::abs(dx) < 1e-8f && std::abs(dy) < 1e-8f) {
        return {cx, cy};
    }

    float t = 1e30f;
    if (std::abs(dx) > 1e-8f) {
        const float tx1 = (-1.0f - cx) / dx;
        const float tx2 = (1.0f - cx) / dx;
        if (tx1 > 0.0f) t = std::min(t, tx1);
        if (tx2 > 0.0f) t = std::min(t, tx2);
    }
    if (std::abs(dy) > 1e-8f) {
        const float ty1 = (-1.0f - cy) / dy;
        const float ty2 = (1.0f - cy) / dy;
        if (ty1 > 0.0f) t = std::min(t, ty1);
        if (ty2 > 0.0f) t = std::min(t, ty2);
    }

    Vec2 out{cx + dx * t, cy + dy * t};
    out.x = std::clamp(out.x, -1.0f, 1.0f);
    out.y = std::clamp(out.y, -1.0f, 1.0f);
    return out;
}

void PushTri(std::vector<Triangle>& tris, const Vec2& a, const Vec2& b, const Vec2& c, bool flip) {
    if (!flip) {
        tris.push_back({a, b, c});
    } else {
        tris.push_back({a, c, b});
    }
}

} // namespace

std::vector<Triangle> BuildEllipseOutsideMaskTriangles(const EllipseMaskParams& params) {
    if (params.segments < 8 || params.radiusX <= 0.0f || params.radiusY <= 0.0f) {
        throw std::invalid_argument("invalid ellipse mask parameters");
    }

    std::vector<Vec2> ellipse(params.segments);
    std::vector<Vec2> square(params.segments);

    for (uint32_t i = 0; i < params.segments; ++i) {
        const float t = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(params.segments);
        const float ex = params.centerX + std::cos(t) * params.radiusX;
        const float ey = params.centerY + std::sin(t) * params.radiusY;
        ellipse[i] = {ex, ey};
        square[i] = ClampToSquareRay(params.centerX, params.centerY, ex, ey);
    }

    std::vector<Triangle> tris;
    tris.reserve(params.segments * 2);

    for (uint32_t i = 0; i < params.segments; ++i) {
        const uint32_t j = (i + 1) % params.segments;
        const Vec2& e0 = ellipse[i];
        const Vec2& e1 = ellipse[j];
        const Vec2& s0 = square[i];
        const Vec2& s1 = square[j];

        PushTri(tris, e0, s0, s1, params.flipWinding);
        PushTri(tris, e0, s1, e1, params.flipWinding);
    }

    return tris;
}

IndexedMesh BuildEllipseOutsideMaskIndexed(const EllipseMaskParams& params) {
    if (params.segments < 8 || params.radiusX <= 0.0f || params.radiusY <= 0.0f) {
        throw std::invalid_argument("invalid ellipse mask parameters");
    }

    IndexedMesh mesh;
    mesh.vertices.reserve(params.segments * 2);
    mesh.indices.reserve(params.segments * 6);

    for (uint32_t i = 0; i < params.segments; ++i) {
        const float t = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(params.segments);
        const Vec2 e{params.centerX + std::cos(t) * params.radiusX,
                     params.centerY + std::sin(t) * params.radiusY};
        const Vec2 s = ClampToSquareRay(params.centerX, params.centerY, e.x, e.y);

        mesh.vertices.push_back(e);
        mesh.vertices.push_back(s);
    }

    for (uint32_t i = 0; i < params.segments; ++i) {
        const uint32_t j = (i + 1) % params.segments;
        const uint32_t e0 = i * 2;
        const uint32_t s0 = e0 + 1;
        const uint32_t e1 = j * 2;
        const uint32_t s1 = e1 + 1;

        if (!params.flipWinding) {
            mesh.indices.insert(mesh.indices.end(), {e0, s0, s1, e0, s1, e1});
        } else {
            mesh.indices.insert(mesh.indices.end(), {e0, s1, s0, e0, e1, s1});
        }
    }

    return mesh;
}

bool IsPointInsideEllipse(float x, float y, const EllipseMaskParams& params) {
    if (params.radiusX <= 0.0f || params.radiusY <= 0.0f) {
        return false;
    }
    const float nx = (x - params.centerX) / params.radiusX;
    const float ny = (y - params.centerY) / params.radiusY;
    return (nx * nx + ny * ny) <= 1.0f;
}

} // namespace vrpacing
