// ============================================================================
// Pure-Node DLL injector for ZeusMod.
//
// Replaces the old `inject.ps1` + `powershell.exe -Add-Type C#` pipeline
// with direct Win32 API calls through koffi (a modern FFI for Node). No
// subprocess, no PowerShell — the whole injection happens inside the
// Electron main process in a few milliseconds.
//
// Flow (standard CreateRemoteThread + LoadLibraryW technique):
//   1. OpenProcess (PROCESS_ALL_ACCESS) on the target PID.
//   2. VirtualAllocEx a small RW page inside the target.
//   3. WriteProcessMemory — write the wide-char DLL path into that page.
//   4. GetProcAddress(kernel32, "LoadLibraryW") — pointer valid cross-process
//      because kernel32 is loaded at the same base in every process on a
//      given boot (Windows ASLR per-boot, not per-process, for kernel32).
//   5. CreateRemoteThread(target, entry=LoadLibraryW, arg=our page).
//   6. WaitForSingleObject + GetExitCodeThread — exit code is the module
//      base address LoadLibraryW returned. Zero means it failed.
//   7. CloseHandle on everything.
//
// Fails cleanly on access denied, missing DLL, non-existent PID.
// ============================================================================

const koffi = require('koffi');

// ── Win32 type aliases ───────────────────────────────────────────────
const HANDLE   = 'void*';
const LPVOID   = 'void*';
const DWORD    = 'uint32';
const SIZE_T   = 'size_t';
const BOOL     = 'bool';

// Access rights bitmask we need on the target process.
const PROCESS_CREATE_THREAD     = 0x0002;
const PROCESS_QUERY_INFORMATION = 0x0400;
const PROCESS_VM_OPERATION      = 0x0008;
const PROCESS_VM_WRITE          = 0x0020;
const PROCESS_VM_READ           = 0x0010;
const PROCESS_INJECT_ACCESS     =
    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
    PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ;

const MEM_COMMIT     = 0x1000;
const MEM_RESERVE    = 0x2000;
const MEM_RELEASE    = 0x8000;
const PAGE_READWRITE = 0x04;

const INFINITE       = 0xFFFFFFFF;
const WAIT_OBJECT_0  = 0x00000000;

// ── FFI binding ──────────────────────────────────────────────────────
const kernel32 = koffi.load('kernel32.dll');

const OpenProcess = kernel32.func('__stdcall', 'OpenProcess',
    HANDLE, [DWORD, BOOL, DWORD]);
const CloseHandle = kernel32.func('__stdcall', 'CloseHandle',
    BOOL, [HANDLE]);
const GetLastError = kernel32.func('__stdcall', 'GetLastError',
    DWORD, []);

const VirtualAllocEx = kernel32.func('__stdcall', 'VirtualAllocEx',
    LPVOID, [HANDLE, LPVOID, SIZE_T, DWORD, DWORD]);
const VirtualFreeEx = kernel32.func('__stdcall', 'VirtualFreeEx',
    BOOL, [HANDLE, LPVOID, SIZE_T, DWORD]);

const WriteProcessMemory = kernel32.func('__stdcall', 'WriteProcessMemory',
    BOOL, [HANDLE, LPVOID, 'const void*', SIZE_T, koffi.out(koffi.pointer(SIZE_T))]);

const CreateRemoteThread = kernel32.func('__stdcall', 'CreateRemoteThread',
    HANDLE, [HANDLE, LPVOID, SIZE_T, LPVOID, LPVOID, DWORD, koffi.out(koffi.pointer(DWORD))]);

const WaitForSingleObject = kernel32.func('__stdcall', 'WaitForSingleObject',
    DWORD, [HANDLE, DWORD]);
const GetExitCodeThread = kernel32.func('__stdcall', 'GetExitCodeThread',
    BOOL, [HANDLE, koffi.out(koffi.pointer(DWORD))]);

const GetModuleHandleW = kernel32.func('__stdcall', 'GetModuleHandleW',
    HANDLE, ['str16']);
const GetProcAddress = kernel32.func('__stdcall', 'GetProcAddress',
    LPVOID, [HANDLE, 'str']);

// ── Helpers ──────────────────────────────────────────────────────────

class InjectError extends Error {
    constructor(message, code) {
        super(message);
        this.name = 'InjectError';
        this.code = code;
    }
}

