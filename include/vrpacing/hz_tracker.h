#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "vrpacing/frame_limiter_math.h"

namespace vrpacing {

struct HzTrackerConfig {
    size_t windowSize = 9;
    double emaAlpha = 0.25;
    double minHz = 10.0;
    double maxHz = 500.0;
};

class HzTracker {
public:
    explicit HzTracker(HzTrackerConfig cfg = {});

    bool PushPeriodNs(int64_t periodNs);
    bool PushHz(double hz);

    std::optional<double> GetEffectiveHz() const;
    double ComputeTargetFps(const LimitInputs& in) const;
    bool IsLikelyRuntimeHalfRate() const;

private:
    bool PushValidatedHz(double hz);
    double MedianWindow() const;

    HzTrackerConfig cfg_;
    std::deque<double> window_;
    std::optional<double> emaHz_;
    std::optional<double> lastRawHz_;
    bool likelyHalfRate_ = false;
};

} // namespace vrpacing
