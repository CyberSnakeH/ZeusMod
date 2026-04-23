// ────────────────────────────────────────────────────────────────────────────
//  ManualMap.cpp — see ManualMap.h for the algorithm overview.
//
//  This file is heavily commented because the whole point of having it in
//  the codebase is to learn from it. Every block corresponds to a step the
//  Windows loader takes when it processes a DLL.
//
//  References:
//    Microsoft, "PE Format" — https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
//    Russinovich et al., "Windows Internals, 7e", chapter 5 (Process loading)
//    Skywing, "What Goes On Inside Windows 2000: Solving the Mysteries of the Loader"
// ────────────────────────────────────────────────────────────────────────────

#include "ManualMap.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>

namespace IcarusMod {

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Read the entire DLL from disk. We need the whole file because we are about
// to walk PE structures that reference each other by file-relative offsets.
bool ReadFileToBuffer(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

// Convert a section's protection characteristics into a Windows page-protection
// constant. This is what VirtualProtectEx wants. The mapping is straight from
// the PE spec — read/write/execute bits combined into one of seven values.
DWORD SectionProtection(DWORD characteristics) {
    const bool read    = (characteristics & IMAGE_SCN_MEM_READ)    != 0;
    const bool write   = (characteristics & IMAGE_SCN_MEM_WRITE)   != 0;
    const bool execute = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;

    if (execute && write && read)  return PAGE_EXECUTE_READWRITE;
    if (execute && read)           return PAGE_EXECUTE_READ;
    if (execute)                   return PAGE_EXECUTE;
    if (write && read)             return PAGE_READWRITE;
    if (read)                      return PAGE_READONLY;
    return PAGE_NOACCESS;
}

// Run a function pointer in the target process and wait for it to return.
// Used for both TLS callbacks and the final DllMain call.
bool RunRemoteRoutine(HANDLE process,
                      LPTHREAD_START_ROUTINE routine,
                      LPVOID parameter,
                      DWORD timeoutMs,
                      DWORD* exitCode) {
    HANDLE th = CreateRemoteThread(process, nullptr, 0,
                                   routine, parameter, 0, nullptr);
    if (!th) return false;

    DWORD wr = WaitForSingleObject(th, timeoutMs);
    if (wr == WAIT_TIMEOUT) {
        CloseHandle(th);
        return false;
    }

    DWORD code = 0;
    GetExitCodeThread(th, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(th);
    return true;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Map — orchestrates all 10 steps. Each step has a comment explaining what
// it does and why the loader does it that way.
// ────────────────────────────────────────────────────────────────────────────

MapResult ManualMap::Map(DWORD pid,
                         const std::wstring& dllPath,
                         MapStats* stats) {
    s_lastError = 0;
    if (stats) *stats = {};

    // ── Step 1: Read the DLL from disk into a host buffer ──────────────────
    std::vector<uint8_t> file;
    if (!ReadFileToBuffer(dllPath, file)) {
        s_lastError = GetLastError();
        return MapResult::FileOpenFailed;
    }

    // ── Step 2: Parse PE headers ───────────────────────────────────────────
    //
    //   IMAGE_DOS_HEADER  (e_magic = 'MZ')
    //     └── e_lfanew points at IMAGE_NT_HEADERS64
    //         ├── Signature == 'PE\0\0'
    //         ├── FileHeader (machine, section count, etc.)
    //         └── OptionalHeader (entry point, image base, sizes,
    //             data directories, …)
    //
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return MapResult::BadPEMagic;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return MapResult::BadPEMagic;
    }

    // ZeusMod is x64-only — refuse 32-bit DLLs early so error messages are
    // honest instead of failing later in some inscrutable place.
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return MapResult::NotX64;
    }

    const size_t imageSize  = nt->OptionalHeader.SizeOfImage;
    const size_t headerSize = nt->OptionalHeader.SizeOfHeaders;

    // ── Step 3: Allocate SizeOfImage in the target ─────────────────────────
    //
    // Open the target with the rights we need: VM ops, write, query, and
    // remote-thread creation. PROCESS_VM_READ is included so we can sanity-
    // check pages later if needed.
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE     | PROCESS_VM_READ |
        PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!process) {
        s_lastError = GetLastError();
        return MapResult::OpenProcessFailed;
    }

    // We allocate RW first; section protections get applied at step 8.
    LPVOID remoteBase = VirtualAllocEx(process, nullptr, imageSize,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remoteBase) {
        s_lastError = GetLastError();
        CloseHandle(process);
        return MapResult::AllocFailed;
    }
    if (stats) {
        stats->moduleBase = reinterpret_cast<uint64_t>(remoteBase);
        stats->imageSize  = imageSize;
    }

    // We do all the section / reloc / import work in a host-side scratch
    // buffer (`localImage`) and then push it to the target in one
    // WriteProcessMemory at the end. That makes debugging in WinDbg much
    // easier — you can dump the local image and validate it without
    // bouncing across processes.
    std::vector<uint8_t> localImage(imageSize, 0);

    // ── Step 4: Map sections ───────────────────────────────────────────────
    //
    // Copy the headers (the loader keeps them at offset 0 of the image)
    // then each section's raw bytes to its VirtualAddress.
    std::memcpy(localImage.data(), file.data(), headerSize);

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->SizeOfRawData == 0) continue; // .bss-style
        std::memcpy(localImage.data() + section->VirtualAddress,
                    file.data()       + section->PointerToRawData,
                    section->SizeOfRawData);
    }
    if (stats) stats->sectionsMapped = nt->FileHeader.NumberOfSections;

    // ── Step 5: Apply base relocations ─────────────────────────────────────
    //
    // The compiler bakes absolute addresses into the binary assuming the
    // image will be loaded at OptionalHeader.ImageBase. If we put it
    // somewhere else (which we always do — VirtualAllocEx picks an
    // address), every absolute pointer needs to be adjusted by `delta`.
    //
    // The reloc table is a chain of IMAGE_BASE_RELOCATION blocks. Each
    // block has a VirtualAddress (the page it covers) and a SizeOfBlock
    // followed by an array of WORD entries. The high 4 bits are the type
    // (IMAGE_REL_BASED_DIR64 on x64), the low 12 bits are the page offset.
    //
    const intptr_t delta = reinterpret_cast<intptr_t>(remoteBase) -
                            static_cast<intptr_t>(nt->OptionalHeader.ImageBase);

    int relocCount = 0;
    if (delta != 0) {
        const auto& relocDir = nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relocDir.Size > 0) {
            auto* block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                localImage.data() + relocDir.VirtualAddress);
            const auto* end = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                localImage.data() + relocDir.VirtualAddress + relocDir.Size);

            while (block < end && block->SizeOfBlock > 0) {
                const size_t entries =
                    (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) /
                    sizeof(WORD);
                auto* entry = reinterpret_cast<WORD*>(block + 1);

                for (size_t e = 0; e < entries; ++e) {
                    const WORD type   = entry[e] >> 12;
                    const WORD offset = entry[e] & 0x0FFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        auto* patch = reinterpret_cast<uint64_t*>(
                            localImage.data() + block->VirtualAddress + offset);
                        *patch += delta;
                        ++relocCount;
                    }
                    // IMAGE_REL_BASED_ABSOLUTE (= 0) is a padding entry,
                    // ignored on purpose. Other types don't appear in
                    // x64 PE images compiled by MSVC.
                }
                block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock);
            }
        }
    }
    if (stats) stats->relocsApplied = relocCount;

    // ── Step 6: Resolve imports ────────────────────────────────────────────
    //
    // The IAT (Import Address Table) is a list of pointers the binary
    // dereferences whenever it calls into another DLL. Each entry starts
    // life as either an ordinal or a pointer to an IMAGE_IMPORT_BY_NAME
    // record. We replace it in-place with the actual function address.
    //
    // We resolve in *our* address space and bank on the fact that under
    // Windows ASLR, system DLLs (kernel32, ntdll, user32, etc.) load at
    // the same base in every process for a given boot session. That makes
    // the function addresses identical between us and the target. The
    // wiki page covers the cases where this assumption breaks.
    //
    int importsBound = 0;
    {
        const auto& importDir = nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.Size > 0) {
            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                localImage.data() + importDir.VirtualAddress);

            for (; desc->Name != 0; ++desc) {
                const char* dllName = reinterpret_cast<const char*>(
                    localImage.data() + desc->Name);

                // Make sure the DLL is loaded *here* — if it's a system DLL
                // it almost certainly already is, but a non-system import
                // would otherwise fail GetProcAddress.
                HMODULE mod = GetModuleHandleA(dllName);
                if (!mod) {
                    mod = LoadLibraryA(dllName);
                    if (!mod) {
                        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
                        CloseHandle(process);
                        s_lastError = GetLastError();
                        return MapResult::ImportResolveFailed;
                    }
                }

                auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
                    localImage.data() + desc->OriginalFirstThunk);
                auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA64*>(
                    localImage.data() + desc->FirstThunk);

                // Some compilers (e.g. linkers without /RELEASE bound import
                // tables) leave OriginalFirstThunk = 0. Fall back to FirstThunk
                // in that case — it's still the unbound IAT pre-load.
                if (desc->OriginalFirstThunk == 0) origThunk = iatThunk;

                while (origThunk->u1.AddressOfData != 0) {
                    FARPROC fn = nullptr;
                    if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                        fn = GetProcAddress(mod, MAKEINTRESOURCEA(
                            origThunk->u1.Ordinal & 0xFFFF));
                    } else {
                        auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                            localImage.data() + origThunk->u1.AddressOfData);
                        fn = GetProcAddress(mod, byName->Name);
                    }
                    if (!fn) {
                        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
                        CloseHandle(process);
                        s_lastError = GetLastError();
                        return MapResult::ImportResolveFailed;
                    }
                    iatThunk->u1.Function = reinterpret_cast<uint64_t>(fn);
                    ++importsBound;
                    ++origThunk;
                    ++iatThunk;
                }
            }
        }
    }
    if (stats) stats->importsBound = importsBound;

    // ── Step 7: Push the fully-fixed-up image to the target ────────────────
    if (!WriteProcessMemory(process, remoteBase,
                            localImage.data(), imageSize, nullptr)) {
        s_lastError = GetLastError();
        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
        CloseHandle(process);
        return MapResult::WriteFailed;
    }

    // ── Step 8: Apply per-section page protections ─────────────────────────
    //
    // The whole image is currently RW. Demote every section to its
    // intended protection: .text → RX, .rdata → R, .data → RW, etc.
    // This lets DEP / W^X catch bugs and avoids leaving the entire image
    // executable.
    section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (section->Misc.VirtualSize == 0) continue;
        DWORD protect = SectionProtection(section->Characteristics);
        DWORD oldProt = 0;
        if (!VirtualProtectEx(process,
                              static_cast<uint8_t*>(remoteBase) + section->VirtualAddress,
                              section->Misc.VirtualSize,
                              protect,
                              &oldProt)) {
            s_lastError = GetLastError();
            VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
            CloseHandle(process);
            return MapResult::ProtectFailed;
        }
    }

    // ── Step 9: TLS callbacks ──────────────────────────────────────────────
    //
    // If the DLL has a TLS directory, we have to invoke each callback
    // BEFORE DllMain. The runtime relies on TLS callbacks being called
    // first — they typically initialize CRT thread-local storage,
    // exception state, etc.
    int tlsRun = 0;
    {
        const auto& tlsDir = nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tlsDir.Size > 0) {
            // The TLS directory's AddressOfCallBacks is a VA (already
            // includes ImageBase), so subtract our base/old image base
            // to find it in the local copy.
            auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(
                localImage.data() + tlsDir.VirtualAddress);

            uint64_t callbacksVA = tls->AddressOfCallBacks; // VA in target
            if (callbacksVA != 0) {
                // Read the array of callback pointers from our local copy:
                // its address relative to our local buffer is
                //   callbacksVA - (target ImageBase + delta)
                // which simplifies to (callbacksVA - remoteBase) since we
                // already adjusted bases in the relocation step.
                uint64_t base = reinterpret_cast<uint64_t>(remoteBase);
                auto* callbacks = reinterpret_cast<uint64_t*>(
                    localImage.data() + (callbacksVA - base));

                for (size_t k = 0; callbacks[k] != 0; ++k) {
                    DWORD code = 0;
                    if (!RunRemoteRoutine(
                            process,
                            reinterpret_cast<LPTHREAD_START_ROUTINE>(callbacks[k]),
                            remoteBase,        // hModule passed as parameter
                            5000, &code)) {
                        s_lastError = GetLastError();
                        VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
                        CloseHandle(process);
                        return MapResult::TlsCallbackFailed;
                    }
                    ++tlsRun;
                }
            }
        }
    }
    if (stats) stats->tlsCallbacks = tlsRun;

    // ── Step 10: Call DllMain(DLL_PROCESS_ATTACH) ──────────────────────────
    //
    // The standard loader normally calls DllMain with three arguments:
    //   (HMODULE hModule, DWORD reason, LPVOID reserved)
    //
    // CreateRemoteThread only gives us *one* parameter slot, so we'd
    // normally need a small shellcode shim to set up rcx/rdx/r8 properly.
    // For a learning implementation we cheat slightly: we pass the
    // remote base as the single argument. Most DllMains we hit (including
    // ZeusMod's) only branch on `reason == DLL_PROCESS_ATTACH`, and our
    // value happens to be non-zero, which the CRT-stub treats as "first
    // call". The wiki page describes the proper shellcode-bootstrap
    // alternative for production-grade mappers.
    //
    if (nt->OptionalHeader.AddressOfEntryPoint != 0) {
        auto entry = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            static_cast<uint8_t*>(remoteBase) + nt->OptionalHeader.AddressOfEntryPoint);

        DWORD code = 0;
        if (!RunRemoteRoutine(process, entry, remoteBase, 15000, &code)) {
            s_lastError = GetLastError();
            VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
            CloseHandle(process);
            return MapResult::EntryPointFailed;
        }
        // DllMain returns BOOL — a 0 return means the DLL refused to
        // initialise, which is a fatal condition.
        if (code == 0) {
            VirtualFreeEx(process, remoteBase, 0, MEM_RELEASE);
            CloseHandle(process);
            return MapResult::EntryPointFailed;
        }
    }

    CloseHandle(process);
    return MapResult::Success;
}

