#include "vrpacing/hz_tracker.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vrpacing {

HzTracker::HzTracker(HzTrackerConfig cfg) : cfg_(cfg) {}

bool HzTracker::PushPeriodNs(int64_t periodNs) {
    if (periodNs <= 0) {
        return false;
    }
    const double hz = 1e9 / static_cast<double>(periodNs);
    return PushValidatedHz(hz);
}

bool HzTracker::PushHz(double hz) {
    return PushValidatedHz(hz);
}

bool HzTracker::PushValidatedHz(double hz) {
    if (!std::isfinite(hz) || hz < cfg_.minHz || hz > cfg_.maxHz) {
        return false;
    }

    lastRawHz_ = hz;
    window_.push_back(hz);
    while (window_.size() > cfg_.windowSize) {
        window_.pop_front();
    }

    if (!emaHz_) {
        emaHz_ = hz;
    } else {
        emaHz_ = (cfg_.emaAlpha * hz) + ((1.0 - cfg_.emaAlpha) * (*emaHz_));
    }

    if (emaHz_ && hz > 0.0) {
        const double ratio = *emaHz_ / hz;
        likelyHalfRate_ = std::abs(ratio - 2.0) < 0.08 || std::abs((1.0 / ratio) - 2.0) < 0.08;
    }

    return true;
}

std::optional<double> HzTracker::GetEffectiveHz() const {
    if (!emaHz_) {
        return std::nullopt;
    }
    if (window_.size() < 3) {
        return emaHz_;
    }

    const double med = MedianWindow();
    return (*emaHz_ * 0.65) + (med * 0.35);
}

double HzTracker::ComputeTargetFps(const LimitInputs& in) const {
    LimitInputs adjusted = in;
    if (const auto hz = GetEffectiveHz()) {
        adjusted.hz = *hz;
    }
    return vrpacing::ComputeTargetFps(adjusted);
}

bool HzTracker::IsLikelyRuntimeHalfRate() const {
    return likelyHalfRate_;
}

double HzTracker::MedianWindow() const {
    std::vector<double> sorted(window_.begin(), window_.end());
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    if (n == 0) {
        return 0.0;
    }
    if (n % 2 == 1) {
        return sorted[n / 2];
    }
    return 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
}

} // namespace vrpacing
