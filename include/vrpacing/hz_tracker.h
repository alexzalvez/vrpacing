#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace vrpacing {

class HzTracker {
public:
    HzTracker() = default;
    void PushHz(double hz);
    std::optional<double> GetEffectiveHz() const;
    void Reset();

    static std::optional<double> SnapToCommonHz(double hz);

private:
    std::vector<double> samples_;
};

} // namespace vrpacing