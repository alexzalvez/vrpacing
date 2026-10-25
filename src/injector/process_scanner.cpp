#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "process_scanner.h"

#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#include "common/logging_win.h"

namespace vrpacing {
namespace {

std::unordered_map<DWORD, int> s_inspectedPids;
std::unordered_set<DWORD> s_hookedPids;

bool IsBlacklistedProcess(const wchar_t* exeName) {
    if (!exeName) return true;

    static const wchar_t* const kBlacklist[] = {
        L"vrserver.exe",
        L"vrcompositor.exe",
        L"vrmonitor.exe",
        L"vrdashboard.exe",
        L"vrwebhelper.exe",
        L"vrstartup.exe",
        L"steamtours.exe",
        L"steam.exe",
        L"steamwebhelper.exe",
        L"OVRServer_x64.exe",
        L"OculusClient.exe",
        L"OculusDash.exe",
        L"OculusMonitor.exe",
        L"OVRRedir.exe",
        L"OVRServiceLauncher.exe",
        L"VirtualDesktop.Streamer.exe",
        L"VirtualDesktop.Server.exe",
        L"VirtualDesktop.Service.exe",
        L"VRPacingTray.exe",
        L"VectorXR.exe",
        L"VectorXR_x64.exe",
        L"explorer.exe",
        L"dwm.exe",
        L"svchost.exe",
        L"RuntimeBroker.exe",
        L"taskhostw.exe",
        L"sihost.exe",
        L"ctfmon.exe",
        L"SearchHost.exe",
        L"StartMenuExperienceHost.exe",
        L"SystemSettings.exe",
        L"cmd.exe",
        L"powershell.exe",
        L"pwsh.exe",
        L"conhost.exe",
        L"Code.exe",
        L"git.exe",
        L"devenv.exe",
        L"chrome.exe",
        L"msedge.exe",
        L"firefox.exe",
        L"discord.exe",
        L"spotify.exe",
        L"epicgameslauncher.exe",
        L"origin.exe",
        L"nvcontainer.exe",
        L"NVIDIA Share.exe"
    };

    for (const auto* blocked : kBlacklist) {
        if (_wcsicmp(exeName, blocked) == 0) {
            return true;
        }
    }
    return false;
}

bool InjectDllToProcess(DWORD pid, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
    if (!process) {
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLib, remoteMem, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    CloseHandle(process);
    return true;
}

struct ProcessModuleCheckResult {
    bool hasHook = false;
    bool isVR = false;
};

ProcessModuleCheckResult CheckProcessModules(DWORD pid) {
    ProcessModuleCheckResult res{};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return res;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, L"VRPacingHook.dll") == 0) {
                res.hasHook = true;
            }
            if (_wcsicmp(me.szModule, L"openvr_api.dll") == 0 ||
                _wcsicmp(me.szModule, L"LibOVRRT64_1.dll") == 0 ||
                _wcsicmp(me.szModule, L"VirtualDesktop.LibOVRRT64_1.dll") == 0 ||
                _wcsicmp(me.szModule, L"LibPVRRT64_1_X.dll") == 0) {
                res.isVR = true;
            }
            if (res.hasHook && res.isVR) {
                break;
            }
        } while (Module32NextW(snapshot, &me));
    }

    CloseHandle(snapshot);
    return res;
}

} // namespace

bool ProcessScanner::InjectHookDll(const std::wstring& processName, const std::wstring& dllPath) {
    if (IsBlacklistedProcess(processName.c_str())) {
        return false;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool result = false;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
                auto check = CheckProcessModules(pe.th32ProcessID);
                if (!check.hasHook) {
                    result = InjectDllToProcess(pe.th32ProcessID, dllPath);
                    if (result) {
                        s_hookedPids.insert(pe.th32ProcessID);
                        std::wstring ws(pe.szExeFile);
                        LogMessage(LogLevel::Info, "Inyeccion manual aplicada a " + std::string(ws.begin(), ws.end()) +
                                   " [PID " + std::to_string(pe.th32ProcessID) + "]");
                    }
                }
                break;
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return result;
}

void ProcessScanner::PollAndInject(const std::wstring& dllPath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    const DWORD myPid = GetCurrentProcessId();

    std::unordered_set<DWORD> activePids;

    if (Process32FirstW(snapshot, &pe)) {
        do {
            activePids.insert(pe.th32ProcessID);

            if (pe.th32ProcessID == 0 || pe.th32ProcessID == myPid) {
                continue;
            }

            if (s_hookedPids.find(pe.th32ProcessID) != s_hookedPids.end()) {
                continue;
            }

            auto it = s_inspectedPids.find(pe.th32ProcessID);
            if (it != s_inspectedPids.end() && it->second >= 3) {
                continue;
            }

            if (IsBlacklistedProcess(pe.szExeFile)) {
                s_inspectedPids[pe.th32ProcessID] = 999;
                continue;
            }

            s_inspectedPids[pe.th32ProcessID]++;

            auto check = CheckProcessModules(pe.th32ProcessID);
            if (check.hasHook) {
                s_hookedPids.insert(pe.th32ProcessID);
                continue;
            }

            if (check.isVR) {
                if (InjectDllToProcess(pe.th32ProcessID, dllPath)) {
                    s_hookedPids.insert(pe.th32ProcessID);
                    std::wstring ws(pe.szExeFile);
                    LogMessage(LogLevel::Info, "Inyeccion automatica aplicada a juego VR: " +
                               std::string(ws.begin(), ws.end()) + " [PID " + std::to_string(pe.th32ProcessID) + "]");
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);

    for (auto it = s_inspectedPids.begin(); it != s_inspectedPids.end(); ) {
        if (activePids.find(it->first) == activePids.end()) {
            it = s_inspectedPids.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s_hookedPids.begin(); it != s_hookedPids.end(); ) {
        if (activePids.find(*it) == activePids.end()) {
            it = s_hookedPids.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace vrpacing

#endif