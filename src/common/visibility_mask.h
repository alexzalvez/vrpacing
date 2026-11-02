#pragma once
// [C4] Shared pure circular visibility mask generator (no API types).

#include <cstdint>
#include <vector>

namespace vrpacing {

enum class VisibilityMaskKind : int {
    HiddenTriangles = 0,  // OpenXR HIDDEN_TRIANGLE_MESH / OpenVR k_eHiddenAreaMesh_Standard
    VisibleTriangles = 1, // OpenXR VISIBLE_TRIANGLE_MESH / OpenVR k_eHiddenAreaMesh_Inverse
    LineLoop = 2          // OpenXR LINE_LOOP / OpenVR k_eHiddenAreaMesh_LineLoop
};

struct MaskVec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct GeneratedVisibilityMask {
    std::vector<MaskVec2> vertices;
    std::vector<uint32_t> indices;
};

void GetCircularMaskCounts(VisibilityMaskKind kind,
                           uint32_t segments,
                           uint32_t& outVertexCount,
                           uint32_t& outIndexCount);

GeneratedVisibilityMask GenerateCircularMaskTangentSpace(VisibilityMaskKind kind,
                                                         float minX,
                                                         float maxX,
                                                         float minY,
                                                         float maxY,
                                                         uint32_t segments = 256);

GeneratedVisibilityMask GenerateResetMaskTangentSpace(VisibilityMaskKind kind,
                                                      float minX,
                                                      float maxX,
                                                      float minY,
                                                      float maxY);

// Maps tangent-space generated vertices into UV space: u=(x-l)/(r-l), v=(y-t)/(b-t)
// and expands indexed triangles into flat triangle vertex lists for OpenVR HiddenAreaMesh_t.
std::vector<MaskVec2> GenerateOpenVrFlatTriangleVerticesUV(VisibilityMaskKind kind,
                                                           float left,
                                                           float right,
                                                           float top,
                                                           float bottom,
                                                           uint32_t segments,
                                                           uint32_t& outTriangleCount);

} // namespace vrpacing