#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <string>

#include "common/config_json.h"
#include "common/logging_win.h"
#include "injector/process_scanner.h"
#include "vrpacing/config.h"
#include "vrpacing/shared_state.h"
#include "vrpacing/telemetry_advisor.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR IDT_SCANNER_TIMER = 101;
constexpr UINT_PTR IDT_TELEMETRY_TIMER = 102;
constexpr int ID_HOTKEY_SCREENSHOT = 9001;

// UI Control IDs
constexpr UINT ID_CHECK_NIS = 2001;
constexpr UINT ID_RADIO_1X = 2003;
constexpr UINT ID_RADIO_HALF = 2004;
constexpr UINT ID_BTN_MINIMIZE = 2009;
constexpr UINT ID_BTN_OPEN_LOG = 2010;
constexpr UINT ID_BTN_EXIT = 2011;
constexpr UINT ID_CHECK_MASK = 2012;
constexpr UINT ID_CHECK_FFR = 2013;
constexpr UINT ID_BTN_RESET_STATS = 2015;
constexpr UINT ID_BTN_OPEN_SHOTS = 2016;
constexpr UINT ID_CHECK_SMAA = 2017;

// Tray Menu IDs
constexpr UINT ID_TRAY_OPEN = 3001;
constexpr UINT ID_TRAY_NIS = 3002;
constexpr UINT ID_TRAY_RATE_1X = 3004;
constexpr UINT ID_TRAY_RATE_HALF = 3005;
constexpr UINT ID_TRAY_OPEN_LOG = 3010;
constexpr UINT ID_TRAY_EXIT = 3011;
constexpr UINT ID_TRAY_MASK = 3012;
constexpr UINT ID_TRAY_FFR = 3013;
constexpr UINT ID_TRAY_TAKE_SHOT = 3014;
constexpr UINT ID_TRAY_OPEN_SHOTS = 3015;
constexpr UINT ID_TRAY_SMAA = 3016;

constexpr RECT kGraphRect = { 30, 172, 434, 246 };

NOTIFYICONDATAW g_nid{};
wchar_t g_lastTrayTip[128]{};
bool g_uiIdleRendered = false;

vrpacing::RuntimeConfig g_cfg;
bool g_smaaEnabled = false; // Always unchecked on startup by design
vrpacing::SharedState* g_shared = nullptr;
HANDLE g_mapFile = nullptr;
vrpacing::ProcessScanner g_scanner;
std::wstring g_hookPath;
std::wstring g_screenshotsDir;

HWND g_hMainWnd = nullptr;
HWND g_hTooltip = nullptr;
HFONT g_hFontNormal = nullptr;
HFONT g_hFontBold = nullptr;
HFONT g_hFontSmall = nullptr;

// Cached GDI objects for zero-allocation graph rendering
HBRUSH g_hBrushGraphBg = nullptr;
HPEN g_hPenGraphBorder = nullptr;
HPEN g_hPenGraphGrid = nullptr;
HPEN g_hPenGraphTarget = nullptr;
HPEN g_hPenGraphLine = nullptr;

HWND g_hLblApi = nullptr;
HWND g_hLblHz = nullptr;
HWND g_hLblCap = nullptr;
HWND g_hLblFps = nullptr;
HWND g_hLblProcess = nullptr;
HWND g_hLblRes = nullptr;
HWND g_hLblPipeline = nullptr;
HWND g_hLblAdvisor = nullptr;
HWND g_hLblShotInfo = nullptr;

std::wstring g_cacheApi;
std::wstring g_cacheHz;
std::wstring g_cacheCap;
std::wstring g_cacheFps;
std::wstring g_cacheProc;
std::wstring g_cacheRes;
std::wstring g_cachePipe;
std::wstring g_cacheAdvisor;
std::wstring g_cacheShot;

static vrpacing::TelemetryAnalyzer g_advisor;

HWND g_hChkNis = nullptr;
HWND g_hChkSmaa = nullptr;
HWND g_hChkFfr = nullptr;
HWND g_hChkMask = nullptr;
HWND g_hRadio1x = nullptr;
HWND g_hRadioHalf = nullptr;

void SetWindowTextIfChanged(HWND hCtrl, const wchar_t* newText, std::wstring& cache) {
    if (!hCtrl || !newText) return;
    if (cache != newText) {
        cache = newText;
        SetWindowTextW(hCtrl, newText);
    }
}

