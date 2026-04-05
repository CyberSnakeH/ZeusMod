#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <cstdint>

namespace IcarusMod {

class ProcessUtils {
public:
    static DWORD FindProcessByName(const wchar_t* processName);
    static bool IsProcessRunning(DWORD pid);
    static bool EnableDebugPrivilege();
    static std::wstring GetProcessPath(DWORD pid);
};

} // namespace IcarusMod