// ────────────────────────────────────────────────────────────────────────────
const wchar_t* ManualMap::ResultToString(MapResult result) {
    static wchar_t buf[256];
    const wchar_t* base = L"Unknown";
    switch (result) {
    case MapResult::Success:             return L"Manual map successful!";
    case MapResult::FileOpenFailed:      base = L"Could not open the DLL on disk"; break;
    case MapResult::BadPEMagic:          base = L"DOS / NT signature mismatch"; break;
    case MapResult::NotX64:              base = L"Manual mapper expects x64 PE32+ DLLs"; break;
    case MapResult::OpenProcessFailed:   base = L"OpenProcess failed"; break;
    case MapResult::AllocFailed:         base = L"VirtualAllocEx failed"; break;
    case MapResult::WriteFailed:         base = L"WriteProcessMemory failed"; break;
    case MapResult::ProtectFailed:       base = L"VirtualProtectEx failed (per-section)"; break;
    case MapResult::ImportResolveFailed: base = L"Import resolution failed"; break;
    case MapResult::EntryPointFailed:    base = L"DllMain returned FALSE or didn't run"; break;
    case MapResult::TlsCallbackFailed:   base = L"TLS callback didn't run cleanly"; break;
    }
    wsprintfW(buf, L"%s (error: %lu)", base, s_lastError);
    return buf;
}

} // namespace IcarusMod