void UpdateTrayTooltipIfChanged(const wchar_t* newTip) {
    if (!newTip) return;
    if (wcscmp(g_lastTrayTip, newTip) != 0) {
        wcscpy_s(g_lastTrayTip, newTip);
        wcscpy_s(g_nid.szTip, newTip);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

void AttachControlTooltip(HWND hParent, HWND hCtrl, const wchar_t* text) {
    if (!g_hTooltip || !hCtrl || !text) return;
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = hParent;
    ti.uId = reinterpret_cast<UINT_PTR>(hCtrl);
    ti.lpszText = const_cast<LPWSTR>(text);
    if (!SendMessageW(g_hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti))) {
        ti.cbSize = TTTOOLINFOW_V1_SIZE;
        SendMessageW(g_hTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

bool IsCurrentProcessElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD cbSize = sizeof(TOKEN_ELEVATION);
    bool isElevated = false;
    if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
        isElevated = (elevation.TokenIsElevated != 0);
    }
    CloseHandle(hToken);
    return isElevated;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

void TriggerComparisonScreenshot() {
    if (!g_shared) return;
    if (g_shared->activePid != 0 && g_cfg.upscaling.enabled) {
        g_shared->screenshotRequest = 1;
        MessageBeep(MB_OK);
    } else {
        MessageBeep(MB_ICONWARNING);
    }
}

void OpenScreenshotsFolder() {
    std::error_code ec;
    std::filesystem::create_directories(g_screenshotsDir, ec);
    ShellExecuteW(nullptr, L"open", g_screenshotsDir.c_str(), nullptr, nullptr, SW_SHOW);
}

void RegisterOpenXrImplicitLayer() {
    const std::wstring jsonPath = ExeDir() + L"\\VRPacingLayer.json";
    if (!std::filesystem::exists(jsonPath)) {
        return;
    }

    const wchar_t* subKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
    DWORD val = 0;

    HKEY hKeyLm = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKeyLm, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKeyLm, jsonPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKeyLm);

        HKEY hKeyCu = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKeyCu) == ERROR_SUCCESS) {
            RegDeleteValueW(hKeyCu, jsonPath.c_str());
            RegCloseKey(hKeyCu);
        }
        vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR implicit layer registered in HKLM (single-instance mode).");
        return;
    }

    HKEY hKeyCu = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKeyCu, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKeyCu, jsonPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKeyCu);
        vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR implicit layer registered in HKCU.");
    }
}

void UnregisterOpenXrImplicitLayer() {
    const std::wstring jsonPath = ExeDir() + L"\\VRPacingLayer.json";
    const wchar_t* subKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
    DWORD val = 1;

    HKEY hKeyCu = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &hKeyCu) == ERROR_SUCCESS) {
        RegDeleteValueW(hKeyCu, jsonPath.c_str());
        RegCloseKey(hKeyCu);
    }

    HKEY hKeyLm = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_SET_VALUE, &hKeyLm) == ERROR_SUCCESS) {
        RegSetValueExW(hKeyLm, jsonPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKeyLm);
        vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR implicit layer disabled in HKLM (application closed).");
    }
}

void UpdateSharedStateFlags() {
    if (!g_shared) return;
    uint32_t flags = 0;
    if (g_cfg.enabled) flags |= vrpacing::kFlagEnabled;
    if (g_cfg.upscaling.enabled) flags |= vrpacing::kFlagUpscalingEnabled;
    if (g_smaaEnabled) flags |= vrpacing::kFlagSmaaEnabled;
    if (g_cfg.mask.enabled) flags |= vrpacing::kFlagMaskEnabled;
    if (g_cfg.ffr.enabled) {
        flags |= vrpacing::kFlagFfrEnabled;
        flags |= vrpacing::kFlagFfrOpenXrEnabled;
    }
    flags |= vrpacing::kFlagPacingEnabled;
    if (g_hMainWnd && IsWindowVisible(g_hMainWnd)) {
        flags |= vrpacing::kFlagTelemetryActive;
    }

    g_shared->flags = flags;
    g_shared->frameGenDivisor = (g_cfg.pacing.frameGenDivisor > 0.0) ? g_cfg.pacing.frameGenDivisor : 1.0;
    g_shared->offsetFps = 0.10;
    if (!g_screenshotsDir.empty()) {
        wcsncpy_s(g_shared->screenshotDir, g_screenshotsDir.c_str(), _TRUNCATE);
    }
}

