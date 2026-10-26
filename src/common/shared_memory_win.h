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
        if (newlyCreated || shared_->version != 5) {
            *shared_ = {};
            shared_->version = 5;
            shared_->flags = kFlagEnabled | kFlagUpscalingEnabled | kFlagPacingEnabled;
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

    void SyncConfig(RuntimeConfig& cfg) {
        // Garantizar Sweet Spot activo por defecto aunque falle IPC
        cfg.upscaling.scale = 2.0f;
        cfg.upscaling.radius = 0.60f;
        cfg.upscaling.sharpness = 0.0f;

        if (!OpenOrCreate() || !shared_ || shared_->version != 5) {
            cfg.enabled = true;
            cfg.upscaling.enabled = true;
            cfg.pacing.enabled = true;
            return;
        }

        const uint32_t flags = shared_->flags;
        cfg.enabled = (flags & kFlagEnabled) != 0;
        cfg.pacing.enabled = cfg.enabled && ((flags & kFlagPacingEnabled) != 0);
        cfg.upscaling.enabled = cfg.enabled && ((flags & kFlagUpscalingEnabled) != 0);
        cfg.mask.enabled = false;

        if (shared_->frameGenDivisor > 0.0) {
            cfg.pacing.frameGenDivisor = shared_->frameGenDivisor;
        }
        if (shared_->offsetFps >= 0.0) {
            cfg.pacing.offsetFps = shared_->offsetFps;
        }
    }

    void UpdateTelemetry(const wchar_t* apiName, double effectiveHz, double appliedCap,
                         uint32_t inW = 0, uint32_t inH = 0,
                         uint32_t outW = 0, uint32_t outH = 0,
                         uint32_t dxgiFmt = 0, int32_t status = 0,
                         int32_t hresult = 0) {
        if (!OpenOrCreate() || !shared_) return;

        shared_->activePid = GetCurrentProcessId();
        shared_->effectiveHz = effectiveHz;
        shared_->appliedCap = appliedCap;
        shared_->frameCounter++;
        if (apiName && apiName[0] != L'\0') {
            wcscpy_s(shared_->activeApi, apiName);
        }
        if (inW > 0) shared_->inputWidth = inW;
        if (inH > 0) shared_->inputHeight = inH;
        if (outW > 0) shared_->outputWidth = outW;
        if (outH > 0) shared_->outputHeight = outH;
        if (dxgiFmt > 0) shared_->dxgiFormat = dxgiFmt;
        if (status != 0) shared_->upscaleStatus = status;
        if (hresult != 0) shared_->lastHResult = hresult;

        if (shared_->hookedProcess[0] == L'\0' || wcscmp(shared_->hookedProcess, L"None") == 0) {
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
    }

    SharedState* GetRaw() { return shared_; }

private:
    HANDLE mapFile_ = nullptr;
    void* view_ = nullptr;
    SharedState* shared_ = nullptr;
};

} // namespace vrpacing

#endif