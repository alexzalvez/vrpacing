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
#include <cstdint>
#include <cwchar>
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

// Contrato de concurrencia de ProcessScanner:
// Todas las estructuras de estado de este archivo son accedidas exclusivamente desde el
// hilo principal de interfaz (UI thread) de VRPacingTray.exe a traves del temporizador
// WM_TIMER (IDT_SCANNER_TIMER). No existen llamadas concurrentes desde hilos secundarios.

struct TrackedProcessInstance {
    DWORD pid = 0;
    uint64_t creationTime = 0;
    std::string exeName;
    HANDLE hProcess = nullptr;
    void* preservedRemoteMem = nullptr;
    std::string blockReason;
};

struct PendingInjection {
    DWORD pid = 0;
    uint64_t creationTime = 0;
    std::string exeName;
    HANDLE hProcess = nullptr;
    HANDLE hThread = nullptr;
    void* remoteMem = nullptr;
    ULONGLONG startTick = 0;
    bool threadFinished = false;
    bool loadedLogged = false;
    bool readyLogged = false;
};

std::unordered_set<DWORD> s_systemIgnoredPids;
std::unordered_map<DWORD, int> s_openFailCounts;
std::unordered_map<DWORD, TrackedProcessInstance> s_hookedInstances;
std::unordered_map<DWORD, TrackedProcessInstance> s_blockedInstances;
std::unordered_map<DWORD, PendingInjection> s_pendingInjections;

std::string WideToNarrow(const wchar_t* wstr) {
    if (!wstr) return "";
    char buf[MAX_PATH]{};
    size_t converted = 0;
    wcstombs_s(&converted, buf, wstr, sizeof(buf) - 1);
    return std::string(buf);
}

uint64_t QueryProcessCreationTime(HANDLE hProc) {
    if (!hProc) return 0;
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = ftCreate.dwLowDateTime;
        uli.HighPart = ftCreate.dwHighDateTime;
        return uli.QuadPart;
    }
    return 0;
}

bool HasProcessHandleExited(HANDLE hProc) {
    if (!hProc) return true;
    const DWORD waitRes = WaitForSingleObject(hProc, 0);
    return (waitRes == WAIT_OBJECT_0);
}

bool IsNamedInstanceEventSignaled(const wchar_t* prefix, DWORD pid, uint64_t creationTime) {
    wchar_t evName[128]{};
    swprintf_s(evName, L"%s%lu_%llX", prefix, pid, static_cast<unsigned long long>(creationTime));
    HANDLE hEv = OpenEventW(SYNCHRONIZE, FALSE, evName);
    if (!hEv) return false;
    const DWORD res = WaitForSingleObject(hEv, 0);
    CloseHandle(hEv);
    return (res == WAIT_OBJECT_0);
}

void ReleaseTrackedInstance(TrackedProcessInstance& inst) {
    if (inst.hProcess) {
        CloseHandle(inst.hProcess);
        inst.hProcess = nullptr;
    }
    inst.preservedRemoteMem = nullptr;
}

void BlockProcessInstanceFromRetry(PendingInjection& entry, bool preserveRemoteMem, const std::string& reason) {
    if (!preserveRemoteMem && entry.remoteMem && entry.hProcess) {
        VirtualFreeEx(entry.hProcess, entry.remoteMem, 0, MEM_RELEASE);
        entry.remoteMem = nullptr;
    }
    if (entry.hThread) {
        CloseHandle(entry.hThread);
        entry.hThread = nullptr;
    }

    TrackedProcessInstance blocked{};
    blocked.pid = entry.pid;
    blocked.creationTime = entry.creationTime;
    blocked.exeName = entry.exeName;
    blocked.hProcess = entry.hProcess;
    blocked.preservedRemoteMem = preserveRemoteMem ? entry.remoteMem : nullptr;
    blocked.blockReason = reason;

    entry.hProcess = nullptr;
    entry.remoteMem = nullptr;

    s_blockedInstances[blocked.pid] = blocked;
    LogMessage(LogLevel::Warn, "ProcessScanner: Blocked further injection retries for " +
               blocked.exeName + " [PID " + std::to_string(blocked.pid) +
               "] until process exit. Reason: " + reason);
}

