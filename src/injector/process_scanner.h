#pragma once

#ifdef _WIN32

#include <string>

namespace vrpacing {

class ProcessScanner {
public:
    bool InjectHookDll(const std::wstring& processName, const std::wstring& dllPath);
    void PollAndInject(const std::wstring& dllPath);
};

} // namespace vrpacing

#endif
