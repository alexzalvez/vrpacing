#ifdef _WIN32

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>

#include "common/logging_win.h"
#include "openvr/hook_openvr.h"
#include "oculus/hook_oculus.h"

namespace vrpacing {
bool InstallOpenVRHooksWithStatus();
}

namespace {

enum class BackendStatus {
    NotPresent,
    TransientFailure,
    ReadyVerified,
    UnverifiedInvoked,
    TerminalFailure
};

// Politica de ciclo de vida:
// 1. La descarga dinamica (FreeLibrary) NO esta soportada. Antes de instalar cualquier hook,
//    el modulo se fija en el espacio de direcciones del proceso mediante GET_MODULE_HANDLE_EX_FLAG_PIN.
//    Si el pin falla, se aborta la instalacion de todos los hooks y se senala Failed.
// 2. DllMain nunca adquiere g_lifecycleMutex, nunca cierra handles compartidos con el worker y
//    nunca invoca funciones de logging bajo el Windows Loader Lock.
// 3. HookWatcherThread y VRPacingHook_RefreshHooks comparten un unico estado serializado (g_state).
struct LifecycleState {
    bool loggingInitialized = false;
    bool modulePinned = false;
    bool pinFailedTerminal = false;
    bool loadedSignaled = false;
    bool readySignaled = false;
    bool failedSignaled = false;
    BackendStatus openvrStatus = BackendStatus::NotPresent;
    int openvrFailures = 0;
    BackendStatus oculusStatus = BackendStatus::NotPresent;
};

constexpr int kMaxOpenVrInstallAttempts = 10;

std::atomic<bool> g_stopWatcher{ false };
std::mutex g_lifecycleMutex;
LifecycleState g_state{};
HANDLE g_hLoadedEvent = nullptr;
HANDLE g_hReadyEvent = nullptr;
HANDLE g_hFailedEvent = nullptr;

uint64_t GetCurrentProcessCreationTime() {
    FILETIME ftCreate{}, ftExit{}, ftKernel{}, ftUser{};
    if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = ftCreate.dwLowDateTime;
        uli.HighPart = ftCreate.dwHighDateTime;
        return uli.QuadPart;
    }
    return 0;
}

bool TryPinSelfModule() {
    HMODULE hPin = nullptr;
    return (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&TryPinSelfModule), &hPin) != FALSE &&
            hPin != nullptr);
}

bool SignalNamedProcessEventLocked(const wchar_t* prefix, HANDLE& outHandle) {
    const DWORD pid = GetCurrentProcessId();
    const uint64_t createTime = GetCurrentProcessCreationTime();
    if (createTime == 0) {
        const DWORD err = GetLastError();
        vrpacing::LogMessage(vrpacing::LogLevel::Error,
                             "HookLifecycle: GetCurrentProcessCreationTime failed (GLE=" +
                             std::to_string(err) + "); refusing to signal event with invalid identity.");
        return false;
    }

    wchar_t evName[128]{};
    swprintf_s(evName, L"%s%lu_%llX", prefix, pid, static_cast<unsigned long long>(createTime));
    if (!outHandle) {
        outHandle = CreateEventW(nullptr, TRUE, FALSE, evName);
        if (!outHandle) {
            const DWORD err = GetLastError();
            vrpacing::LogMessage(vrpacing::LogLevel::Error,
                                 "HookLifecycle: CreateEventW failed (GLE=" + std::to_string(err) + ").");
            return false;
        }
    }

    if (!SetEvent(outHandle)) {
        const DWORD err = GetLastError();
        vrpacing::LogMessage(vrpacing::LogLevel::Error,
                             "HookLifecycle: SetEvent failed (GLE=" + std::to_string(err) + ").");
        return false;
    }
    return true;
}

