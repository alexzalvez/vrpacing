#ifdef _WIN32

#include "frame_limiter_win.h"

#include <windows.h>
#include <immintrin.h>

#include "vrpacing/frame_limiter_math.h"

namespace vrpacing {

FrameLimiterWin::FrameLimiterWin() = default;

FrameLimiterWin::~FrameLimiterWin() {
    if (timerHandle_) {
        CloseHandle(static_cast<HANDLE>(timerHandle_));
        timerHandle_ = nullptr;
    }
}

bool FrameLimiterWin::Initialize() {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq)) {
        return false;
    }
    qpcFreq_ = freq.QuadPart;

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        return false;
    }

    timerHandle_ = timer;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    nextDeadline_ = now.QuadPart;

    return true;
}

void FrameLimiterWin::SetTargetFps(double targetFps) {
    periodTicks_ = ComputeFramePeriodTicks(targetFps, qpcFreq_);
}

void FrameLimiterWin::WaitBeforeRuntime() {
    if (periodTicks_ <= 0 || !timerHandle_) {
        return;
    }

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (nextDeadline_ == 0) {
        nextDeadline_ = now.QuadPart + periodTicks_;
    }

    nextDeadline_ = AdvanceDeadlineNoDrift(nextDeadline_, now.QuadPart, periodTicks_);

    const int64_t ticksLeft = nextDeadline_ - now.QuadPart;
    if (ticksLeft <= 0) {
        return;
    }

    const double secLeft = static_cast<double>(ticksLeft) / static_cast<double>(qpcFreq_);
    if (secLeft > 0.0005) {
        const int64_t ns = static_cast<int64_t>((secLeft - 0.00025) * 1e9);
        LARGE_INTEGER due{};
        due.QuadPart = -ns / 100;
        SetWaitableTimerEx(static_cast<HANDLE>(timerHandle_), &due, 0, nullptr, nullptr, nullptr, 0);
        WaitForSingleObject(static_cast<HANDLE>(timerHandle_), INFINITE);
    }

    while (true) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= nextDeadline_) {
            break;
        }
        _mm_pause();
    }
}

} // namespace vrpacing

#endif
