#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>

#include <cmath>
#include <cwchar>
#include <filesystem>
#include <string>

#include "common/config_json.h"
#include "common/logging_win.h"
#include "injector/process_scanner.h"
#include "vrpacing/config.h"
#include "vrpacing/shared_state.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR IDT_SCANNER_TIMER = 101;

// UI Control IDs
constexpr UINT ID_CHECK_NIS = 2001;
constexpr UINT ID_CHECK_PACING = 2002;
constexpr UINT ID_RADIO_1X = 2003;
constexpr UINT ID_RADIO_HALF = 2004;
constexpr UINT ID_RADIO_THIRD = 2005;
constexpr UINT ID_BTN_MINIMIZE = 2006;
constexpr UINT ID_BTN_OPEN_LOG = 2007;
constexpr UINT ID_BTN_EXIT = 2008;

// Tray Menu IDs
constexpr UINT ID_TRAY_OPEN = 3001;
constexpr UINT ID_TRAY_NIS = 3002;
constexpr UINT ID_TRAY_PACING = 3003;
constexpr UINT ID_TRAY_RATE_1X = 3004;
constexpr UINT ID_TRAY_RATE_HALF = 3005;
constexpr UINT ID_TRAY_RATE_THIRD = 3006;
constexpr UINT ID_TRAY_OPEN_LOG = 3007;
constexpr UINT ID_TRAY_EXIT = 3008;

NOTIFYICONDATAW g_nid{};
vrpacing::RuntimeConfig g_cfg;
vrpacing::SharedState* g_shared = nullptr;
HANDLE g_mapFile = nullptr;
vrpacing::ProcessScanner g_scanner;
std::wstring g_hookPath;

HWND g_hMainWnd = nullptr;
HFONT g_hFontNormal = nullptr;
HFONT g_hFontBold = nullptr;

HWND g_hLblApi = nullptr;
HWND g_hLblHz = nullptr;
HWND g_hLblCap = nullptr;
HWND g_hLblFrames = nullptr;
HWND g_hLblProcess = nullptr;
HWND g_hLblRes = nullptr;
HWND g_hLblPipeline = nullptr;

HWND g_hChkNis = nullptr;
HWND g_hChkPacing = nullptr;
HWND g_hRadio1x = nullptr;
HWND g_hRadioHalf = nullptr;
HWND g_hRadioThird = nullptr;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

