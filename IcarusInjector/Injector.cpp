#include "Injector.h"
#include <cstdio>

namespace IcarusMod {

InjectionResult Injector::Inject(DWORD pid, const std::wstring& dllPath) {
    // Verify DLL exists
    DWORD attrs = GetFileAttributesW(dllPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return InjectionResult::LoadLibraryFailed; // DLL file not found
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid
    );
    if (!process) {
        s_lastError = GetLastError();
        return InjectionResult::OpenProcessFailed;
    }

    size_t pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(process, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        s_lastError = GetLastError();
        CloseHandle(process);
        return InjectionResult::AllocFailed;
    }

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), pathSize, nullptr)) {
        s_lastError = GetLastError();
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return InjectionResult::WriteFailed;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW")
    );

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibAddr, remoteMem, 0, nullptr);
    if (!thread) {
        s_lastError = GetLastError();
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        CloseHandle(process);
        return InjectionResult::ThreadCreationFailed;
    }

    DWORD waitResult = WaitForSingleObject(thread, 15000);

    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    CloseHandle(process);

    if (waitResult == WAIT_TIMEOUT) {
        s_lastError = WAIT_TIMEOUT;
        return InjectionResult::ThreadCreationFailed;
    }

    // exitCode is the HMODULE returned by LoadLibraryW (0 = failed)
    if (exitCode == 0) {
        s_lastError = 0;
        return InjectionResult::LoadLibraryFailed;
    }

    return InjectionResult::Success;
}

const wchar_t* Injector::ResultToString(InjectionResult result) {
    static wchar_t buf[256];
    const wchar_t* base = L"Unknown";
    switch (result) {
    case InjectionResult::Success:              return L"Injection successful!";
    case InjectionResult::ProcessNotFound:      base = L"Process not found"; break;
    case InjectionResult::OpenProcessFailed:    base = L"Failed to open process"; break;
    case InjectionResult::AllocFailed:          base = L"Failed to allocate memory"; break;
    case InjectionResult::WriteFailed:          base = L"Failed to write memory"; break;
    case InjectionResult::ThreadCreationFailed: base = L"Remote thread failed"; break;
    case InjectionResult::LoadLibraryFailed:    base = L"LoadLibrary failed (DLL not found or blocked)"; break;
    }
    wsprintfW(buf, L"%s (error: %lu)", base, s_lastError);
    return buf;
}

} // namespace IcarusMod
