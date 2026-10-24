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
#include <string>

#include "common/config_json.h"
#include "common/logging_win.h"
#include "injector/process_scanner.h"
#include "vrpacing/config.h"
#include "vrpacing/shared_state.h"

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR IDT_SCANNER_TIMER = 101;

constexpr UINT ID_TRAY_EXIT = 1001;
constexpr UINT ID_TRAY_RELOAD_CONFIG = 1002;
constexpr UINT ID_TRAY_OPEN_CONFIG = 1003;

constexpr UINT ID_TOGGLE_MASTER = 1101;
constexpr UINT ID_TOGGLE_PACING = 1102;
constexpr UINT ID_TOGGLE_UPSCALING = 1103;
constexpr UINT ID_TOGGLE_MASK = 1104;

constexpr UINT ID_SCALE_100 = 1201;
constexpr UINT ID_SCALE_133 = 1202;
constexpr UINT ID_SCALE_150 = 1203;
constexpr UINT ID_SCALE_177 = 1204;
constexpr UINT ID_SCALE_200 = 1205;

constexpr UINT ID_SHARP_00 = 1301;
constexpr UINT ID_SHARP_15 = 1302;
constexpr UINT ID_SHARP_30 = 1303;
constexpr UINT ID_SHARP_50 = 1304;

constexpr UINT ID_HZ_AUTO = 1401;
constexpr UINT ID_HZ_60 = 1402;
constexpr UINT ID_HZ_72 = 1403;
constexpr UINT ID_HZ_80 = 1404;
constexpr UINT ID_HZ_90 = 1405;
constexpr UINT ID_HZ_120 = 1406;

NOTIFYICONDATAW g_nid{};
vrpacing::RuntimeConfig g_cfg;
vrpacing::SharedState* g_shared = nullptr;
HANDLE g_mapFile = nullptr;
vrpacing::ProcessScanner g_scanner;
std::wstring g_hookPath;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

void UpdateSharedStateFlags() {
    if (!g_shared) return;
    uint32_t flags = 0;
    if (g_cfg.enabled) flags |= vrpacing::kFlagEnabled;
    if (g_cfg.upscaling.enabled) flags |= vrpacing::kFlagUpscalingEnabled;
    if (g_cfg.mask.enabled) flags |= vrpacing::kFlagMaskEnabled;
    if (g_cfg.pacing.enabled) flags |= vrpacing::kFlagPacingEnabled;
    g_shared->flags = flags;
}

