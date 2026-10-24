#include "vrpacing/frame_limiter_math.h"

#include <algorithm>
#include <cmath>

namespace vrpacing {

double ComputeTargetFps(const LimitInputs& in) {
    const double divisor = (in.frameGenDivisor <= 0.0) ? 1.0 : in.frameGenDivisor;
    const double afr = (in.afrMultiplier <= 0.0) ? 1.0 : in.afrMultiplier;
    const double raw = (in.hz * afr / divisor) - in.offsetFps;
    return std::clamp(raw, 10.0, 500.0);
}

int64_t ComputeFramePeriodTicks(double targetFps, int64_t qpcFrequency) {
    if (targetFps <= 0.0 || qpcFrequency <= 0) {
        return 0;
    }
    const double ticks = static_cast<double>(qpcFrequency) / targetFps;
    return std::max<int64_t>(1, static_cast<int64_t>(std::llround(ticks)));
}

int64_t AdvanceDeadlineNoDrift(int64_t prevDeadlineTicks, int64_t nowTicks, int64_t periodTicks) {
    if (periodTicks <= 0) {
        return nowTicks;
    }

    int64_t next = prevDeadlineTicks + periodTicks;
    if (next < nowTicks - periodTicks) {
        const int64_t behind = nowTicks - next;
        const int64_t skip = behind / periodTicks;
        next += skip * periodTicks;
        while (next < nowTicks) {
            next += periodTicks;
        }
    }
    return next;
}

} // namespace vrpacing
