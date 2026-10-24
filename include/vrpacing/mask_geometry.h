#pragma once

#include <cstdint>
#include <vector>

namespace vrpacing {

struct Vec2 {
    float x;
    float y;
};

struct Triangle {
    Vec2 a;
    Vec2 b;
    Vec2 c;
};

struct IndexedMesh {
    std::vector<Vec2> vertices;
    std::vector<uint32_t> indices;
};

struct EllipseMaskParams {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float radiusX = 1.0f;
    float radiusY = 1.0f;
    uint32_t segments = 96;
    bool flipWinding = false;
};

std::vector<Triangle> BuildEllipseOutsideMaskTriangles(const EllipseMaskParams& params);
IndexedMesh BuildEllipseOutsideMaskIndexed(const EllipseMaskParams& params);
bool IsPointInsideEllipse(float x, float y, const EllipseMaskParams& params);

} // namespace vrpacing
