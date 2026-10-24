#pragma once

#ifdef _WIN32

#include <cstdint>

namespace vrpacing {

class FrameLimiterWin {
public:
    FrameLimiterWin();
    ~FrameLimiterWin();

    bool Initialize();
    void SetTargetFps(double targetFps);
    void WaitBeforeRuntime();

private:
    int64_t qpcFreq_ = 0;
    int64_t periodTicks_ = 0;
    int64_t nextDeadline_ = 0;
    void* timerHandle_ = nullptr;
};

} // namespace vrpacing

#endif
