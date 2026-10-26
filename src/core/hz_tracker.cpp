#include "vrpacing/hz_tracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vrpacing {

std::optional<double> HzTracker::SnapToCommonHz(double hz) {
    static constexpr double kCommonRates[] = {
        30.0, 36.0, 40.0, 45.0, 60.0, 72.0, 80.0, 90.0, 120.0, 144.0
    };
    constexpr double kTolerance = 0.06; // 6%

    for (double common : kCommonRates) {
        const double diff = std::abs(hz - common);
        if (diff / common <= kTolerance) {
            return common;
        }
    }
    return std::nullopt;
}

void HzTracker::PushHz(double hz) {
    if (hz < 25.0 || hz > 200.0) return;

    samples_.push_back(hz);
    if (samples_.size() > 60) {
        samples_.erase(samples_.begin());
    }
}

std::optional<double> HzTracker::GetEffectiveHz() const {
    if (samples_.empty()) return std::nullopt;

    std::vector<double> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];

    return SnapToCommonHz(median).value_or(median);
}

void HzTracker::Reset() {
    samples_.clear();
}

} // namespace vrpacing