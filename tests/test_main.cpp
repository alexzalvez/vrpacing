#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"
#include "vrpacing/mask_geometry.h"

namespace {

int g_failed = 0;

void Check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "[OK] " << name << "\n";
    } else {
        std::cout << "[FAIL] " << name << "\n";
        ++g_failed;
    }
}

void TestMaskGeometry() {
    vrpacing::EllipseMaskParams p{};
    p.centerX = 0.0f;
    p.centerY = 0.0f;
    p.radiusX = 0.8f;
    p.radiusY = 0.6f;
    p.segments = 64;

    auto tris = vrpacing::BuildEllipseOutsideMaskTriangles(p);
    auto mesh = vrpacing::BuildEllipseOutsideMaskIndexed(p);

    Check(tris.size() == 128, "mask: 2 triángulos por segmento");
    Check(mesh.vertices.size() == 128, "mask: vértices indexados esperados");
    Check(mesh.indices.size() == 384, "mask: índices esperados");

    Check(vrpacing::IsPointInsideEllipse(0.0f, 0.0f, p), "mask: centro dentro");
    Check(!vrpacing::IsPointInsideEllipse(1.0f, 1.0f, p), "mask: esquina fuera");
}

void TestLimiterMath() {
    vrpacing::LimitInputs in{};
    in.hz = 90.0;
    in.afrMultiplier = 1.0;
    in.frameGenDivisor = 2.0;
    in.offsetFps = 0.1;

    const double target = vrpacing::ComputeTargetFps(in);
    Check(std::abs(target - 44.9) < 1e-6, "limiter: fórmula base");

    const int64_t ticks = vrpacing::ComputeFramePeriodTicks(90.0, 9000000);
    Check(ticks == 100000, "limiter: ticks exactos");

    const int64_t next = vrpacing::AdvanceDeadlineNoDrift(1000, 5000, 1000);
    Check(next >= 5000, "limiter: resincronización sin deriva");
}

void TestHzTracker() {
    vrpacing::HzTracker tracker;
    for (int i = 0; i < 20; ++i) {
        tracker.PushHz(89.9 + ((i % 2) ? 0.1 : -0.1));
    }

    const auto hz = tracker.GetEffectiveHz();
    Check(hz.has_value(), "hz_tracker: valor efectivo disponible");
    if (hz) {
        Check(*hz > 89.0 && *hz < 91.0, "hz_tracker: suavizado estable");
    }

    vrpacing::LimitInputs in{};
    in.offsetFps = 0.1;
    const double target = tracker.ComputeTargetFps(in);
    Check(target > 88.0 && target < 91.0, "hz_tracker: target derivado");
}

} // namespace

int main() {
    try {
        TestMaskGeometry();
        TestLimiterMath();
        TestHzTracker();
    } catch (const std::exception& e) {
        std::cout << "[EXCEPTION] " << e.what() << "\n";
        return 1;
    }

    if (g_failed == 0) {
        std::cout << "Todos los tests pasaron.\n";
        return 0;
    }

    std::cout << "Fallos: " << g_failed << "\n";
    return 1;
}
