#include "vrpacing/hz_tracker.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vrpacing {

std::optional<double> HzTracker::SnapToCommonHz(double hz) {
    static constexpr double kCommonRates[] = {
        24.0, 30.0, 32.0, 33.333333, 36.0, 40.0, 45.0, 48.0, 50.0,
        60.0, 72.0, 80.0, 90.0, 96.0, 100.0, 120.0, 144.0
    };
    constexpr double kTolerance = 0.03; // 3% de tolerancia para distinguir 90, 96 y 100 Hz

    double bestDiff = 1e9;
    std::optional<double> bestRate = std::nullopt;

    for (double common : kCommonRates) {
        const double diff = std::abs(hz - common);
        if (diff / common <= kTolerance) {
            if (diff < bestDiff) {
                bestDiff = diff;
                bestRate = common;
            }
        }
    }
    return bestRate;
}

bool HzTracker::PushPeriodNs(int64_t periodNs) {
    if (periodNs <= 0) return false;
    const double hz = 1e9 / static_cast<double>(periodNs);
    PushHz(hz);
    return true;
}

void HzTracker::PushHz(double hz) {
    if (hz < 20.0 || hz > 200.0) return;

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

    auto snapped = SnapToCommonHz(median);
    if (snapped) {
        return snapped;
    }

    for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) {
        auto s = SnapToCommonHz(*it);
        if (s) {
            return s;
        }
    }

    return 90.0;
}

double HzTracker::ComputeTargetFps(const LimitInputs& in) const {
    LimitInputs copy = in;
    if (copy.hz <= 0.0) {
        copy.hz = GetEffectiveHz().value_or(90.0);
    }
    return vrpacing::ComputeTargetFps(copy);
}

bool HzTracker::IsLikelyRuntimeHalfRate() const {
    const auto eff = GetEffectiveHz();
    if (!eff) return false;
    const double h = *eff;
    return (std::abs(h - 45.0) < 1.0 || std::abs(h - 36.0) < 1.0 || std::abs(h - 40.0) < 1.0 || std::abs(h - 60.0) < 1.0 || std::abs(h - 48.0) < 1.0);
}

void HzTracker::Reset() {
    samples_.clear();
}

} // namespace vrpacing