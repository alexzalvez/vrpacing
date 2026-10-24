#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>

#include <string>

#include "common/config_json.h"
#include "common/logging_win.h"
#include "injector/process_scanner.h"
#include "vrpacing/shared_state.h"

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_TRAY_EXIT = 1001;

NOTIFYICONDATAW g_nid{};
vrpacing::RuntimeConfig g_cfg;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

void ShowMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    InsertMenuW(menu, -1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Salir");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_EXIT) {
                PostQuitMessage(0);
            }
            return 0;
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowMenu(hwnd);
            }
            return 0;
        case WM_DESTROY:
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
    wcscpy_s(g_nid.szTip, L"VRPacing");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(vrpacing::SharedState), vrpacing::kSharedStateName);
    void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vrpacing::SharedState));
    auto* shared = reinterpret_cast<vrpacing::SharedState*>(view);
    if (shared) {
        *shared = {};
        shared->version = 1;
        shared->flags = vrpacing::kFlagEnabled;
        wcscpy_s(shared->activeApi, L"idle");
    }

    vrpacing::ProcessScanner scanner;
    const std::wstring hookPath = ExeDir() + L"\\VRPacingHook.dll";

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (view) UnmapViewOfFile(view);
                if (map) CloseHandle(map);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        scanner.PollAndInject(hookPath);
        Sleep(500);
    }
}

#endif
