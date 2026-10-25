#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cwchar>

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

        view_ = MapViewOfFile(mapFile_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        if (!view_) {
            CloseHandle(mapFile_);
            mapFile_ = nullptr;
            return false;
        }

        shared_ = reinterpret_cast<SharedState*>(view_);
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

    void SyncConfig(RuntimeConfig& cfg) {
        if (!OpenOrCreate() || !shared_) return;
        if (shared_->version != 2) return;

        const uint32_t flags = shared_->flags;
        cfg.enabled = (flags & kFlagEnabled) != 0;
        cfg.pacing.enabled = cfg.enabled && ((flags & kFlagPacingEnabled) != 0);
        cfg.upscaling.enabled = cfg.enabled && ((flags & kFlagUpscalingEnabled) != 0);
        cfg.mask.enabled = cfg.enabled && ((flags & kFlagMaskEnabled) != 0);

        // Locked NIS Sweet Spot
        cfg.upscaling.scale = 2.0f;
        cfg.upscaling.radius = 0.60f;
        cfg.upscaling.sharpness = 0.0f;

        if (shared_->frameGenDivisor > 0.0) {
            cfg.pacing.frameGenDivisor = shared_->frameGenDivisor;
        }
        if (shared_->offsetFps >= 0.0) {
            cfg.pacing.offsetFps = shared_->offsetFps;
        }
    }

    void UpdateTelemetry(const wchar_t* apiName, double effectiveHz, double appliedCap) {
        if (!OpenOrCreate() || !shared_) return;

        shared_->effectiveHz = effectiveHz;
        shared_->appliedCap = appliedCap;
        shared_->frameCounter++;
        if (apiName && apiName[0] != L'\0') {
            wcscpy_s(shared_->activeApi, apiName);
        }
    }

    SharedState* GetRaw() { return shared_; }

private:
    HANDLE mapFile_ = nullptr;
    void* view_ = nullptr;
    SharedState* shared_ = nullptr;
};

} // namespace vrpacing

#endif