void AdvanceHookLifecycleLocked() {
    if (!g_state.loggingInitialized) {
        vrpacing::InitLogging();
        vrpacing::LogMessage(vrpacing::LogLevel::Info,
                             std::string("VRPacingHook worker initialized outside Loader Lock (Build: ") +
                             __DATE__ + " " + __TIME__ + ").");
        g_state.loggingInitialized = true;
    }

    if (!g_state.loadedSignaled) {
        if (SignalNamedProcessEventLocked(L"Local\\VRPacingHookLoaded_", g_hLoadedEvent)) {
            g_state.loadedSignaled = true;
        }
    }

    if (g_state.pinFailedTerminal) {
        if (!g_state.readySignaled && !g_state.failedSignaled) {
            if (SignalNamedProcessEventLocked(L"Local\\VRPacingHookFailed_", g_hFailedEvent)) {
                g_state.failedSignaled = true;
            }
        }
        return;
    }

    const bool hasOpenVrMod = (GetModuleHandleW(L"openvr_api.dll") != nullptr);
    const bool hasOculusMod = (GetModuleHandleW(L"LibOVRRT64_1.dll") != nullptr ||
                               GetModuleHandleW(L"VirtualDesktop.LibOVRRT64_1.dll") != nullptr ||
                               GetModuleHandleW(L"LibPVRRT64_1_X.dll") != nullptr);

    if (!hasOpenVrMod && !hasOculusMod) {
        return;
    }

    // Requisito estricto antes de instalar cualquier hook: fijar la DLL en memoria.
    if (!g_state.modulePinned) {
        if (TryPinSelfModule()) {
            g_state.modulePinned = true;
        } else {
            g_state.pinFailedTerminal = true;
            vrpacing::LogMessage(vrpacing::LogLevel::Error,
                                 "CRITICAL: GET_MODULE_HANDLE_EX_FLAG_PIN failed. Aborting all hook installations.");
            if (!g_state.readySignaled && !g_state.failedSignaled) {
                if (SignalNamedProcessEventLocked(L"Local\\VRPacingHookFailed_", g_hFailedEvent)) {
                    g_state.failedSignaled = true;
                }
            }
            return;
        }
    }

    // 1. Ruta OpenVR (con resultado verificable sobre VR_GetGenericInterface)
    if (hasOpenVrMod &&
        g_state.openvrStatus != BackendStatus::ReadyVerified &&
        g_state.openvrStatus != BackendStatus::TerminalFailure) {
        if (vrpacing::InstallOpenVRHooksWithStatus()) {
            g_state.openvrStatus = BackendStatus::ReadyVerified;
            g_state.openvrFailures = 0;
        } else {
            g_state.openvrFailures++;
            if (g_state.openvrFailures >= kMaxOpenVrInstallAttempts) {
                g_state.openvrStatus = BackendStatus::TerminalFailure;
                vrpacing::LogMessage(vrpacing::LogLevel::Error,
                                     "HookLifecycle: OpenVR hook installation reached terminal failure after " +
                                     std::to_string(g_state.openvrFailures) + " attempts.");
            } else {
                g_state.openvrStatus = BackendStatus::TransientFailure;
            }
        }
    }

    // Publicacion/recuperacion de eventos Ready/Failed desacoplada de la instalacion:
    // si SetEvent fallo transitoriamente en un ciclo previo, se reintenta sin reinstalar hooks.
    if (g_state.openvrStatus == BackendStatus::ReadyVerified &&
        !g_state.readySignaled && !g_state.failedSignaled) {
        if (SignalNamedProcessEventLocked(L"Local\\VRPacingHookReady_", g_hReadyEvent)) {
            g_state.readySignaled = true;
            vrpacing::LogMessage(vrpacing::LogLevel::Info,
                                 "HookLifecycle: OpenVR VR_GetGenericInterface acquisition hook verified READY.");
        }
    } else if (g_state.openvrStatus == BackendStatus::TerminalFailure &&
               !g_state.readySignaled && !g_state.failedSignaled) {
        if (SignalNamedProcessEventLocked(L"Local\\VRPacingHookFailed_", g_hFailedEvent)) {
            g_state.failedSignaled = true;
        }
    }

    // 2. Ruta Oculus (instalador void sin retorno verificable; no afirma Ready)
    if (hasOculusMod && g_state.oculusStatus == BackendStatus::NotPresent) {
        vrpacing::InstallOculusHooks();
        g_state.oculusStatus = BackendStatus::UnverifiedInvoked;
        vrpacing::LogMessage(vrpacing::LogLevel::Info,
                             "HookLifecycle: Oculus LibOVR installer invoked (unverified status; Ready event not asserted).");
    }
}

DWORD WINAPI HookWatcherThread(LPVOID) {
    int postSettleSignalRetries = 0;
    while (!g_stopWatcher.load(std::memory_order_relaxed)) {
        bool shouldExitLoop = false;
        {
            std::lock_guard<std::mutex> lock(g_lifecycleMutex);
            AdvanceHookLifecycleLocked();

            const bool openvrSettled =
                (g_state.openvrStatus == BackendStatus::ReadyVerified && g_state.readySignaled) ||
                (g_state.openvrStatus == BackendStatus::TerminalFailure && g_state.failedSignaled);
            const bool oculusSettled = (g_state.oculusStatus == BackendStatus::UnverifiedInvoked);
            const bool pinFailSettled = (g_state.pinFailedTerminal && g_state.failedSignaled);

            if (g_state.loadedSignaled &&
                (pinFailSettled ||
                 (g_state.openvrStatus == BackendStatus::TerminalFailure && g_state.failedSignaled) ||
                 (openvrSettled && oculusSettled))) {
                shouldExitLoop = true;
            } else if ((g_state.pinFailedTerminal ||
                        g_state.openvrStatus == BackendStatus::ReadyVerified ||
                        g_state.openvrStatus == BackendStatus::TerminalFailure) &&
                       ++postSettleSignalRetries >= 20) {
                shouldExitLoop = true;
            }
        }

        if (shouldExitLoop) {
            break;
        }

        Sleep(100);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl VRPacingHook_RefreshHooks() {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    AdvanceHookLifecycleLocked();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(module);
            HANDLE hThread = CreateThread(nullptr, 0, HookWatcherThread, nullptr, 0, nullptr);
            if (hThread) {
                CloseHandle(hThread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            // El modulo permanece fijado (GET_MODULE_HANDLE_EX_FLAG_PIN); no se adquiere
            // g_lifecycleMutex ni se cierran handles compartidos bajo el Loader Lock.
            g_stopWatcher.store(true, std::memory_order_relaxed);
            break;
        default:
            break;
    }
    return TRUE;
}

#endif