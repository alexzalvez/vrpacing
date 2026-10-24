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

#include "common/logging_win.h"

namespace vrpacing {
namespace {

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

bool ProcessHasModule(DWORD pid, const std::wstring& moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &me));
    }

    CloseHandle(snapshot);
    return found;
}

} // namespace

bool ProcessScanner::InjectHookDll(const std::wstring& processName, const std::wstring& dllPath) {
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
                if (!ProcessHasModule(pe.th32ProcessID, L"VRPacingHook.dll")) {
                    result = InjectDllToProcess(pe.th32ProcessID, dllPath);
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

    if (Process32FirstW(snapshot, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == myPid) {
                continue;
            }

            if (ProcessHasModule(pe.th32ProcessID, L"VRPacingHook.dll")) {
                continue;
            }

            const bool isVRProcess =
                ProcessHasModule(pe.th32ProcessID, L"openvr_api.dll") ||
                ProcessHasModule(pe.th32ProcessID, L"LibOVRRT64_1.dll") ||
                ProcessHasModule(pe.th32ProcessID, L"VirtualDesktop.LibOVRRT64_1.dll") ||
                ProcessHasModule(pe.th32ProcessID, L"LibPVRRT64_1_X.dll");

            if (isVRProcess) {
                if (InjectDllToProcess(pe.th32ProcessID, dllPath)) {
                    LogMessage(LogLevel::Info, "Inyeccion VRPacingHook completada en PID " + std::to_string(pe.th32ProcessID));
                }
            }
        } while (Process32NextW(snapshot, &pe));
    }

    CloseHandle(snapshot);
}

} // namespace vrpacing

#endif