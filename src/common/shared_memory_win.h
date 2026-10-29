#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

#include "vrpacing/config.h"
#include "vrpacing/shared_state.h"

namespace vrpacing {

class SharedMemoryClient {
public:
    SharedMemoryClient() = default;
    ~SharedMemoryClient() {
        Close();
    }

    bool OpenOrCreate() {
        if (shared_) return true;

        mapFile_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            sizeof(SharedState),
            kSharedStateName
        );

        if (!mapFile_) {
            return false;
        }

        const bool newlyCreated = (GetLastError() != ERROR_ALREADY_EXISTS);

        view_ = MapViewOfFile(mapFile_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        if (!view_) {
            CloseHandle(mapFile_);
            mapFile_ = nullptr;
            return false;
        }

        shared_ = reinterpret_cast<SharedState*>(view_);
        if (newlyCreated || shared_->version != 6) {
            *shared_ = {};
            shared_->version = 6;
            shared_->flags = kFlagEnabled | kFlagUpscalingEnabled | kFlagPacingEnabled | kFlagTelemetryActive;
            shared_->frameGenDivisor = 1.0;
            shared_->offsetFps = 0.10;
            wcscpy_s(shared_->activeApi, L"Idle");
            wcscpy_s(shared_->hookedProcess, L"None");
        }
        return true;
    }

    void Close() {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapFile_) {
            CloseHandle(mapFile_);
            mapFile_ = nullptr;
        }
        shared_ = nullptr;
    }

    void SetOpenXrD3D11Pid(uint32_t pid) {
        if (OpenOrCreate() && shared_) {
            shared_->openXrD3D11Pid = pid;
        }
    }

    bool IsOpenXrD3D11ActiveForCurrentProcess() {
        if (!OpenOrCreate() || !shared_) return false;
        return (shared_->openXrD3D11Pid != 0 && shared_->openXrD3D11Pid == GetCurrentProcessId());
    }

    bool IsTelemetryActive() {
        if (!OpenOrCreate() || !shared_) return false;
        return (shared_->flags & kFlagTelemetryActive) != 0;
    }

    void SyncConfig(RuntimeConfig& cfg) {
        cfg.upscaling.scale = 2.0f;
        cfg.upscaling.radius = 0.60f;
        cfg.upscaling.sharpness = 0.0f;

        if (!OpenOrCreate() || !shared_ || shared_->version != 6) {
            cfg.enabled = true;
            cfg.upscaling.enabled = true;
            cfg.pacing.enabled = true;
            cfg.mask.enabled = false;
            cfg.ffr.enabled = false;
            return;
        }

        const uint32_t flags = shared_->flags;
        cfg.enabled = (flags & kFlagEnabled) != 0;
        cfg.pacing.enabled = cfg.enabled;
        shared_->flags |= kFlagPacingEnabled;
        cfg.upscaling.enabled = cfg.enabled && ((flags & kFlagUpscalingEnabled) != 0);

        cfg.mask.enabled = cfg.enabled && ((flags & kFlagMaskEnabled) != 0);
        cfg.mask.radiusX = 1.00f;
        cfg.mask.radiusY = 1.00f;
        cfg.mask.segments = 256;

        cfg.ffr.enabled = cfg.enabled && ((flags & kFlagFfrEnabled) != 0);
        cfg.ffr.innerRadius = 0.60f;

        if (shared_->frameGenDivisor > 0.0) {
            cfg.pacing.frameGenDivisor = shared_->frameGenDivisor;
        }
        if (shared_->offsetFps >= 0.0) {
            cfg.pacing.offsetFps = shared_->offsetFps;
        }
    }

    void UpdateResolutionAndStatus(uint32_t inW, uint32_t inH,
                                   uint32_t outW, uint32_t outH,
                                   uint32_t dxgiFmt, int32_t status,
                                   int32_t hresult = 0) {
        if (!OpenOrCreate() || !shared_) return;
        if (inW > 0) shared_->inputWidth = inW;
        if (inH > 0) shared_->inputHeight = inH;
        if (outW > 0) shared_->outputWidth = outW;
        if (outH > 0) shared_->outputHeight = outH;
        if (dxgiFmt > 0) shared_->dxgiFormat = dxgiFmt;
        if (status != 0) shared_->upscaleStatus = status;
        if (hresult != 0) shared_->lastHResult = hresult;
    }