void PromoteToHookedInstance(PendingInjection& entry) {
    if (entry.remoteMem && entry.hProcess) {
        VirtualFreeEx(entry.hProcess, entry.remoteMem, 0, MEM_RELEASE);
        entry.remoteMem = nullptr;
    }
    if (entry.hThread) {
        CloseHandle(entry.hThread);
        entry.hThread = nullptr;
    }

    TrackedProcessInstance hooked{};
    hooked.pid = entry.pid;
    hooked.creationTime = entry.creationTime;
    hooked.exeName = entry.exeName;
    hooked.hProcess = entry.hProcess;
    entry.hProcess = nullptr;

    s_hookedInstances[hooked.pid] = hooked;
}

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

void PruneExitedTrackedProcesses() {
    for (auto it = s_hookedInstances.begin(); it != s_hookedInstances.end();) {
        if (HasProcessHandleExited(it->second.hProcess)) {
            ReleaseTrackedInstance(it->second);
            it = s_hookedInstances.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = s_blockedInstances.begin(); it != s_blockedInstances.end();) {
        if (HasProcessHandleExited(it->second.hProcess)) {
            ReleaseTrackedInstance(it->second);
            it = s_blockedInstances.erase(it);
        } else {
            ++it;
        }
    }
}

void UpdatePendingInjections() {
    for (auto it = s_pendingInjections.begin(); it != s_pendingInjections.end();) {
        PendingInjection& entry = it->second;

        if (HasProcessHandleExited(entry.hProcess)) {
            if (entry.hThread) CloseHandle(entry.hThread);
            if (entry.hProcess) CloseHandle(entry.hProcess);
            it = s_pendingInjections.erase(it);
            continue;
        }

        if (!entry.threadFinished && entry.hThread) {
            const DWORD waitRes = WaitForSingleObject(entry.hThread, 0);
            if (waitRes == WAIT_OBJECT_0) {
                if (entry.remoteMem && entry.hProcess) {
                    VirtualFreeEx(entry.hProcess, entry.remoteMem, 0, MEM_RELEASE);
                    entry.remoteMem = nullptr;
                }
                CloseHandle(entry.hThread);
                entry.hThread = nullptr;
                entry.threadFinished = true;

                const bool loaded = IsNamedInstanceEventSignaled(L"Local\\VRPacingHookLoaded_", entry.pid, entry.creationTime);
                const bool ready = IsNamedInstanceEventSignaled(L"Local\\VRPacingHookReady_", entry.pid, entry.creationTime);
                LogMessage(LogLevel::Info, "ProcessScanner: Deferred remote thread finished for " +
                           entry.exeName + " [PID " + std::to_string(entry.pid) +
                           "] (DLL Loaded=" + std::to_string(loaded ? 1 : 0) +
                           ", OpenVR Ready=" + std::to_string(ready ? 1 : 0) + ").");

                if (loaded) entry.loadedLogged = true;
                if (ready) entry.readyLogged = true;
            } else if (waitRes != WAIT_TIMEOUT) {
                const DWORD err = GetLastError();
                BlockProcessInstanceFromRetry(entry, true,
                                              "WaitForSingleObject failed on pending thread (GLE=" + std::to_string(err) + ")");
                it = s_pendingInjections.erase(it);
                continue;
            } else if ((GetTickCount64() - entry.startTick) > 15000) {
                BlockProcessInstanceFromRetry(entry, true,
                                              "Remote LoadLibraryW thread exceeded 15000ms without terminating");
                it = s_pendingInjections.erase(it);
                continue;
            }
        }

        if (entry.threadFinished) {
            if (!entry.loadedLogged &&
                IsNamedInstanceEventSignaled(L"Local\\VRPacingHookLoaded_", entry.pid, entry.creationTime)) {
                entry.loadedLogged = true;
                LogMessage(LogLevel::Info, "ProcessScanner: VRPacingHook DLL confirmed loaded in " +
                           entry.exeName + " [PID " + std::to_string(entry.pid) + "].");
            }

            if (!entry.readyLogged &&
                IsNamedInstanceEventSignaled(L"Local\\VRPacingHookReady_", entry.pid, entry.creationTime)) {
                entry.readyLogged = true;
                LogMessage(LogLevel::Info, "ProcessScanner: OpenVR VR_GetGenericInterface hook confirmed READY in " +
                           entry.exeName + " [PID " + std::to_string(entry.pid) + "].");
            }

            if (entry.readyLogged) {
                PromoteToHookedInstance(entry);
                it = s_pendingInjections.erase(it);
                continue;
            }

            if (IsNamedInstanceEventSignaled(L"Local\\VRPacingHookFailed_", entry.pid, entry.creationTime)) {
                BlockProcessInstanceFromRetry(entry, false,
                                              "VRPacingHook reported terminal hook installation failure via Failed event");
                it = s_pendingInjections.erase(it);
                continue;
            }

            if ((GetTickCount64() - entry.startTick) > 15000) {
                BlockProcessInstanceFromRetry(entry, false,
                                              entry.loadedLogged
                                                  ? "DLL loaded without verified OpenVR Ready signal (e.g. unverified Oculus path)"
                                                  : "Timed out (15000ms) waiting for DLL Load/Ready confirmation");
                it = s_pendingInjections.erase(it);
                continue;
            }
        }

        ++it;
    }
}

bool StartOrCompleteInjectionByPid(DWORD pid, const wchar_t* exeNameW, const std::wstring& dllPath, DWORD waitTimeoutMs) {
    if (s_pendingInjections.find(pid) != s_pendingInjections.end() ||
        s_hookedInstances.find(pid) != s_hookedInstances.end() ||
        s_blockedInstances.find(pid) != s_blockedInstances.end()) {
        return false;
    }

    if (!std::filesystem::exists(dllPath)) {
        LogMessage(LogLevel::Error, "ProcessScanner: Hook DLL not found on disk.");
        return false;
    }

    const std::string exeName = WideToNarrow(exeNameW);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                               FALSE, pid);
    if (!hProc) {
        return false;
    }

    const uint64_t creationTime = QueryProcessCreationTime(hProc);
    if (creationTime == 0) {
        CloseHandle(hProc);
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
        const DWORD err = GetLastError();
        LogMessage(LogLevel::Error, "ProcessScanner: CreateRemoteThread failed for " +
                   exeName + " [PID " + std::to_string(pid) + "] (GLE=" + std::to_string(err) + ").");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    PendingInjection entry{};
    entry.pid = pid;
    entry.creationTime = creationTime;
    entry.exeName = exeName;
    entry.hProcess = hProc;
    entry.hThread = hThread;
    entry.remoteMem = remoteMem;
    entry.startTick = GetTickCount64();

    const DWORD waitRes = WaitForSingleObject(hThread, waitTimeoutMs);
    if (waitRes == WAIT_OBJECT_0) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        entry.remoteMem = nullptr;
        CloseHandle(hThread);
        entry.hThread = nullptr;
        entry.threadFinished = true;

        const bool loaded = IsNamedInstanceEventSignaled(L"Local\\VRPacingHookLoaded_", pid, creationTime);
        const bool ready = IsNamedInstanceEventSignaled(L"Local\\VRPacingHookReady_", pid, creationTime);
        const bool failed = IsNamedInstanceEventSignaled(L"Local\\VRPacingHookFailed_", pid, creationTime);

        LogMessage(LogLevel::Info, "ProcessScanner: Injection thread finished for " +
                   exeName + " [PID " + std::to_string(pid) +
                   "] (DLL Loaded=" + std::to_string(loaded ? 1 : 0) +
                   ", OpenVR Ready=" + std::to_string(ready ? 1 : 0) + ").");

        if (loaded) entry.loadedLogged = true;
        if (ready) {
            entry.readyLogged = true;
            LogMessage(LogLevel::Info, "ProcessScanner: OpenVR VR_GetGenericInterface hook confirmed READY in " +
                       exeName + " [PID " + std::to_string(pid) + "].");
            PromoteToHookedInstance(entry);
            return true;
        }

        if (failed) {
            BlockProcessInstanceFromRetry(entry, false,
                                          "VRPacingHook reported terminal hook installation failure via Failed event");
            return false;
        }

        s_pendingInjections[pid] = entry;
        return loaded;
    } else if (waitRes == WAIT_TIMEOUT) {
        LogMessage(LogLevel::Warn, "ProcessScanner: Remote thread WAIT_TIMEOUT (" +
                   std::to_string(waitTimeoutMs) + "ms) for " + exeName +
                   " [PID " + std::to_string(pid) + "]. Preserving remoteMem and monitoring asynchronously.");
        s_pendingInjections[pid] = entry;
        return false;
    } else {
        const DWORD err = GetLastError();
        BlockProcessInstanceFromRetry(entry, true,
                                      "Initial WaitForSingleObject failed (res=" + std::to_string(waitRes) +
                                      ", GLE=" + std::to_string(err) + ")");
        return false;
    }
}

} // namespace

