#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vrpacing/frame_limiter_math.h"

namespace vrpacing {

class HzTracker {
public:
    HzTracker() = default;

    bool PushPeriodNs(int64_t periodNs);
    void PushHz(double hz);
    std::optional<double> GetEffectiveHz() const;
    double ComputeTargetFps(const LimitInputs& in) const;
    bool IsLikelyRuntimeHalfRate() const;
    void Reset();

    static std::optional<double> SnapToCommonHz(double hz);

private:
    std::vector<double> samples_;
};

} // namespace vrpacing