    void UpdateTelemetry(const wchar_t* apiName, double effectiveHz, double appliedCap,
                         uint32_t inW = 0, uint32_t inH = 0,
                         uint32_t outW = 0, uint32_t outH = 0,
                         uint32_t dxgiFmt = 0, int32_t status = 0,
                         int32_t hresult = 0) {
        if (!OpenOrCreate() || !shared_) return;

        const DWORD currentPid = GetCurrentProcessId();
        const bool pidChanged = (shared_->activePid != currentPid);

        if (pidChanged) {
            shared_->inputWidth = 0;
            shared_->inputHeight = 0;
            shared_->outputWidth = 0;
            shared_->outputHeight = 0;
            shared_->dxgiFormat = 0;
            shared_->upscaleStatus = 0;
            shared_->lastHResult = 0;
            shared_->avgFps = 0.0;
            shared_->onePercentLowFps = 0.0;
        }

        shared_->activePid = currentPid;
        shared_->effectiveHz = effectiveHz;
        shared_->appliedCap = appliedCap;
        shared_->frameCounter++;

        if (apiName && apiName[0] != L'\0' && wcscmp(shared_->activeApi, apiName) != 0) {
            wcscpy_s(shared_->activeApi, apiName);
        }
        if (inW > 0) shared_->inputWidth = inW;
        if (inH > 0) shared_->inputHeight = inH;
        if (outW > 0) shared_->outputWidth = outW;
        if (outH > 0) shared_->outputHeight = outH;
        if (dxgiFmt > 0) shared_->dxgiFormat = dxgiFmt;
        if (status != 0) shared_->upscaleStatus = status;
        if (hresult != 0) shared_->lastHResult = hresult;

        if (pidChanged || shared_->hookedProcess[0] == L'\0' || wcscmp(shared_->hookedProcess, L"None") == 0) {
            wchar_t modPath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modPath, MAX_PATH)) {
                std::wstring p(modPath);
                const auto pos = p.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    p = p.substr(pos + 1);
                }
                wcsncpy_s(shared_->hookedProcess, p.c_str(), _TRUNCATE);
            }
        }

        if (!IsTelemetryActive()) {
            return;
        }

        static int64_t s_lastQpc = 0;
        static int64_t s_qpcFreq = 0;
        static double s_samples[180]{};
        static size_t s_sampleIdx = 0;
        static size_t s_sampleCount = 0;
        static uint32_t s_calcCounter = 0;

        if (s_qpcFreq == 0) {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            s_qpcFreq = f.QuadPart;
        }

        LARGE_INTEGER nowQpc{};
        QueryPerformanceCounter(&nowQpc);

        if (s_lastQpc > 0 && s_qpcFreq > 0) {
            const double dt = static_cast<double>(nowQpc.QuadPart - s_lastQpc) / static_cast<double>(s_qpcFreq);
            if (dt > 0.002 && dt < 0.100) {
                s_samples[s_sampleIdx] = dt;
                s_sampleIdx = (s_sampleIdx + 1) % 180;
                if (s_sampleCount < 180) s_sampleCount++;

                s_calcCounter++;
                if (s_calcCounter >= 30 && s_sampleCount >= 30) {
                    s_calcCounter = 0;
                    double sumDt = 0.0;
                    double sorted[180]{};

                    for (size_t i = 0; i < s_sampleCount; ++i) {
                        sumDt += s_samples[i];
                        sorted[i] = s_samples[i];
                    }

                    if (sumDt > 0.0) {
                        shared_->avgFps = static_cast<double>(s_sampleCount) / sumDt;
                    }

                    std::sort(sorted, sorted + s_sampleCount);
                    const size_t p99Idx = (std::min)(s_sampleCount - 1, static_cast<size_t>(std::floor(s_sampleCount * 0.99)));
                    const double p99Dt = sorted[p99Idx];
                    if (p99Dt > 0.0) {
                        shared_->onePercentLowFps = 1.0 / p99Dt;
                    }
                }
            }
        }
        s_lastQpc = nowQpc.QuadPart;
    }

    SharedState* GetRaw() { return shared_; }

private:
    HANDLE mapFile_ = nullptr;
    void* view_ = nullptr;
    SharedState* shared_ = nullptr;
};

} // namespace vrpacing

#endif