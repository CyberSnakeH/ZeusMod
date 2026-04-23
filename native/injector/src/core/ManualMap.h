// ────────────────────────────────────────────────────────────────────────────
//  ManualMap — pedagogical PE manual-mapper for the ZeusMod project.
//
//  This is a *learning* implementation of what the Windows loader does when
//  you call LoadLibraryW. We re-do every step by hand against a remote
//  process so the result is a fully resolved DLL in the target's address
//  space without that DLL being registered in the loader's module list
//  (PEB->Ldr, EnumProcessModules, etc.).
//
//  In the ZeusMod codebase the standard LoadLibraryW path (Injector::Inject)
//  remains the default. ManualMap is an opt-in alternative behind the
//  --manual CLI flag. There are no detection-evasion features layered on
//  top — the only "stealth" property is a side-effect of bypassing the
//  loader, and the goal is to understand the PE format and Windows loader
//  internals, not to defeat any anti-cheat.
//
//  Steps performed (mirrors Windows Internals chapter 5, the loader):
//    1. Read the DLL from disk into a host buffer.
//    2. Parse PE headers (DOS, NT, optional, sections).
//    3. Allocate SizeOfImage in the target process.
//    4. Map sections to their VirtualAddress (host-side for now).
//    5. Apply base relocations against the chosen target base.
//    6. Resolve imports — assumes system DLLs share addresses with the
//       injector (true under per-boot ASLR for kernel32/ntdll/etc.).
//    7. Push the fully-fixed-up image into the target via WriteProcessMemory.
//    8. Apply per-section page protections via VirtualProtectEx.
//    9. Invoke each TLS callback in the target.
//   10. Invoke the DLL entry point (DllMain) with DLL_PROCESS_ATTACH.
//
//  Caveats clearly explained in the wiki page docs/wiki/Manual-Mapping.md.
// ────────────────────────────────────────────────────────────────────────────

#pragma once

#include <Windows.h>
#include <string>

namespace IcarusMod {

enum class MapResult {
    Success,
    FileOpenFailed,        // CreateFileW / ReadFile against the DLL on disk
    BadPEMagic,            // DOS / NT signature mismatch
    NotX64,                // We only handle PE32+ (IMAGE_FILE_MACHINE_AMD64)
    OpenProcessFailed,
    AllocFailed,           // VirtualAllocEx in the target
    WriteFailed,           // WriteProcessMemory
    ProtectFailed,         // VirtualProtectEx
    ImportResolveFailed,   // Local GetProcAddress or LoadLibrary failed
    EntryPointFailed,      // DllMain remote thread didn't return TRUE
    TlsCallbackFailed,     // A TLS callback's remote thread didn't return cleanly
};

struct MapStats {
    uint64_t   moduleBase     = 0;   // Final remote base address
    size_t     imageSize      = 0;   // SizeOfImage
    int        sectionsMapped = 0;
    int        relocsApplied  = 0;
    int        importsBound   = 0;
    int        tlsCallbacks   = 0;
};

class ManualMap {
public:
    // Manual-map dllPath into the process identified by pid.
    // On Success, stats->moduleBase is the remote base address.
    static MapResult Map(DWORD pid,
                         const std::wstring& dllPath,
                         MapStats* stats = nullptr);

    static const wchar_t* ResultToString(MapResult result);
    static DWORD GetLastWin32Error() { return s_lastError; }

private:
    static inline DWORD s_lastError = 0;
};

} // namespace IcarusMod
