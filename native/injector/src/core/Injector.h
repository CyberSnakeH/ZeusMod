#pragma once
#include <Windows.h>
#include <string>

namespace IcarusMod {

enum class InjectionResult {
    Success,
    ProcessNotFound,
    OpenProcessFailed,
    AllocFailed,
    WriteFailed,
    ThreadCreationFailed,
    LoadLibraryFailed,
};

class Injector {
public:
    static InjectionResult Inject(DWORD pid, const std::wstring& dllPath);
    static const wchar_t* ResultToString(InjectionResult result);
    static DWORD GetLastInjectionError() { return s_lastError; }

private:
    static inline DWORD s_lastError = 0;
};

} // namespace IcarusMod