void RegisterOpenXrImplicitLayer() {
    const std::wstring jsonPath = ExeDir() + L"\\VRPacingLayer.json";
    if (!std::filesystem::exists(jsonPath)) {
        return;
    }

    HKEY hKey = nullptr;
    const wchar_t* subKey = L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (status == ERROR_SUCCESS) {
        DWORD val = 0;
        RegSetValueExW(hKey, jsonPath.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        RegCloseKey(hKey);
        vrpacing::LogMessage(vrpacing::LogLevel::Info, "OpenXR implicit layer registrada en HKCU.");
    }
}

void UpdateSharedStateFlags() {
    if (!g_shared) return;
    uint32_t flags = 0;
    if (g_cfg.enabled) flags |= vrpacing::kFlagEnabled;
    if (g_cfg.upscaling.enabled) flags |= vrpacing::kFlagUpscalingEnabled;
    if (g_cfg.mask.enabled) flags |= vrpacing::kFlagMaskEnabled;
    if (g_cfg.pacing.enabled) flags |= vrpacing::kFlagPacingEnabled;

    g_shared->flags = flags;
    g_shared->frameGenDivisor = (g_cfg.pacing.frameGenDivisor > 0.0) ? g_cfg.pacing.frameGenDivisor : 1.0;
    g_shared->offsetFps = g_cfg.pacing.offsetFps;
}

void SyncUiWithConfig() {
    if (g_hChkNis) {
        SendMessageW(g_hChkNis, BM_SETCHECK, g_cfg.upscaling.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (g_hChkPacing) {
        SendMessageW(g_hChkPacing, BM_SETCHECK, g_cfg.pacing.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    const double divisor = g_cfg.pacing.frameGenDivisor;
    const bool is1x = (divisor <= 1.05);
    const bool isHalf = (std::abs(divisor - 2.0) < 0.1);
    const bool isThird = (std::abs(divisor - 3.0) < 0.1);

    if (g_hRadio1x) SendMessageW(g_hRadio1x, BM_SETCHECK, is1x ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hRadioHalf) SendMessageW(g_hRadioHalf, BM_SETCHECK, isHalf ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_hRadioThird) SendMessageW(g_hRadioThird, BM_SETCHECK, isThird ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SaveAndPropagate() {
    g_cfg.upscaling.scale = 2.0f;
    g_cfg.upscaling.radius = 0.60f;
    g_cfg.upscaling.sharpness = 0.0f;
    g_cfg.mask.enabled = true;
    g_cfg.pacing.offsetFps = 0.10;

    vrpacing::SaveConfig(g_cfg);
    UpdateSharedStateFlags();
    SyncUiWithConfig();
}

void UpdateTelemetryDisplay() {
    wchar_t txtApi[64]{};
    wchar_t txtHz[64]{};
    wchar_t txtCap[64]{};
    wchar_t txtFrames[64]{};
    wchar_t txtProc[96]{};
    wchar_t txtRes[96]{};
    wchar_t txtPipe[128]{};

    if (g_shared) {
        // Watchdog de proceso VR: si el proceso termino, limpiar estado a Idle
        if (g_shared->activePid != 0) {
            HANDLE hProc = OpenProcess(SYNCHRONIZE, FALSE, g_shared->activePid);
            if (hProc) {
                DWORD waitRes = WaitForSingleObject(hProc, 0);
                CloseHandle(hProc);
                if (waitRes == WAIT_OBJECT_0) {
                    g_shared->activePid = 0;
                    wcscpy_s(g_shared->activeApi, L"Idle");
                    wcscpy_s(g_shared->hookedProcess, L"None");
                    g_shared->inputWidth = 0;
                    g_shared->inputHeight = 0;
                    g_shared->outputWidth = 0;
                    g_shared->outputHeight = 0;
                    g_shared->dxgiFormat = 0;
                    g_shared->upscaleStatus = 0;
                    g_shared->lastHResult = 0;
                    g_shared->appliedCap = 0.0;
                    vrpacing::LogMessage(vrpacing::LogLevel::Info, "Juego VR cerrado. Telemetria e IPC restablecidos a Idle.");
                }
            } else {
                if (GetLastError() == ERROR_INVALID_PARAMETER) {
                    g_shared->activePid = 0;
                    wcscpy_s(g_shared->activeApi, L"Idle");
                    wcscpy_s(g_shared->hookedProcess, L"None");
                    g_shared->inputWidth = 0;
                    g_shared->inputHeight = 0;
                    g_shared->outputWidth = 0;
                    g_shared->outputHeight = 0;
                    g_shared->dxgiFormat = 0;
                    g_shared->upscaleStatus = 0;
                    g_shared->lastHResult = 0;
                    g_shared->appliedCap = 0.0;
                }
            }
        }

        const bool hasApi = (g_shared->activeApi[0] != L'\0' &&
                             wcscmp(g_shared->activeApi, L"Idle") != 0 &&
                             wcscmp(g_shared->activeApi, L"none") != 0);

        if (hasApi) {
            swprintf_s(txtApi, L"Active API: %s", g_shared->activeApi);
        } else {
            wcscpy_s(txtApi, L"Active API: Idle (No VR Session)");
        }

        if (g_shared->effectiveHz > 10.0) {
            swprintf_s(txtHz, L"HMD Refresh: %.1f Hz", g_shared->effectiveHz);
        } else {
            wcscpy_s(txtHz, L"HMD Refresh: Detecting...");
        }

        if (g_cfg.pacing.enabled && g_shared->appliedCap > 0.0) {
            swprintf_s(txtCap, L"Active Cap: %.2f FPS", g_shared->appliedCap);
        } else if (g_cfg.pacing.enabled) {
            wcscpy_s(txtCap, L"Active Cap: Armed (-0.10 FPS)");
        } else {
            wcscpy_s(txtCap, L"Active Cap: Disabled");
        }

        swprintf_s(txtFrames, L"Rendered Frames: %llu", g_shared->frameCounter);

        if (g_shared->hookedProcess[0] != L'\0' && wcscmp(g_shared->hookedProcess, L"None") != 0) {
            swprintf_s(txtProc, L"Process: %s", g_shared->hookedProcess);
        } else {
            wcscpy_s(txtProc, L"Process: Monitoring for VR games...");
        }

        if (g_shared->inputWidth > 0 && g_shared->outputWidth > 0) {
            swprintf_s(txtRes, L"Resolution: %ux%u -> %ux%u | FMT: %u",
                       g_shared->inputWidth, g_shared->inputHeight,
                       g_shared->outputWidth, g_shared->outputHeight,
                       g_shared->dxgiFormat);
        } else {
            wcscpy_s(txtRes, L"Resolution: Waiting for Swapchain...");
        }

        if (g_shared->upscaleStatus == 1) {
            wcscpy_s(txtPipe, L"NIS Pipeline: ACTIVE OK (2.0x Scaled + Masked)");
        } else if (g_shared->upscaleStatus == -1) {
            swprintf_s(txtPipe, L"NIS Pipeline: Swapchain Create Error [HR: 0x%08X]", g_shared->lastHResult);
        } else if (g_shared->upscaleStatus == -2) {
            wcscpy_s(txtPipe, L"NIS Pipeline: Shader Initialization Error");
        } else if (g_shared->upscaleStatus == -3) {
            swprintf_s(txtPipe, L"NIS Pipeline: Dispatch/UAV Error [HR: 0x%08X]", g_shared->lastHResult);
        } else {
            wcscpy_s(txtPipe, L"NIS Pipeline: Idle");
        }

        wchar_t tip[128]{};
        if (hasApi && g_shared->appliedCap > 0.0) {
            swprintf_s(tip, L"VRPacing [%s] | %.2f FPS (%.1f Hz)",
                       g_shared->activeApi, g_shared->appliedCap, g_shared->effectiveHz);
        } else if (hasApi) {
            swprintf_s(tip, L"VRPacing [%s] | Active", g_shared->activeApi);
        } else {
            wcscpy_s(tip, L"VRPacing [Idle - Monitoring]");
        }
        wcscpy_s(g_nid.szTip, tip);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    } else {
        wcscpy_s(txtApi, L"Active API: IPC Unavailable");
        wcscpy_s(txtHz, L"HMD Refresh: -- Hz");
        wcscpy_s(txtCap, L"Active Cap: -- FPS");
        wcscpy_s(txtFrames, L"Rendered Frames: 0");
        wcscpy_s(txtProc, L"Process: Shared memory offline");
        wcscpy_s(txtRes, L"Resolution: --");
        wcscpy_s(txtPipe, L"NIS Pipeline: Offline");
    }

    if (g_hLblApi) SetWindowTextW(g_hLblApi, txtApi);
    if (g_hLblHz) SetWindowTextW(g_hLblHz, txtHz);
    if (g_hLblCap) SetWindowTextW(g_hLblCap, txtCap);
    if (g_hLblFrames) SetWindowTextW(g_hLblFrames, txtFrames);
    if (g_hLblProcess) SetWindowTextW(g_hLblProcess, txtProc);
    if (g_hLblRes) SetWindowTextW(g_hLblRes, txtRes);
    if (g_hLblPipeline) SetWindowTextW(g_hLblPipeline, txtPipe);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING | MF_DEFAULT, ID_TRAY_OPEN, L"Open VRPacing Panel");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING | (g_cfg.upscaling.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_NIS, L"NIS 2.0x Upscaling");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.pacing.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_PACING, L"Frame Pacing (-0.10 FPS)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    const double divisor = g_cfg.pacing.frameGenDivisor;
    const bool is1x = (divisor <= 1.05);
    const bool isHalf = (std::abs(divisor - 2.0) < 0.1);
    const bool isThird = (std::abs(divisor - 3.0) < 0.1);

    AppendMenuW(hMenu, MF_STRING | (is1x ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_RATE_1X, L"Rate: 1x Native (Panel Hz)");
    AppendMenuW(hMenu, MF_STRING | (isHalf ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_RATE_HALF, L"Rate: 1/2x Fractional (VectorXR)");
    AppendMenuW(hMenu, MF_STRING | (isThird ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_RATE_THIRD, L"Rate: 1/3x Fractional (VectorXR)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
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

    auto SetFont = [](HWND hCtrl, HFONT hFont) {
        SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    };

    HWND hGrpStatus = CreateWindowExW(0, L"BUTTON", L" Telemetry & Lab Diagnostics ",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      15, 10, 434, 142, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpStatus, g_hFontBold);

    g_hLblApi = CreateWindowExW(0, L"STATIC", L"Active API: Detecting...",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 30, 195, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblApi, g_hFontNormal);

    g_hLblHz = CreateWindowExW(0, L"STATIC", L"HMD Refresh: -- Hz",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               230, 30, 200, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblHz, g_hFontNormal);

    g_hLblCap = CreateWindowExW(0, L"STATIC", L"Active Cap: -- FPS",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 50, 195, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblCap, g_hFontNormal);

    g_hLblFrames = CreateWindowExW(0, L"STATIC", L"Rendered Frames: 0",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   230, 50, 200, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblFrames, g_hFontNormal);

    g_hLblProcess = CreateWindowExW(0, L"STATIC", L"Process: Monitoring...",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    30, 70, 400, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblProcess, g_hFontNormal);

    g_hLblRes = CreateWindowExW(0, L"STATIC", L"Resolution: Waiting for Swapchain...",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                30, 90, 400, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblRes, g_hFontNormal);

    g_hLblPipeline = CreateWindowExW(0, L"STATIC", L"NIS Pipeline: Idle",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     30, 112, 400, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(g_hLblPipeline, g_hFontBold);

    HWND hGrpUpscale = CreateWindowExW(0, L"BUTTON", L" Spatial Upscaling (NVIDIA Image Scaling) ",
                                       WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       15, 160, 434, 75, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpUpscale, g_hFontBold);

    g_hChkNis = CreateWindowExW(0, L"BUTTON", L"Enable NIS 2.0x Upscaling",
                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                30, 182, 400, 20, hwnd, reinterpret_cast<HMENU>(ID_CHECK_NIS), hInst, nullptr);
    SetFont(g_hChkNis, g_hFontNormal);

    HWND hLblFixed = CreateWindowExW(0, L"STATIC", L"Locked Profile: Scale 2.0x  |  Radius 0.60  |  Sharpness 0%",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     30, 206, 400, 18, hwnd, nullptr, hInst, nullptr);
    SetFont(hLblFixed, g_hFontNormal);

    HWND hGrpPacing = CreateWindowExW(0, L"BUTTON", L" Deterministic Frame Pacing ",
                                      WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                      15, 245, 434, 215, hwnd, nullptr, hInst, nullptr);
    SetFont(hGrpPacing, g_hFontBold);

    g_hChkPacing = CreateWindowExW(0, L"BUTTON", L"Enable Frame Pacing (-0.10 FPS Low-Latency Offset)",
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   30, 267, 400, 20, hwnd, reinterpret_cast<HMENU>(ID_CHECK_PACING), hInst, nullptr);
    SetFont(g_hChkPacing, g_hFontNormal);

    HWND hLblRate = CreateWindowExW(0, L"STATIC", L"Target Rate Mode:",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    30, 292, 400, 16, hwnd, nullptr, hInst, nullptr);
    SetFont(hLblRate, g_hFontBold);

    g_hRadio1x = CreateWindowExW(0, L"BUTTON", L"1x - Native Panel Refresh Rate (Default, e.g. 89.90 FPS @ 90Hz)",
                                 WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                                 42, 310, 390, 20, hwnd, reinterpret_cast<HMENU>(ID_RADIO_1X), hInst, nullptr);
    SetFont(g_hRadio1x, g_hFontNormal);

    HWND hLblFrac = CreateWindowExW(0, L"STATIC", L"Fractional FPS Caps (For External FrameGen Mods):",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    42, 335, 390, 16, hwnd, nullptr, hInst, nullptr);
    SetFont(hLblFrac, g_hFontBold);

    g_hRadioHalf = CreateWindowExW(0, L"BUTTON", L"1/2x - Half Refresh Rate (e.g. 44.90 FPS @ 90Hz)",
                                   WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                   42, 354, 390, 20, hwnd, reinterpret_cast<HMENU>(ID_RADIO_HALF), hInst, nullptr);
    SetFont(g_hRadioHalf, g_hFontNormal);

    g_hRadioThird = CreateWindowExW(0, L"BUTTON", L"1/3x - Third Refresh Rate (e.g. 29.90 FPS @ 90Hz)",
                                    WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                                    42, 376, 390, 20, hwnd, reinterpret_cast<HMENU>(ID_RADIO_THIRD), hInst, nullptr);
    SetFont(g_hRadioThird, g_hFontNormal);

    HWND hLblNote = CreateWindowExW(0, L"STATIC",
                                    L"Note: Fractional caps are designed for motion interpolation mods like VectorXR to maintain flat frametimes without SSW. Do NOT enable SSW/ASW when using 1/2x or 1/3x modes.",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    30, 402, 404, 48, hwnd, nullptr, hInst, nullptr);
    SetFont(hLblNote, g_hFontNormal);

    HWND hBtnMinimize = CreateWindowExW(0, L"BUTTON", L"Minimize to Tray",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        15, 470, 138, 32, hwnd, reinterpret_cast<HMENU>(ID_BTN_MINIMIZE), hInst, nullptr);
    SetFont(hBtnMinimize, g_hFontBold);

    HWND hBtnLog = CreateWindowExW(0, L"BUTTON", L"Open Log",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   163, 470, 138, 32, hwnd, reinterpret_cast<HMENU>(ID_BTN_OPEN_LOG), hInst, nullptr);
    SetFont(hBtnLog, g_hFontBold);

    HWND hBtnExit = CreateWindowExW(0, L"BUTTON", L"Exit Application",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    311, 470, 138, 32, hwnd, reinterpret_cast<HMENU>(ID_BTN_EXIT), hInst, nullptr);
    SetFont(hBtnExit, g_hFontBold);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            CreateUiElements(hwnd, cs->hInstance);
            SyncUiWithConfig();
            RegisterOpenXrImplicitLayer();
            SetTimer(hwnd, IDT_SCANNER_TIMER, 1500, nullptr);
            return 0;
        }

        case WM_TIMER:
            if (wParam == IDT_SCANNER_TIMER) {
                g_scanner.PollAndInject(g_hookPath);
                UpdateTelemetryDisplay();
            }
            return 0;

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
                    break;

                case ID_CHECK_PACING:
                    g_cfg.pacing.enabled = (SendMessageW(g_hChkPacing, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SaveAndPropagate();
                    break;

                case ID_RADIO_1X:
                    g_cfg.pacing.frameGenDivisor = 1.0;
                    SaveAndPropagate();
                    break;

                case ID_RADIO_HALF:
                    g_cfg.pacing.frameGenDivisor = 2.0;
                    SaveAndPropagate();
                    break;

                case ID_RADIO_THIRD:
                    g_cfg.pacing.frameGenDivisor = 3.0;
                    SaveAndPropagate();
                    break;

                case ID_BTN_MINIMIZE:
                    ShowWindow(hwnd, SW_HIDE);
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
                    break;

                case ID_TRAY_NIS:
                    g_cfg.upscaling.enabled = !g_cfg.upscaling.enabled;
                    SaveAndPropagate();
                    break;

                case ID_TRAY_PACING:
                    g_cfg.pacing.enabled = !g_cfg.pacing.enabled;
                    SaveAndPropagate();
                    break;

                case ID_TRAY_RATE_1X:
                    g_cfg.pacing.frameGenDivisor = 1.0;
                    SaveAndPropagate();
                    break;

                case ID_TRAY_RATE_HALF:
                    g_cfg.pacing.frameGenDivisor = 2.0;
                    SaveAndPropagate();
                    break;

                case ID_TRAY_RATE_THIRD:
                    g_cfg.pacing.frameGenDivisor = 3.0;
                    SaveAndPropagate();
                    break;
            }
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, IDT_SCANNER_TIMER);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold) DeleteObject(g_hFontBold);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
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

    vrpacing::InitLogging();
    vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingTray starting...");

    g_cfg = vrpacing::LoadConfig();
    g_cfg.upscaling.scale = 2.0f;
    g_cfg.upscaling.radius = 0.60f;
    g_cfg.upscaling.sharpness = 0.0f;
    g_cfg.mask.enabled = true;
    g_cfg.pacing.offsetFps = 0.10;
    if (g_cfg.pacing.frameGenDivisor <= 0.0) {
        g_cfg.pacing.frameGenDivisor = 1.0;
    }
    vrpacing::SaveConfig(g_cfg);

    g_hookPath = ExeDir() + L"\\VRPacingHook.dll";

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
    const int winH = 560;
    const int posX = (screenW - winW) / 2;
    const int posY = (screenH - winH) / 2;

    g_hMainWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"VRPacing",
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
    wcscpy_s(g_nid.szTip, L"VRPacing [Starting...]");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_mapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vrpacing::SharedState), vrpacing::kSharedStateName);
    void* view = MapViewOfFile(g_mapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrpacing::SharedState));
    g_shared = reinterpret_cast<vrpacing::SharedState*>(view);
    if (g_shared) {
        *g_shared = {};
        g_shared->version = 4;
        g_shared->activePid = 0;
        UpdateSharedStateFlags();
        wcscpy_s(g_shared->activeApi, L"Idle");
    }

    ShowWindow(g_hMainWnd, SW_SHOW);
    UpdateWindow(g_hMainWnd);

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