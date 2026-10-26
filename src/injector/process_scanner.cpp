#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "injector/process_scanner.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/logging_win.h"

namespace vrpacing {
namespace {

std::unordered_set<DWORD> s_hookedPids;

bool IsBlacklistedProcess(const wchar_t* exeName) {
    if (!exeName) return true;

    static const wchar_t* const kBlacklist[] = {
        L"cemu.exe",
        L"vrserver.exe",
        L"vrcompositor.exe",
        L"vrmonitor.exe",
        L"vrdashboard.exe",
        L"vrwebhelper.exe",
        L"vrstartup.exe",
        L"steamtours.exe",
        L"steam.exe",
        L"steamwebhelper.exe",
        L"ovrserver_x64.exe",
        L"oculusclient.exe",
        L"oculusdash.exe",
        L"oculusmonitor.exe",
        L"ovrredir.exe",
        L"ovrservicelauncher.exe",
        L"virtualdesktop.streamer.exe",
        L"virtualdesktop.server.exe",
        L"virtualdesktop.service.exe",
        L"vrpacingtray.exe",
        L"vectorxr.exe",
        L"vectorxr_x64.exe",
        L"explorer.exe",
        L"dwm.exe",
        L"svchost.exe",
        L"runtimebroker.exe",
        L"taskhostw.exe",
        L"sihost.exe",
        L"ctfmon.exe",
        L"searchhost.exe",
        L"devenv.exe"
    };

    std::wstring lower(exeName);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

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

bool InjectDllByPid(DWORD pid, const std::wstring& dllPath) {
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

bool ProcessScanner::InjectHookDll(const std::wstring& processName, const std::wstring& dllPath) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool injected = false;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
                if (InjectDllByPid(pe.th32ProcessID, dllPath)) {
                    s_hookedPids.insert(pe.th32ProcessID);
                    injected = true;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return injected;
}

void ProcessScanner::PollAndInject(const std::wstring& dllPath) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == GetCurrentProcessId()) continue;
            if (s_hookedPids.find(pid) != s_hookedPids.end()) continue;

            if (IsBlacklistedProcess(pe.szExeFile)) {
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProc) continue;

            const bool isOpenXRLayer = IsModuleLoadedInProcess(hProc, L"VRPacingLayer.dll");
            if (isOpenXRLayer) {
                s_hookedPids.insert(pid);
                LogMessage(LogLevel::Info, "Proceso VR ya gestionado por OpenXR Layer (omitiendo VRPacingHook): " +
                           std::string(pe.szExeFile, pe.szExeFile + wcslen(pe.szExeFile)) +
                           " [PID " + std::to_string(pid) + "]");
                CloseHandle(hProc);
                continue;
            }

            const bool isOpenVR = IsModuleLoadedInProcess(hProc, L"openvr_api.dll");
            const bool isOculus = IsModuleLoadedInProcess(hProc, L"LibOVRRT64_1.dll") ||
                                  IsModuleLoadedInProcess(hProc, L"VirtualDesktop.LibOVRRT64_1.dll");

            if (isOpenVR || isOculus) {
                if (InjectDllByPid(pid, dllPath)) {
                    s_hookedPids.insert(pid);
                    LogMessage(LogLevel::Info, "Inyeccion automatica aplicada a juego VR: " +
                               std::string(pe.szExeFile, pe.szExeFile + wcslen(pe.szExeFile)) +
                               " [PID " + std::to_string(pid) + "]");
                }
            }

            CloseHandle(hProc);
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

} // namespace vrpacing

#endif