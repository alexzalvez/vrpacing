#pragma once

#include <cstdint>

namespace vrpacing {

struct LimitInputs {
    double hz = 90.0;
    double afrMultiplier = 1.0;
    double frameGenDivisor = 1.0;
    double offsetFps = 0.1;
};

double ComputeTargetFps(const LimitInputs& in);
int64_t ComputeFramePeriodTicks(double targetFps, int64_t qpcFrequency);
int64_t AdvanceDeadlineNoDrift(int64_t prevDeadlineTicks, int64_t nowTicks, int64_t periodTicks);

} // namespace vrpacing
