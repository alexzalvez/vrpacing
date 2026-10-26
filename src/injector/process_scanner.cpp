#ifdef _WIN32

#include "injector/process_scanner.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "common/logging_win.h"
#include "vrpacing/shared_state.h"

namespace vrpacing {
namespace {

bool IsProcessBlacklisted(const std::wstring& exeName) {
    std::wstring lower = exeName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    static const wchar_t* kBlacklist[] = {
        L"cemu.exe",
        L"devenv.exe",
        L"vrpacingtray.exe",
        L"steam.exe",
        L"steamtours.exe",
        L"oculusclient.exe",
        L"virtualdesktop.streamer.exe"
    };

    for (const auto* b : kBlacklist) {
        if (lower == b) return true;
    }
    return false;
}

bool IsModuleLoadedInProcess(HANDLE hProc, const wchar_t* targetDllName) {
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(hProc, hMods, sizeof(hMods), &cbNeeded)) {
        return false;
    }

    const DWORD count = cbNeeded / sizeof(HMODULE);
    wchar_t modName[MAX_PATH];

    for (DWORD i = 0; i < count; ++i) {
        if (GetModuleBaseNameW(hProc, hMods[i], modName, MAX_PATH)) {
            if (_wcsicmp(modName, targetDllName) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool InjectDll(DWORD pid, const std::wstring& dllPath) {
    if (!std::filesystem::exists(dllPath)) {
        LogMessage(LogLevel::Error, "ProcessScanner: DLL de inyeccion no encontrada en disco.");
        return false;
    }

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) {
        return false;
    }

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathBytes, nullptr)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    LPTHREAD_START_ROUTINE loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return true;
}

} // namespace

void ProcessScanner::PollAndInject(const std::wstring& hookDllPath) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == GetCurrentProcessId()) continue;
            if (injectedPids_.find(pid) != injectedPids_.end()) continue;

            std::wstring exeName = pe.szExeFile;
            if (IsProcessBlacklisted(exeName)) {
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProc) continue;

            const bool isOpenVR = IsModuleLoadedInProcess(hProc, L"openvr_api.dll");
            const bool isOculus = IsModuleLoadedInProcess(hProc, L"LibOVRRT64_1.dll") ||
                                  IsModuleLoadedInProcess(hProc, L"VirtualDesktop.LibOVRRT64_1.dll");

            // Si es un proceso OpenXR nativo que ya carga VRPacingLayer, omitir inyeccion de hook
            const bool isOpenXRLayer = IsModuleLoadedInProcess(hProc, L"VRPacingLayer.dll");

            if (isOpenXRLayer) {
                injectedPids_.insert(pid);
                LogMessage(LogLevel::Info, "Proceso VR ya gestionado por OpenXR Layer (omitiendo VRPacingHook): " +
                           std::string(exeName.begin(), exeName.end()) + " [PID " + std::to_string(pid) + "]");
                CloseHandle(hProc);
                continue;
            }

            if (isOpenVR || isOculus) {
                if (InjectDll(pid, hookDllPath)) {
                    injectedPids_.insert(pid);
                    LogMessage(LogLevel::Info, "Inyeccion automatica aplicada a juego VR: " +
                               std::string(exeName.begin(), exeName.end()) + " [PID " + std::to_string(pid) + "]");
                }
            }

            CloseHandle(hProc);
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

} // namespace vrpacing

#endif