void SyncUiWithConfig() {
    if (g_hChkNis) {
        SendMessageW(g_hChkNis, BM_SETCHECK, g_cfg.upscaling.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkSmaa) {
        SendMessageW(g_hChkSmaa, BM_SETCHECK, g_smaaEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkFfr) {
        SendMessageW(g_hChkFfr, BM_SETCHECK, g_cfg.ffr.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkMask) {
        SendMessageW(g_hChkMask, BM_SETCHECK, g_cfg.mask.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    const double divisor = g_cfg.pacing.frameGenDivisor;
    const bool isHalf = (divisor > 1.05);

    if (g_hRadio1x) SendMessageW(g_hRadio1x, BM_SETCHECK, (!isHalf) ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hRadioHalf) SendMessageW(g_hRadioHalf, BM_SETCHECK, isHalf ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SaveAndPropagate() {
    g_cfg.upscaling.scale = 2.0f;
    g_cfg.upscaling.radius = 0.60f;
    g_cfg.upscaling.sharpness = 0.0f;
    g_cfg.mask.radiusX = 1.00f;
    g_cfg.mask.radiusY = 1.00f;
    g_cfg.mask.segments = 256;
    g_cfg.ffr.innerRadius = 0.60f;
    g_cfg.ffr.openxrEnabled = g_cfg.ffr.enabled;
    g_cfg.pacing.enabled = true;
    g_cfg.pacing.offsetFps = 0.10;

    if (g_cfg.pacing.frameGenDivisor > 1.05) {
        g_cfg.pacing.frameGenDivisor = 2.0;
    } else {
        g_cfg.pacing.frameGenDivisor = 1.0;
    }

    vrpacing::SaveConfig(g_cfg);
    UpdateSharedStateFlags();
    SyncUiWithConfig();
    g_uiIdleRendered = false; // Force immediate label refresh on user toggle
}

void ResetSharedSessionToIdle() {
    if (!g_shared) return;
    const DWORD deadPid = g_shared->activePid;
    g_shared->activePid = 0;
    if (g_shared->openXrD3D11Pid == deadPid) {
        g_shared->openXrD3D11Pid = 0;
    }
    wcscpy_s(g_shared->activeApi, L"Idle");
    wcscpy_s(g_shared->hookedProcess, L"None");
    g_shared->effectiveHz = 0.0;
    g_shared->inputWidth = 0;
    g_shared->inputHeight = 0;
    g_shared->outputWidth = 0;
    g_shared->outputHeight = 0;
    g_shared->dxgiFormat = 0;
    g_shared->upscaleStatus = 0;
    g_shared->lastHResult = 0;
    g_shared->appliedCap = 0.0;
    g_shared->avgFps = 0.0;
    g_shared->onePercentLowFps = 0.0;
    g_shared->frameCounter = 0;
    g_shared->rawPanelHz = 0.0;
    g_shared->sswActive = 0;
    g_shared->vrsStatus = 0;
    g_shared->maskStatus = 0;
    g_shared->smaaStatus = 0;
    g_shared->frameTimeWriteIdx = 0;
    g_shared->screenshotRequest = 0;
    for (size_t i = 0; i < 256; ++i) {
        g_shared->frameTimesMs[i] = 0.0f;
    }
    g_advisor.Reset();
    g_uiIdleRendered = false;
}

void CheckActiveProcessLiveness() {
    if (!g_shared || g_shared->activePid == 0) return;

    HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, g_shared->activePid);
    if (hProc) {
        DWORD waitRes = WaitForSingleObject(hProc, 0);
        CloseHandle(hProc);
        if (waitRes == WAIT_OBJECT_0) {
            ResetSharedSessionToIdle();
            vrpacing::LogMessage(vrpacing::LogLevel::Info, "VR game closed. Telemetry and IPC state reset to Idle.");
        }
    } else if (GetLastError() == ERROR_INVALID_PARAMETER) {
        ResetSharedSessionToIdle();
    }
}

void DrawFrametimeGraph(HDC hdcWindow) {
    const int w = kGraphRect.right - kGraphRect.left;
    const int h = kGraphRect.bottom - kGraphRect.top;
    if (w <= 0 || h <= 0) return;

    HDC memDc = CreateCompatibleDC(hdcWindow);
    HBITMAP memBmp = CreateCompatibleBitmap(hdcWindow, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    RECT rcLocal{ 0, 0, w, h };
    FillRect(memDc, &rcLocal, g_hBrushGraphBg);

    HGDIOBJ oldPen = SelectObject(memDc, g_hPenGraphBorder);
    HGDIOBJ oldBrush = SelectObject(memDc, GetStockObject(NULL_BRUSH));
    Rectangle(memDc, 0, 0, w, h);

    constexpr float kMaxMs = 33.33f;
    auto MsToY = [h, kMaxMs](float ms) -> int {
        const float clamped = std::clamp(ms, 0.0f, kMaxMs);
        const float norm = clamped / kMaxMs;
        return (h - 2) - static_cast<int>( std::lround(norm * static_cast<float>(h - 4)) );
    };

    SelectObject(memDc, g_hPenGraphGrid);
    const float kGuides[] = { 8.33f, 11.11f, 16.67f, 22.22f };
    for (float gMs : kGuides) {
        const int y = MsToY(gMs);
        MoveToEx(memDc, 1, y, nullptr);
        LineTo(memDc, w - 1, y);
    }

    const bool hasActiveSession = (g_shared && g_shared->activePid != 0 && g_shared->appliedCap > 0.0);
    if (hasActiveSession) {
        const float targetMs = static_cast<float>(1000.0 / g_shared->appliedCap);
        const int yTarget = MsToY(targetMs);
        SelectObject(memDc, g_hPenGraphTarget);
        MoveToEx(memDc, 1, yTarget, nullptr);
        LineTo(memDc, w - 1, yTarget);
    }

    const auto& history = g_advisor.GetGraphHistory();
    SelectObject(memDc, g_hPenGraphLine);

    POINT pts[180]{};
    int validCount = 0;
    for (size_t i = 0; i < history.size(); ++i) {
        const float ms = history[i];
        if (ms <= 0.1f) continue;
        const int x = 2 + static_cast<int>((i * static_cast<size_t>(w - 4)) / (history.size() - 1));
        const int y = MsToY(ms);
        pts[validCount++] = POINT{ x, y };
    }
    if (validCount >= 2) {
        Polyline(memDc, pts, validCount);
    }

    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, RGB(155, 165, 180));
    HGDIOBJ oldFont = SelectObject(memDc, g_hFontSmall ? g_hFontSmall : g_hFontNormal);

    wchar_t overlayTxt[96]{};
    if (hasActiveSession) {
        const double targetMs = 1000.0 / g_shared->appliedCap;
        swprintf_s(overlayTxt, L"Frame Interval (0 - 33.3 ms)  |  Target Budget: %.2f ms", targetMs);
    } else {
        wcscpy_s(overlayTxt, L"Frame Interval Graph (0 - 33.3 ms)  |  Idle");
    }
    RECT rcText{ 6, 3, w - 6, 19 };
    DrawTextW(memDc, overlayTxt, -1, &rcText, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    BitBlt(hdcWindow, kGraphRect.left, kGraphRect.top, w, h, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldFont);
    SelectObject(memDc, oldBrush);
    SelectObject(memDc, oldPen);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

void UpdateTelemetryDisplay(bool forceUpdate = false) {
    if (!g_shared) return;

    const bool hasApi = (g_shared->activePid != 0 &&
                         g_shared->activeApi[0] != L'\0' &&
                         wcscmp(g_shared->activeApi, L"Idle") != 0 &&
                         wcscmp(g_shared->activeApi, L"none") != 0);

    // Zero-CPU Idle Gate: if no VR game is active and the Idle state is already rendered, do nothing
    if (!hasApi && g_uiIdleRendered && !forceUpdate) {
        return;
    }

    wchar_t tip[128]{};
    if (hasApi && g_shared->appliedCap > 0.0) {
        bool isSsw = false;
        double targetHz = g_shared->effectiveHz;
        double panelHz = g_shared->rawPanelHz;
        vrpacing::TelemetryAnalyzer::ResolveDisplayMode(*g_shared, isSsw, targetHz, panelHz);
        if (isSsw) {
            swprintf_s(tip, L"VRPacing %s [%s] | %.2f FPS (%.0f/%.0f Hz SSW)",
                       vrpacing::kAppVersionString, g_shared->activeApi, g_shared->appliedCap, targetHz, panelHz);
        } else {
            swprintf_s(tip, L"VRPacing %s [%s] | %.2f FPS (%.0f Hz Native)",
                       vrpacing::kAppVersionString, g_shared->activeApi, g_shared->appliedCap, panelHz);
        }
    } else if (hasApi) {
        swprintf_s(tip, L"VRPacing %s [%s] | Active", vrpacing::kAppVersionString, g_shared->activeApi);
    } else {
        swprintf_s(tip, L"VRPacing %s [Idle - Monitoring]", vrpacing::kAppVersionString);
    }
    UpdateTrayTooltipIfChanged(tip);

    if (!IsWindowVisible(g_hMainWnd)) {
        if (!hasApi) g_uiIdleRendered = true;
        return;
    }

    wchar_t txtApi[64]{};
    wchar_t txtHz[64]{};
    wchar_t txtCap[64]{};
    wchar_t txtFps[96]{};
    wchar_t txtProc[96]{};
    wchar_t txtRes[96]{};
    wchar_t txtPipe[128]{};
    wchar_t txtShot[128]{};

    if (hasApi) {
        swprintf_s(txtApi, L"Active API: %s", g_shared->activeApi);
    } else {
        wcscpy_s(txtApi, L"Active API: Idle (No VR Session)");
    }

    if (hasApi && g_shared->effectiveHz > 10.0) {
        bool isSsw = false;
        double targetHz = g_shared->effectiveHz;
        double panelHz = g_shared->rawPanelHz;
        vrpacing::TelemetryAnalyzer::ResolveDisplayMode(*g_shared, isSsw, targetHz, panelHz);
        if (isSsw) {
            swprintf_s(txtHz, L"HMD Refresh: %.0f / %.0f Hz (SSW)", targetHz, panelHz);
        } else {
            swprintf_s(txtHz, L"HMD Refresh: %.0f Hz (Native)", panelHz);
        }
    } else {
        wcscpy_s(txtHz, L"HMD Refresh: Waiting...");
    }

    if (hasApi && g_shared->appliedCap > 0.0) {
        swprintf_s(txtCap, L"Active Cap: %.2f FPS", g_shared->appliedCap);
    } else {
        wcscpy_s(txtCap, L"Active Cap: Armed (-0.10 FPS)");
    }

    if (hasApi && g_shared->avgFps > 5.0) {
        swprintf_s(txtFps, L"Avg FPS: %.1f  |  1%% Low: %.1f FPS", g_shared->avgFps, g_shared->onePercentLowFps);
    } else if (hasApi) {
        swprintf_s(txtFps, L"FPS: Measuring... (Frames: %llu)", g_shared->frameCounter);
    } else {
        wcscpy_s(txtFps, L"Avg FPS: --  |  1% Low: --");
    }

    if (hasApi && g_shared->hookedProcess[0] != L'\0' && wcscmp(g_shared->hookedProcess, L"None") != 0) {
        swprintf_s(txtProc, L"Process: %s", g_shared->hookedProcess);
    } else {
        wcscpy_s(txtProc, L"Process: Monitoring for VR games...");
    }

    if (hasApi && g_shared->inputWidth > 0 && g_shared->outputWidth > 0) {
        swprintf_s(txtRes, L"Resolution: %ux%u -> %ux%u | FMT: %u",
                   g_shared->inputWidth, g_shared->inputHeight,
                   g_shared->outputWidth, g_shared->outputHeight,
                   g_shared->dxgiFormat);
    } else {
        wcscpy_s(txtRes, L"Resolution: Waiting for Swapchain...");
    }

    const wchar_t* nisStr = L"Off";
    if (g_cfg.upscaling.enabled) {
        if (!hasApi) {
            nisStr = g_smaaEnabled ? L"Standby (+SMAA)" : L"Standby";
        } else if (g_shared->upscaleStatus == 1) {
            nisStr = (g_smaaEnabled && g_shared->smaaStatus == 1) ? L"Active (2.0x+SMAA)" : L"Active (2.0x)";
        } else if (g_shared->upscaleStatus < 0) {
            nisStr = L"Error";
        } else {
            nisStr = L"Standby";
        }
    }

    const wchar_t* vrsStr = L"Off";
    if (g_cfg.ffr.enabled) {
        if (!hasApi) vrsStr = L"Standby";
        else if (g_shared->vrsStatus == 1) vrsStr = L"D3D11 NVAPI";
        else if (g_shared->vrsStatus == 2) vrsStr = L"D3D12 Tier-2";
        else if (g_shared->vrsStatus == -1) vrsStr = L"Excluded (OC)";
        else if (g_shared->vrsStatus == -2) vrsStr = L"Unsupported";
        else vrsStr = L"Standby";
    }

    const wchar_t* maskStr = L"Off";
    if (g_cfg.mask.enabled) {
        if (!hasApi) maskStr = L"Standby";
        else if (g_shared->maskStatus == 1) maskStr = L"Active";
        else if (g_shared->maskStatus == -1) maskStr = L"Excluded (OC)";
        else maskStr = L"Standby";
    }

    swprintf_s(txtPipe, L"NIS: %s  |  VRS: %s  |  Mask: %s", nisStr, vrsStr, maskStr);

    if (g_shared->screenshotSavedCount > 0) {
        swprintf_s(txtShot, L"Screenshot Hotkey: Ctrl + F12 (Saved: %u set%s in Screenshots\\)",
                   g_shared->screenshotSavedCount, (g_shared->screenshotSavedCount == 1) ? L"" : L"s");
    } else if (g_smaaEnabled) {
        wcscpy_s(txtShot, L"Comparison Screenshot: Ctrl + F12 saves 4-image suite (1x/2x + SMAA)");
    } else {
        wcscpy_s(txtShot, L"Comparison Screenshot: Press Ctrl + F12 in-game (saves 1x/2x .png pair)");
    }

    SetWindowTextIfChanged(g_hLblApi, txtApi, g_cacheApi);
    SetWindowTextIfChanged(g_hLblHz, txtHz, g_cacheHz);
    SetWindowTextIfChanged(g_hLblCap, txtCap, g_cacheCap);
    SetWindowTextIfChanged(g_hLblFps, txtFps, g_cacheFps);
    SetWindowTextIfChanged(g_hLblProcess, txtProc, g_cacheProc);
    SetWindowTextIfChanged(g_hLblRes, txtRes, g_cacheRes);
    SetWindowTextIfChanged(g_hLblPipeline, txtPipe, g_cachePipe);
    SetWindowTextIfChanged(g_hLblShotInfo, txtShot, g_cacheShot);

    if (g_hLblAdvisor) {
        const auto& rep = g_advisor.GetReport();
        SetWindowTextIfChanged(g_hLblAdvisor, rep.summaryLine, g_cacheAdvisor);
    }

    InvalidateRect(g_hMainWnd, &kGraphRect, FALSE);
    g_uiIdleRendered = !hasApi;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING | MF_DEFAULT, ID_TRAY_OPEN, L"Open VRPacing Panel");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING | (g_cfg.upscaling.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_NIS, L"Enable NIS 2.0x Spatial Upscaling");
    AppendMenuW(hMenu, MF_STRING | (g_smaaEnabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_SMAA, L"Enable Pre-Upscale SMAA 1x (for MSAA / FXAA / No AA)");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.ffr.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_FFR, L"Enable Fixed Foveated Rendering (VRS)");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.mask.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_MASK, L"Enable Pre-Render Circular Mask");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    const bool isHalf = (g_cfg.pacing.frameGenDivisor > 1.05);
    AppendMenuW(hMenu, MF_STRING | (!isHalf ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_RATE_1X, L"Rate: Automatic (Native Hz && Auto SSW)");
    AppendMenuW(hMenu, MF_STRING | (isHalf ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_RATE_HALF, L"Rate: Half Refresh Rate (Manual Hz / 2)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TAKE_SHOT, L"Take Comparison Screenshot (Ctrl+F12)");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_SHOTS, L"Open Screenshots Folder");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_LOG, L"Open Log File");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

void CreateUiElements(HWND hwnd, HINSTANCE hInst) {
    g_hFontNormal = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_hFontBold = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_hFontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    g_hBrushGraphBg   = CreateSolidBrush(RGB(22, 26, 32));
    g_hPenGraphBorder = CreatePen(PS_SOLID, 1, RGB(65, 72, 84));
    g_hPenGraphGrid   = CreatePen(PS_SOLID, 1, RGB(40, 46, 56));
    g_hPenGraphTarget = CreatePen(PS_SOLID, 1, RGB(50, 185, 85));
    g_hPenGraphLine   = CreatePen(PS_SOLID, 1, RGB(80, 205, 255));

    auto SetFont = [](HWND hCtrl, HFONT hFont) {
        SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    };

    // 1. Telemetry & Real-Time Diagnostics Group
    HWND hGrpStatus = CreateWindowExW(0, L"BUTTON", L" Telemetry && Real-Time Diagnostics ",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      15, 8, 434, 248, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpStatus, g_hFontBold);

    g_hLblApi = CreateWindowExW(0, L"STATIC", L"Active API: Idle (No VR Session)",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 28, 195, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblApi, g_hFontNormal);

    g_hLblHz = CreateWindowExW(0, L"STATIC", L"HMD Refresh: Waiting...",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               230, 28, 204, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblHz, g_hFontNormal);

    g_hLblCap = CreateWindowExW(0, L"STATIC", L"Active Cap: Armed (-0.10 FPS)",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 50, 195, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblCap, g_hFontNormal);

    g_hLblFps = CreateWindowExW(0, L"STATIC", L"Avg FPS: --  |  1% Low: --",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                230, 50, 204, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblFps, g_hFontNormal);

    g_hLblProcess = CreateWindowExW(0, L"STATIC", L"Process: Monitoring for VR games...",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    30, 72, 310, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblProcess, g_hFontNormal);

    HWND hBtnReset = CreateWindowExW(0, L"BUTTON", L"Reset Stats",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     350, 69, 84, 22, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_BTN_RESET_STATS)), hInst, nullptr);
    SetFont(hBtnReset, g_hFontSmall);

    g_hLblRes = CreateWindowExW(0, L"STATIC", L"Resolution: Waiting for Swapchain...",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 94, 404, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblRes, g_hFontNormal);

    g_hLblPipeline = CreateWindowExW(0, L"STATIC", L"NIS: Standby  |  VRS: Standby  |  Mask: Standby",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     30, 118, 404, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblPipeline, g_hFontBold);

    g_hLblAdvisor = CreateWindowExW(0, L"STATIC", L"Advisor: Waiting for VR session...",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    30, 142, 404, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblAdvisor, g_hFontBold);

    // 2. Render Pipeline Optimizations Group
    HWND hGrpOpt = CreateWindowExW(0, L"BUTTON", L" Render Pipeline Optimizations ",
                                   WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   15, 264, 434, 152, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpOpt, g_hFontBold);

    g_hChkNis = CreateWindowExW(0, L"BUTTON", L"Enable NIS 2.0x Spatial Upscaling (60% Radius, 0% Sharpness)",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                30, 286, 404, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_CHECK_NIS)), hInst, nullptr);
    SetFont(g_hChkNis, g_hFontNormal);

    g_hChkSmaa = CreateWindowExW(0, L"BUTTON", L"Enable Pre-Upscale SMAA 1x (Anti-Aliasing before NIS 2.0x)",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 30, 311, 404, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_CHECK_SMAA)), hInst, nullptr);
    SetFont(g_hChkSmaa, g_hFontNormal);

    g_hChkFfr = CreateWindowExW(0, L"BUTTON", L"Enable Fixed Foveated Rendering (VRS - D3D11 && D3D12)",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                30, 336, 404, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_CHECK_FFR)), hInst, nullptr);
    SetFont(g_hChkFfr, g_hFontNormal);

    g_hChkMask = CreateWindowExW(0, L"BUTTON", L"Enable Pre-Render Circular Visibility Mask",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 30, 361, 404, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_CHECK_MASK)), hInst, nullptr);
    SetFont(g_hChkMask, g_hFontNormal);

    g_hLblShotInfo = CreateWindowExW(0, L"STATIC", L"Comparison Screenshot: Press Ctrl + F12 in-game (saves 1x/2x .png pair)",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     30, 388, 404, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblShotInfo, g_hFontSmall);

    // 3. Frame Pacing & Target Rate Mode Group
    HWND hGrpPacing = CreateWindowExW(0, L"BUTTON", L" Frame Pacing && Target Rate Mode ",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      15, 424, 434, 114, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpPacing, g_hFontBold);

    HWND hLblPacingStatus = CreateWindowExW(0, L"STATIC", L"Pacing is Always Active (-0.10 FPS offset below refresh target).",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            30, 446, 404, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(hLblPacingStatus, g_hFontNormal);

    g_hRadio1x = CreateWindowExW(0, L"BUTTON", L"Automatic (Recommended - Native Hz && Auto SSW detection)",
                                 WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                 36, 470, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_RADIO_1X)), hInst, nullptr);
    SetFont(g_hRadio1x, g_hFontNormal);

    g_hRadioHalf = CreateWindowExW(0, L"BUTTON", L"Half Refresh Rate (Manual Hz / 2 lock - Force half-rate cap)",
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   36, 496, 400, 20, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_RADIO_HALF)), hInst, nullptr);
    SetFont(g_hRadioHalf, g_hFontNormal);

    // 4. Bottom Action Buttons
    HWND hBtnMinimize = CreateWindowExW(0, L"BUTTON", L"Minimize",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        15, 550, 101, 32, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_BTN_MINIMIZE)), hInst, nullptr);
    SetFont(hBtnMinimize, g_hFontBold);

    HWND hBtnShots = CreateWindowExW(0, L"BUTTON", L"Screenshots",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     126, 550, 101, 32, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_BTN_OPEN_SHOTS)), hInst, nullptr);
    SetFont(hBtnShots, g_hFontBold);

    HWND hBtnLog = CreateWindowExW(0, L"BUTTON", L"Open Log",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   237, 550, 101, 32, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_BTN_OPEN_LOG)), hInst, nullptr);
    SetFont(hBtnLog, g_hFontBold);

    HWND hBtnExit = CreateWindowExW(0, L"BUTTON", L"Exit",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    348, 550, 101, 32, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ID_BTN_EXIT)), hInst, nullptr);
    SetFont(hBtnExit, g_hFontBold);

    // 5. Native Win32 Floating Tooltips
    g_hTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                 WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 hwnd, nullptr, hInst, nullptr);
    if (g_hTooltip) {
        SetWindowPos(g_hTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(g_hTooltip, TTM_SETMAXTIPWIDTH, 0, 390);
        SendMessageW(g_hTooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 200);
        SendMessageW(g_hTooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 18000);

        AttachControlTooltip(hwnd, g_hChkSmaa,
            L"Pre-Upscale SMAA 1x (Subpixel Morphological Anti-Aliasing):\r\n"
            L"• Recommended when the game uses MSAA, FXAA, or No Anti-Aliasing. Smooths high-contrast edges, foliage, and specular aliasing at 1.0x before NIS 2.0x reconstruction.\r\n"
            L"• Not recommended in combination with TAA, DLSS, FSR 2+, TSR, or temporal upscalers in general.");

        AttachControlTooltip(hwnd, g_hChkNis,
            L"Reconstructs the central 60% foveal cone at an exact 2.0x integer ratio with 0% artificial sharpening prior to VR compositor barrel distortion.");
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            CreateUiElements(hwnd, cs->hInstance);
            SyncUiWithConfig();
            RegisterOpenXrImplicitLayer();
            RegisterHotKey(hwnd, ID_HOTKEY_SCREENSHOT, MOD_CONTROL | MOD_NOREPEAT, VK_F12);
            SetTimer(hwnd, IDT_SCANNER_TIMER, 2000, nullptr);
            SetTimer(hwnd, IDT_TELEMETRY_TIMER, 250, nullptr);
            return 0;
        }

        case WM_HOTKEY:
            if (wParam == ID_HOTKEY_SCREENSHOT) {
                TriggerComparisonScreenshot();
            }
            return 0;

        case WM_TIMER:
            if (wParam == IDT_TELEMETRY_TIMER) {
                if (g_shared && g_shared->activePid != 0) {
                    const uint32_t prevShots = g_shared->screenshotSavedCount;
                    g_advisor.Ingest(*g_shared);
                    g_shared->avgFps = g_advisor.GetAvgFps();
                    g_shared->onePercentLowFps = g_advisor.GetOnePercentLowFps();
                    (void)prevShots;
                    UpdateTelemetryDisplay(true);
                } else {
                    UpdateTelemetryDisplay(false);
                }
            } else if (wParam == IDT_SCANNER_TIMER) {
                CheckActiveProcessLiveness();
                if (!g_shared || g_shared->activePid == 0) {
                    g_scanner.PollAndInject(g_hookPath);
                }
                UpdateTelemetryDisplay(false);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawFrametimeGraph(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            auto hdcStatic = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdcStatic, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }

        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            switch (id) {
                case ID_CHECK_NIS:
                    g_cfg.upscaling.enabled = (SendMessageW(g_hChkNis, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_CHECK_SMAA:
                    g_smaaEnabled = (SendMessageW(g_hChkSmaa, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_CHECK_FFR: {
                    const bool checked = (SendMessageW(g_hChkFfr, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_cfg.ffr.enabled = checked;
                    g_cfg.ffr.openxrEnabled = checked;
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;
                }

                case ID_CHECK_MASK:
                    g_cfg.mask.enabled = (SendMessageW(g_hChkMask, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_RADIO_1X:
                case ID_TRAY_RATE_1X:
                    g_cfg.pacing.frameGenDivisor = 1.0;
                    SaveAndPropagate();
                    break;

                case ID_RADIO_HALF:
                case ID_TRAY_RATE_HALF:
                    g_cfg.pacing.frameGenDivisor = 2.0;
                    SaveAndPropagate();
                    break;

                case ID_BTN_RESET_STATS:
                    g_advisor.Reset();
                    if (g_shared) {
                        g_shared->avgFps = 0.0;
                        g_shared->onePercentLowFps = 0.0;
                    }
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_BTN_MINIMIZE:
                    ShowWindow(hwnd, SW_HIDE);
                    UpdateSharedStateFlags();
                    break;

                case ID_TRAY_TAKE_SHOT:
                    TriggerComparisonScreenshot();
                    break;

                case ID_BTN_OPEN_SHOTS:
                case ID_TRAY_OPEN_SHOTS:
                    OpenScreenshotsFolder();
                    break;

                case ID_BTN_OPEN_LOG:
                case ID_TRAY_OPEN_LOG: {
                    const std::wstring logPath = vrpacing::GetLogFilePath();
                    ShellExecuteW(nullptr, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOW);
                    break;
                }

                case ID_BTN_EXIT:
                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;

                case ID_TRAY_OPEN:
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    UpdateSharedStateFlags();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_TRAY_NIS:
                    g_cfg.upscaling.enabled = !g_cfg.upscaling.enabled;
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_TRAY_SMAA:
                    g_smaaEnabled = !g_smaaEnabled;
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_TRAY_FFR:
                    g_cfg.ffr.enabled = !g_cfg.ffr.enabled;
                    g_cfg.ffr.openxrEnabled = g_cfg.ffr.enabled;
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;

                case ID_TRAY_MASK:
                    g_cfg.mask.enabled = !g_cfg.mask.enabled;
                    SaveAndPropagate();
                    UpdateTelemetryDisplay(true);
                    break;
            }
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                UpdateSharedStateFlags();
                UpdateTelemetryDisplay(true);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            UpdateSharedStateFlags();
            return 0;

        case WM_DESTROY:
            UnregisterHotKey(hwnd, ID_HOTKEY_SCREENSHOT);
            KillTimer(hwnd, IDT_SCANNER_TIMER);
            KillTimer(hwnd, IDT_TELEMETRY_TIMER);
            UnregisterOpenXrImplicitLayer();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            if (g_hFontSmall) DeleteObject(g_hFontSmall);
            if (g_hBrushGraphBg) DeleteObject(g_hBrushGraphBg);
            if (g_hPenGraphBorder) DeleteObject(g_hPenGraphBorder);
            if (g_hPenGraphGrid) DeleteObject(g_hPenGraphGrid);
            if (g_hPenGraphTarget) DeleteObject(g_hPenGraphTarget);
            if (g_hPenGraphLine) DeleteObject(g_hPenGraphLine);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (!IsCurrentProcessElevated()) {
        wchar_t exePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
            ShellExecuteW(nullptr, L"runas", exePath, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }

    HANDLE hSingleMutex = CreateMutexW(nullptr, TRUE, L"Local\\VRPacingSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExisting = FindWindowW(L"VRPacingMainWindowClass", nullptr);
        if (hExisting) {
            ShowWindow(hExisting, SW_SHOW);
            SetForegroundWindow(hExisting);
        }
        if (hSingleMutex) CloseHandle(hSingleMutex);
        return 0;
    }

    INITCOMMONCONTROLSEX icex{};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    vrpacing::InitLogging();
    vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingTray starting...");

    g_cfg = vrpacing::LoadConfig();
    g_smaaEnabled = false; // Always unchecked on startup
    g_cfg.upscaling.scale = 2.0f;
    g_cfg.upscaling.radius = 0.60f;
    g_cfg.upscaling.sharpness = 0.0f;
    g_cfg.mask.radiusX = 1.00f;
    g_cfg.mask.radiusY = 1.00f;
    g_cfg.mask.segments = 256;
    g_cfg.ffr.innerRadius = 0.60f;
    g_cfg.ffr.openxrEnabled = g_cfg.ffr.enabled;
    g_cfg.pacing.enabled = true;
    g_cfg.pacing.offsetFps = 0.10;
    if (g_cfg.pacing.frameGenDivisor > 1.05) {
        g_cfg.pacing.frameGenDivisor = 2.0;
    } else {
        g_cfg.pacing.frameGenDivisor = 1.0;
    }
    vrpacing::SaveConfig(g_cfg);

    g_hookPath = ExeDir() + L"\\VRPacingHook.dll";
    g_screenshotsDir = ExeDir() + L"\\Screenshots";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"VRPacingMainWindowClass";
    RegisterClassExW(&wc);

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int winW = 480;
    const int winH = 632;
    const int posX = (screenW - winW) / 2;
    const int posY = (screenH - winH) / 2;

    std::wstring winTitle = L"VRPacing ";
    winTitle += vrpacing::kAppVersionString;

    g_hMainWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        winTitle.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, winW, winH,
        nullptr, nullptr, hInst, nullptr
    );

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    swprintf_s(g_nid.szTip, L"VRPacing %s [Idle - Monitoring]", vrpacing::kAppVersionString);
    wcscpy_s(g_lastTrayTip, g_nid.szTip);
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_mapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vrpacing::SharedState), vrpacing::kSharedStateName);
    void* view = MapViewOfFile(g_mapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrpacing::SharedState));
    g_shared = reinterpret_cast<vrpacing::SharedState*>(view);
    if (g_shared) {
        *g_shared = {};
        g_shared->version = vrpacing::kSharedStateVersion;
        g_shared->activePid = 0;
        g_shared->openXrD3D11Pid = 0;
        wcscpy_s(g_shared->activeApi, L"Idle");
        wcsncpy_s(g_shared->screenshotDir, g_screenshotsDir.c_str(), _TRUNCATE);
    }

    ShowWindow(g_hMainWnd, SW_SHOW);
    UpdateWindow(g_hMainWnd);
    UpdateSharedStateFlags();
    UpdateTelemetryDisplay(true);

    // Run one initial scan on startup so pre-existing processes are classified immediately
    g_scanner.PollAndInject(g_hookPath);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (view) UnmapViewOfFile(view);
    if (g_mapFile) CloseHandle(g_mapFile);
    if (hSingleMutex) CloseHandle(hSingleMutex);

    return 0;
}

#endif