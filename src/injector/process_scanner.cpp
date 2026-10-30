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
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging_win.h"

#pragma comment(lib, "advapi32.lib")

namespace vrpacing {
namespace {

std::unordered_set<DWORD> s_systemIgnoredPids;
std::unordered_set<DWORD> s_hookedPids;
std::unordered_map<DWORD, int> s_openFailCounts;

void EnableDebugPrivilegeOnce() {
    static std::once_flag s_privFlag;
    std::call_once(s_privFlag, []() {
        HANDLE hToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            return;
        }
        LUID luid{};
        if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(hToken);
    });
}

bool IsBlacklistedProcess(const wchar_t* exeName) {
    if (!exeName) return true;

    static const wchar_t* const kBlacklist[] = {
        L"system",
        L"secure system",
        L"registry",
        L"memory compression",
        L"cemu.exe",
        L"bettervr_launcher.exe",
        L"ofxrbridgetray.exe",
        L"ofxrbridge.exe",
        L"vrserver.exe",
        L"vrcompositor.exe",
        L"vrmonitor.exe",
        L"vrdashboard.exe",
        L"vrwebhelper.exe",
        L"vrstartup.exe",
        L"steamtours.exe",
        L"steam.exe",
        L"steamwebhelper.exe",
        L"steamservice.exe",
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
        L"searchindexer.exe",
        L"searchprotocolhost.exe",
        L"searchfilterhost.exe",
        L"devenv.exe",
        L"csrss.exe",
        L"smss.exe",
        L"services.exe",
        L"lsass.exe",
        L"lsaiso.exe",
        L"wininit.exe",
        L"winlogon.exe",
        L"fontdrvhost.exe",
        L"spoolsv.exe",
        L"audiodg.exe",
        L"wudfhost.exe",
        L"wmiprvse.exe",
        L"conhost.exe",
        L"powershell.exe",
        L"pwsh.exe",
        L"cmd.exe",
        L"msiafterburner.exe",
        L"rtss.exe",
        L"rtsshooksloader64.exe",
        L"encoderserver.exe",
        L"nvcontainer.exe",
        L"nvdisplay.container.exe",
        L"msmpeng.exe",
        L"mpdefendercoservice.exe",
        L"nissrv.exe",
        L"securityhealthservice.exe",
        L"gameinputsvc.exe",
        L"gamingservices.exe",
        L"gamingservicesnet.exe"
    };

    std::wstring lower(exeName);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    for (const auto* b : kBlacklist) {
        if (lower == b) return true;
    }
    return false;
}

bool IsSystemWindowsProcess(HANDLE hProc) {
    wchar_t fullPath[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProc, 0, fullPath, &sz)) {
        return false;
    }
    std::wstring lower(fullPath);
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return (lower.find(L"\\windows\\") != std::wstring::npos ||
            lower.find(L"\\microsoft office\\") != std::wstring::npos ||
            lower.find(L"\\windows defender\\") != std::wstring::npos);
}

bool HasVrRuntimeModuleLoaded(HANDLE hProc, bool& alreadyHooked) {
    alreadyHooked = false;
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModulesEx(hProc, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_64BIT)) {
        if (!EnumProcessModules(hProc, hMods, sizeof(hMods), &cbNeeded)) {
            return false;
        }
    }

    const DWORD count = (std::min)(static_cast<DWORD>(1024), static_cast<DWORD>(cbNeeded / sizeof(HMODULE)));
    wchar_t modName[MAX_PATH]{};
    bool foundVrMod = false;

    for (DWORD i = 0; i < count; ++i) {
        if (GetModuleBaseNameW(hProc, hMods[i], modName, MAX_PATH)) {
            if (_wcsicmp(modName, L"VRPacingHook.dll") == 0 ||
                _wcsicmp(modName, L"VRPacingLayer.dll") == 0) {
                alreadyHooked = true;
                return false;
            }
            if (_wcsicmp(modName, L"openvr_api.dll") == 0 ||
                _wcsicmp(modName, L"LibOVRRT64_1.dll") == 0 ||
                _wcsicmp(modName, L"VirtualDesktop.LibOVRRT64_1.dll") == 0 ||
                _wcsicmp(modName, L"LibPVRRT64_1_X.dll") == 0) {
                foundVrMod = true;
            }
        }
    }
    return foundVrMod;
}

bool InjectDllByPid(DWORD pid, const std::wstring& dllPath) {
    if (!std::filesystem::exists(dllPath)) {
        LogMessage(LogLevel::Error, "ProcessScanner: Hook DLL not found on disk.");
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
    EnableDebugPrivilegeOnce();

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
    EnableDebugPrivilegeOnce();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    if (s_systemIgnoredPids.size() > 4096) {
        s_systemIgnoredPids.clear();
        s_openFailCounts.clear();
    }

    std::unordered_set<DWORD> activeSnapPids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            activeSnapPids.insert(pid);

            if (pid <= 4 || pid == GetCurrentProcessId()) continue;
            if (s_hookedPids.find(pid) != s_hookedPids.end()) continue;
            if (s_systemIgnoredPids.find(pid) != s_systemIgnoredPids.end()) continue;

            if (IsBlacklistedProcess(pe.szExeFile)) {
                s_systemIgnoredPids.insert(pid);
                continue;
            }

            HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hCheck) {
                const bool isWinSys = IsSystemWindowsProcess(hCheck);
                CloseHandle(hCheck);
                if (isWinSys) {
                    s_systemIgnoredPids.insert(pid);
                    continue;
                }
            } else {
                if (++s_openFailCounts[pid] >= 5) {
                    s_systemIgnoredPids.insert(pid);
                    s_openFailCounts.erase(pid);
                }
                continue;
            }

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (!hProc) {
                if (++s_openFailCounts[pid] >= 5) {
                    s_systemIgnoredPids.insert(pid);
                    s_openFailCounts.erase(pid);
                }
                continue;
            }

            s_openFailCounts.erase(pid);

            bool alreadyHooked = false;
            const bool isVrTarget = HasVrRuntimeModuleLoaded(hProc, alreadyHooked);
            CloseHandle(hProc);

            if (alreadyHooked) {
                s_hookedPids.insert(pid);
                continue;
            }

            if (isVrTarget) {
                if (InjectDllByPid(pid, dllPath)) {
                    s_hookedPids.insert(pid);
                    char exeA[MAX_PATH]{};
                    size_t converted = 0;
                    wcstombs_s(&converted, exeA, pe.szExeFile, sizeof(exeA) - 1);
                    LogMessage(LogLevel::Info, "Automatic injection applied to VR process: " +
                               std::string(exeA) + " [PID " + std::to_string(pid) + "]");
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    for (auto it = s_hookedPids.begin(); it != s_hookedPids.end();) {
        if (activeSnapPids.find(*it) == activeSnapPids.end()) {
            it = s_hookedPids.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace vrpacing

#endif