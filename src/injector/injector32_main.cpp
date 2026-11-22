#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <cstdlib>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        if (argv) LocalFree(argv);
        return 2;
    }

    const DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    const std::wstring dllPath(argv[2]);
    LocalFree(argv);

    if (pid == 0 || dllPath.empty()) {
        return 3;
    }

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                               FALSE, pid);
    if (!hProc) {
        return 4;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        return 5;
    }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 6;
    }

    LPTHREAD_START_ROUTINE loadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLib) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 7;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 8;
    }

    const DWORD waitRes = WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    if (waitRes == WAIT_OBJECT_0) {
        GetExitCodeThread(hThread, &exitCode);
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    }

    CloseHandle(hThread);
    CloseHandle(hProc);

    if (waitRes == WAIT_TIMEOUT) {
        return 10;
    }
    return (waitRes == WAIT_OBJECT_0 && exitCode != 0) ? 0 : 9;
}

#endif