// Nota: Debe invocarse exclusivamente desde el mismo hilo de interfaz que PollAndInject.
bool ProcessScanner::InjectHookDll(const std::wstring& processName, const std::wstring& dllPath) {
    EnableDebugPrivilegeOnce();
    PruneExitedTrackedProcesses();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool injected = false;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0) {
                if (StartOrCompleteInjectionByPid(pe.th32ProcessID, pe.szExeFile, dllPath, 5000)) {
                    injected = true;
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return injected;
}

// Invocado exclusivamente desde el hilo UI de VRPacingTray.exe mediante WM_TIMER (IDT_SCANNER_TIMER).
void ProcessScanner::PollAndInject(const std::wstring& dllPath) {
    static std::once_flag s_logInitFlag;
    std::call_once(s_logInitFlag, []() {
        LogMessage(LogLevel::Info,
                   std::string("ProcessScanner initialized (single-threaded UI timer contract, Build: ") +
                   __DATE__ + " " + __TIME__ + ").");
    });

    EnableDebugPrivilegeOnce();
    PruneExitedTrackedProcesses();
    UpdatePendingInjections();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    if (s_systemIgnoredPids.size() > 4096) {
        s_systemIgnoredPids.clear();
        s_openFailCounts.clear();
    }

    std::vector<PROCESSENTRY32W> processEntries;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(hSnap, &pe)) {
        do {
            processEntries.push_back(pe);
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);

    for (const auto& entry : processEntries) {
        const DWORD pid = entry.th32ProcessID;

        if (pid <= 4 || pid == GetCurrentProcessId()) continue;
        if (s_hookedInstances.find(pid) != s_hookedInstances.end()) continue;
        if (s_blockedInstances.find(pid) != s_blockedInstances.end()) continue;
        if (s_pendingInjections.find(pid) != s_pendingInjections.end()) continue;
        if (s_systemIgnoredPids.find(pid) != s_systemIgnoredPids.end()) continue;

        if (IsBlacklistedProcess(entry.szExeFile)) {
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

        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
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

        if (alreadyHooked) {
            TrackedProcessInstance hooked{};
            hooked.pid = pid;
            hooked.creationTime = QueryProcessCreationTime(hProc);
            hooked.exeName = WideToNarrow(entry.szExeFile);
            hooked.hProcess = hProc;
            s_hookedInstances[pid] = hooked;
            continue;
        }

        CloseHandle(hProc);

        if (isVrTarget) {
            StartOrCompleteInjectionByPid(pid, entry.szExeFile, dllPath, 1500);
        }
    }
}

} // namespace vrpacing

#endif