#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

#include <cstddef>
#include "vrpacing/frame_limiter_math.h"
#include "vrpacing/hz_tracker.h"
#include "vrpacing/mask_geometry.h"
#include "vrpacing/shared_state.h"
#include "vrpacing/telemetry_advisor.h"
#ifdef _WIN32
#include "common/vrs_filter.h"
#endif

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

    Check(tris.size() == 128, "mask: 2 triangles per segment");
    Check(mesh.vertices.size() == 128, "mask: expected indexed vertices");
    Check(mesh.indices.size() == 384, "mask: expected indices");

    Check(vrpacing::IsPointInsideEllipse(0.0f, 0.0f, p), "mask: center inside");
    Check(!vrpacing::IsPointInsideEllipse(1.0f, 1.0f, p), "mask: corner outside");
}

void TestLimiterMath() {
    vrpacing::LimitInputs in{};
    in.hz = 90.0;
    in.afrMultiplier = 1.0;
    in.frameGenDivisor = 1.0;
    in.offsetFps = 0.1;

    const double target1x = vrpacing::ComputeTargetFps(in);
    Check(std::abs(target1x - 89.9) < 1e-6, "limiter: 1x native panel (89.90 FPS)");

    in.frameGenDivisor = 2.0;
    const double targetHalf = vrpacing::ComputeTargetFps(in);
    Check(std::abs(targetHalf - 44.9) < 1e-6, "limiter: 1/2x fractional cap (44.90 FPS)");

    in.frameGenDivisor = 3.0;
    const double targetThird = vrpacing::ComputeTargetFps(in);
    Check(std::abs(targetThird - 29.9) < 1e-6, "limiter: 1/3x fractional cap (29.90 FPS)");

    in.hz = 0.0;
    Check(vrpacing::ComputeTargetFps(in) == 0.0, "limiter: invalid hz zero");

    const int64_t ticks = vrpacing::ComputeFramePeriodTicks(90.0, 9000000);
    Check(ticks == 100000, "limiter: exact ticks");

    const int64_t next = vrpacing::AdvanceDeadlineNoDrift(1000, 5000, 1000);
    Check(next >= 5000, "limiter: drift-free resync");
}

void TestHzTracker() {
    vrpacing::HzTracker tracker;
    for (int i = 0; i < 20; ++i) {
        tracker.PushHz(89.9 + ((i % 2) ? 0.1 : -0.1));
    }

    const auto hz = tracker.GetEffectiveHz();
    Check(hz.has_value(), "hz_tracker: effective value available");
    if (hz) {
        Check(*hz > 89.0 && *hz < 91.0, "hz_tracker: stable smoothing");
    }

    vrpacing::LimitInputs in{};
    in.offsetFps = 0.1;
    const double target = tracker.ComputeTargetFps(in);
    Check(target > 88.0 && target < 91.0, "hz_tracker: derived target");
}

void TestSharedStateAbi() {
    Check(sizeof(vrpacing::SharedState) == 1944, "shared_state: fixed 1944-byte size across x86/x64");
    Check(offsetof(vrpacing::SharedState, avgFps) == 336, "shared_state: exact avgFps offset");
}

void TestTelemetryAdvisorMode() {
    vrpacing::SharedState shm{};
    shm.effectiveHz = 90.0;
    shm.rawPanelHz = 90.0;
    shm.frameGenDivisor = 2.0;
    shm.sswActive = 0;

    bool isSsw = false;
    double targetHz = 0.0, panelHz = 0.0;
    vrpacing::TelemetryAnalyzer::ResolveDisplayMode(shm, isSsw, targetHz, panelHz);
    Check(isSsw && std::abs(targetHz - 45.0) < 0.1 && std::abs(panelHz - 90.0) < 0.1,
          "advisor: manual half-rate SSW mode");

    shm.frameGenDivisor = 1.0;
    vrpacing::TelemetryAnalyzer::ResolveDisplayMode(shm, isSsw, targetHz, panelHz);
    Check(!isSsw && std::abs(targetHz - 90.0) < 0.1 && std::abs(panelHz - 90.0) < 0.1,
          "advisor: native refresh mode");
}

#ifdef _WIN32
void TestVrsFilter() {
    vrpacing::VrsFilterInput in{};
    in.rtW = 1648;
    in.rtH = 1800;
    in.rtArraySize = 1;
    in.rtSamples = 1;
    in.rtResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    in.validViewDim = true;
    in.rtvSlices = 1;
    in.hasDsv = true;
    in.dsvW = 1648;
    in.dsvH = 1800;
    in.dsvArraySize = 1;
    in.dsvSamples = 1;
    in.targetEyeW = 1648;
    in.targetEyeH = 1800;
    in.allowArrayAndDoubleWide = true;

    const auto okSingle = vrpacing::EvaluateVrsCandidate(in);
    Check(okSingle.accepted && !okSingle.doubleWide && !okSingle.isArray, "vrs_filter: accepts matching single-eye RT");

    in.hasDsv = false;
    Check(!vrpacing::EvaluateVrsCandidate(in).accepted, "vrs_filter: rejects RT without DSV");

    in.hasDsv = true;
    in.rtW = 3296;
    in.dsvW = 3296;
    const auto okDw = vrpacing::EvaluateVrsCandidate(in);
    Check(okDw.accepted && okDw.doubleWide, "vrs_filter: accepts matching double-wide RT");

    Check(vrpacing::IsVrsAutoExcludedExeName(L"hlvr.exe"), "vrs_filter: auto-excludes hlvr.exe");
    Check(vrpacing::IsVrsAutoExcludedExeName(L"thelab.exe"), "vrs_filter: auto-excludes thelab.exe");
    Check(!vrpacing::IsVrsAutoExcludedExeName(L"metroexodus.exe"), "vrs_filter: does not exclude metroexodus.exe");
}
#endif

} // namespace

int main() {
    try {
        TestMaskGeometry();
        TestLimiterMath();
        TestHzTracker();
        TestSharedStateAbi();
        TestTelemetryAdvisorMode();
#ifdef _WIN32
        TestVrsFilter();
#endif
    } catch (const std::exception& e) {
        std::cout << "[EXCEPTION] " << e.what() << "\n";
        return 1;
    }

    if (g_failed == 0) {
        std::cout << "All unit tests passed.\n";
        return 0;
    }

    std::cout << "Failures: " << g_failed << "\n";
    return 1;
}