void UpdateTrayTooltip() {
    wchar_t tip[128]{};
    if (g_shared && g_shared->appliedCap > 0.0) {
        swprintf_s(tip, L"VRPacing [%s] | %.1f FPS (%.0f Hz)",
                   g_shared->activeApi, g_shared->appliedCap, g_shared->effectiveHz);
    } else if (g_shared && g_shared->activeApi[0] != L'\0' && wcscmp(g_shared->activeApi, L"none") != 0 && wcscmp(g_shared->activeApi, L"idle") != 0) {
        swprintf_s(tip, L"VRPacing [%s] | Activo", g_shared->activeApi);
    } else {
        swprintf_s(tip, L"VRPacing [En espera]");
    }
    wcscpy_s(g_nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void ShowMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();

    wchar_t infoApi[64]{};
    wchar_t infoHz[64]{};
    wchar_t infoCap[64]{};
    wchar_t infoFrames[64]{};

    if (g_shared) {
        swprintf_s(infoApi, L"API: %s", (g_shared->activeApi[0] != L'\0') ? g_shared->activeApi : L"Inactivo");
        swprintf_s(infoHz, L"Refresco HMD: %.1f Hz", g_shared->effectiveHz);
        if (g_shared->appliedCap > 0.0) {
            swprintf_s(infoCap, L"Cap Pacing: %.2f FPS", g_shared->appliedCap);
        } else {
            swprintf_s(infoCap, L"Cap Pacing: Desactivado");
        }
        swprintf_s(infoFrames, L"Frames Renderizados: %llu", g_shared->frameCounter);
    } else {
        wcscpy_s(infoApi, L"API: Sin memoria compartida");
        wcscpy_s(infoHz, L"Refresco HMD: --");
        wcscpy_s(infoCap, L"Cap Pacing: --");
        wcscpy_s(infoFrames, L"Frames Renderizados: 0");
    }

    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, infoApi);
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, infoHz);
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, infoCap);
    AppendMenuW(hMenu, MF_STRING | MF_GRAYED, 0, infoFrames);
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING | (g_cfg.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TOGGLE_MASTER, L"Habilitar VRPacing");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.pacing.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TOGGLE_PACING, L"Frame Pacing");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.upscaling.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TOGGLE_UPSCALING, L"Escalado NIS");
    AppendMenuW(hMenu, MF_STRING | (g_cfg.mask.enabled ? MF_CHECKED : MF_UNCHECKED), ID_TOGGLE_MASK, L"Mascara Foveada");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hScaleMenu = CreatePopupMenu();
    auto CheckScale = [](float target) {
        return (std::abs(g_cfg.upscaling.scale - target) < 0.01f) ? MF_CHECKED : MF_UNCHECKED;
    };
    AppendMenuW(hScaleMenu, MF_STRING | CheckScale(1.0f), ID_SCALE_100, L"1.0x (Nativo)");
    AppendMenuW(hScaleMenu, MF_STRING | CheckScale(1.33f), ID_SCALE_133, L"1.33x (Calidad)");
    AppendMenuW(hScaleMenu, MF_STRING | CheckScale(1.50f), ID_SCALE_150, L"1.50x (Equilibrado)");
    AppendMenuW(hScaleMenu, MF_STRING | CheckScale(1.77f), ID_SCALE_177, L"1.77x (Rendimiento)");
    AppendMenuW(hScaleMenu, MF_STRING | CheckScale(2.0f), ID_SCALE_200, L"2.0x (NIS VR Sweet Spot)");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hScaleMenu), L"Factor de Escalado");

    HMENU hSharpMenu = CreatePopupMenu();
    auto CheckSharp = [](float target) {
        return (std::abs(g_cfg.upscaling.sharpness - target) < 0.01f) ? MF_CHECKED : MF_UNCHECKED;
    };
    AppendMenuW(hSharpMenu, MF_STRING | CheckSharp(0.0f), ID_SHARP_00, L"0% (Nitidez pura)");
    AppendMenuW(hSharpMenu, MF_STRING | CheckSharp(0.15f), ID_SHARP_15, L"15%");
    AppendMenuW(hSharpMenu, MF_STRING | CheckSharp(0.30f), ID_SHARP_30, L"30%");
    AppendMenuW(hSharpMenu, MF_STRING | CheckSharp(0.50f), ID_SHARP_50, L"50%");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSharpMenu), L"Nitidez NIS");

    HMENU hHzMenu = CreatePopupMenu();
    auto CheckHz = [](double target) {
        return (std::abs(g_cfg.pacing.hzOverride - target) < 0.1) ? MF_CHECKED : MF_UNCHECKED;
    };
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(0.0), ID_HZ_AUTO, L"Auto (Detectar HMD)");
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(60.0), ID_HZ_60, L"60 Hz (SSW 120Hz -> 59.9 FPS)");
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(72.0), ID_HZ_72, L"72 Hz (71.9 FPS)");
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(80.0), ID_HZ_80, L"80 Hz (79.9 FPS)");
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(90.0), ID_HZ_90, L"90 Hz (89.9 FPS)");
    AppendMenuW(hHzMenu, MF_STRING | CheckHz(120.0), ID_HZ_120, L"120 Hz (119.9 FPS)");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hHzMenu), L"Frecuencia Pacing");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_RELOAD_CONFIG, L"Recargar vrpacing.json");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_CONFIG, L"Editar vrpacing.json");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Salir");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            SetTimer(hwnd, IDT_SCANNER_TIMER, 1000, nullptr);
            return 0;

        case WM_TIMER:
            if (wParam == IDT_SCANNER_TIMER) {
                g_scanner.PollAndInject(g_hookPath);
                UpdateTrayTooltip();
            }
            return 0;

        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            switch (id) {
                case ID_TOGGLE_MASTER:
                    g_cfg.enabled = !g_cfg.enabled;
                    vrpacing::SaveConfig(g_cfg);
                    UpdateSharedStateFlags();
                    break;
                case ID_TOGGLE_PACING:
                    g_cfg.pacing.enabled = !g_cfg.pacing.enabled;
                    vrpacing::SaveConfig(g_cfg);
                    UpdateSharedStateFlags();
                    break;
                case ID_TOGGLE_UPSCALING:
                    g_cfg.upscaling.enabled = !g_cfg.upscaling.enabled;
                    vrpacing::SaveConfig(g_cfg);
                    UpdateSharedStateFlags();
                    break;
                case ID_TOGGLE_MASK:
                    g_cfg.mask.enabled = !g_cfg.mask.enabled;
                    vrpacing::SaveConfig(g_cfg);
                    UpdateSharedStateFlags();
                    break;

                case ID_SCALE_100: g_cfg.upscaling.scale = 1.0f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SCALE_133: g_cfg.upscaling.scale = 1.33f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SCALE_150: g_cfg.upscaling.scale = 1.50f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SCALE_177: g_cfg.upscaling.scale = 1.77f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SCALE_200: g_cfg.upscaling.scale = 2.0f; vrpacing::SaveConfig(g_cfg); break;

                case ID_SHARP_00: g_cfg.upscaling.sharpness = 0.0f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SHARP_15: g_cfg.upscaling.sharpness = 0.15f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SHARP_30: g_cfg.upscaling.sharpness = 0.30f; vrpacing::SaveConfig(g_cfg); break;
                case ID_SHARP_50: g_cfg.upscaling.sharpness = 0.50f; vrpacing::SaveConfig(g_cfg); break;

                case ID_HZ_AUTO: g_cfg.pacing.hzOverride = 0.0; vrpacing::SaveConfig(g_cfg); break;
                case ID_HZ_60:   g_cfg.pacing.hzOverride = 60.0; vrpacing::SaveConfig(g_cfg); break;
                case ID_HZ_72:   g_cfg.pacing.hzOverride = 72.0; vrpacing::SaveConfig(g_cfg); break;
                case ID_HZ_80:   g_cfg.pacing.hzOverride = 80.0; vrpacing::SaveConfig(g_cfg); break;
                case ID_HZ_90:   g_cfg.pacing.hzOverride = 90.0; vrpacing::SaveConfig(g_cfg); break;
                case ID_HZ_120:  g_cfg.pacing.hzOverride = 120.0; vrpacing::SaveConfig(g_cfg); break;

                case ID_TRAY_RELOAD_CONFIG:
                    g_cfg = vrpacing::LoadConfig();
                    UpdateSharedStateFlags();
                    vrpacing::LogMessage(vrpacing::LogLevel::Info, "Configuracion recargada desde disco.");
                    break;

                case ID_TRAY_OPEN_CONFIG: {
                    const std::string path = vrpacing::GetConfigPath();
                    const std::wstring wpath(path.begin(), path.end());
                    ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOW);
                    break;
                }

                case ID_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowMenu(hwnd);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, IDT_SCANNER_TIMER);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    vrpacing::InitLogging();
    vrpacing::LogMessage(vrpacing::LogLevel::Info, "VRPacingTray iniciado.");

    g_cfg = vrpacing::LoadConfig();
    g_hookPath = ExeDir() + L"\\VRPacingHook.dll";

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"VRPacingTrayWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VRPacingTray", 0, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"VRPacing [Iniciando]");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_mapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vrpacing::SharedState), vrpacing::kSharedStateName);
    void* view = MapViewOfFile(g_mapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrpacing::SharedState));
    g_shared = reinterpret_cast<vrpacing::SharedState*>(view);
    if (g_shared) {
        *g_shared = {};
        g_shared->version = 1;
        UpdateSharedStateFlags();
        wcscpy_s(g_shared->activeApi, L"idle");
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (view) UnmapViewOfFile(view);
    if (g_mapFile) CloseHandle(g_mapFile);

    return 0;
}

#endif