function win32Error(ctx) {
    return new InjectError(`${ctx} (Win32 err ${GetLastError()})`, 'WIN32');
}

/**
 * Inject `dllPath` into process `pid`. Returns { moduleBase: bigint-ish }.
 * Throws InjectError on any failure. Idempotent: calling twice on the
 * same process simply LoadLibraryW's again, which reuses the loaded DLL
 * (LoadLibraryW is reference-counted so this is safe).
 */
function injectDll(pid, dllPath) {
    if (!Number.isInteger(pid) || pid <= 0) {
        throw new InjectError(`Invalid PID ${pid}`, 'BAD_PID');
    }

    // 1. Open the target process with full-ish rights.
    const hProcess = OpenProcess(PROCESS_INJECT_ACCESS, false, pid);
    if (!hProcess || Number(koffi.address(hProcess)) === 0) {
        throw win32Error(`OpenProcess(pid=${pid})`);
    }

    // Remote page we allocate — keep reference so we can free it later.
    let remoteArg = null;
    let hThread = null;

    try {
        // 2. Allocate a page big enough for the NUL-terminated UTF-16 path.
        //    Each UTF-16 code unit = 2 bytes, plus 2 bytes for the NUL.
        const pathBytes = Buffer.from(dllPath + '\0', 'utf16le');
        remoteArg = VirtualAllocEx(hProcess, null, pathBytes.length,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remoteArg || Number(koffi.address(remoteArg)) === 0) {
            throw win32Error('VirtualAllocEx');
        }

        // 3. Copy the path into the remote page.
        const written = [0];
        if (!WriteProcessMemory(hProcess, remoteArg, pathBytes, pathBytes.length, written)) {
            throw win32Error('WriteProcessMemory');
        }
        if (written[0] !== pathBytes.length) {
            throw new InjectError(
                `WriteProcessMemory truncated (${written[0]}/${pathBytes.length})`,
                'TRUNCATED');
        }

        // 4. Resolve LoadLibraryW — kernel32 is at the same base in every
        //    process of the same boot, so the pointer we get in OUR address
        //    space is equally valid as the remote thread's entry point.
        const hKernel32 = GetModuleHandleW('kernel32.dll');
        if (!hKernel32) throw win32Error('GetModuleHandleW(kernel32)');
        const loadLib = GetProcAddress(hKernel32, 'LoadLibraryW');
        if (!loadLib) throw win32Error('GetProcAddress(LoadLibraryW)');

        // 5. Spawn a remote thread whose entry IS LoadLibraryW, with our
        //    page as the lpParameter — Windows calls LoadLibraryW(ourPath).
        const threadIdBox = [0];
        hThread = CreateRemoteThread(hProcess, null, 0, loadLib, remoteArg, 0, threadIdBox);
        if (!hThread || Number(koffi.address(hThread)) === 0) {
            throw win32Error('CreateRemoteThread');
        }

        // 6. Wait for LoadLibraryW to return. 10s cap — DllMain attaches
        //    in our DLL start MinHook + resolvers etc., typically well
        //    under a second, but we allow headroom for slow disks.
        const waitResult = WaitForSingleObject(hThread, 10_000);
        if (waitResult !== WAIT_OBJECT_0) {
            throw new InjectError(`WaitForSingleObject = 0x${waitResult.toString(16)}`, 'TIMEOUT');
        }

        const exitCodeBox = [0];
        if (!GetExitCodeThread(hThread, exitCodeBox)) {
            throw win32Error('GetExitCodeThread');
        }
        // Exit code IS the 32-bit-truncated module base. Zero means
        // LoadLibraryW returned NULL (DLL failed to load — bad arch,
        // missing dependencies, DllMain returned FALSE).
        if (exitCodeBox[0] === 0) {
            throw new InjectError(
                `LoadLibraryW returned NULL in target process — DLL rejected (check arch x64, dependencies, and DllMain)`,
                'LOAD_FAILED');
        }
        return { success: true, moduleBase: exitCodeBox[0], pid };
    }
    finally {
        // 7. Best-effort cleanup. VirtualFreeEx with MEM_RELEASE is
        // fine even if LoadLibraryW already copied the path internally.
        if (hThread) CloseHandle(hThread);
        if (remoteArg) {
            try { VirtualFreeEx(hProcess, remoteArg, 0, MEM_RELEASE); } catch {}
        }
        CloseHandle(hProcess);
    }
}

module.exports = { injectDll, InjectError };
