param(
    [Parameter(Mandatory = $true)][string]$ProcessName,
    [Parameter(Mandatory = $true)][string]$DllPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $DllPath)) {
    Write-Output "ERROR:DLL_NOT_FOUND"
    exit 2
}

$target = Get-Process | Where-Object { $_.ProcessName -ieq [System.IO.Path]::GetFileNameWithoutExtension($ProcessName) } | Select-Object -First 1
if (-not $target) {
    Write-Output "ERROR:PROCESS_NOT_FOUND"
    exit 3
}

$source = @"
using System;
using System.Runtime.InteropServices;

public static class RemoteInjector {
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr OpenProcess(UInt32 dwDesiredAccess, bool bInheritHandle, UInt32 dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, UIntPtr dwSize, UInt32 flAllocationType, UInt32 flProtect);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, UInt32 nSize, out UIntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, UIntPtr dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, UInt32 dwCreationFlags, out UInt32 lpThreadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern UInt32 WaitForSingleObject(IntPtr hHandle, UInt32 dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeThread(IntPtr hThread, out UInt32 lpExitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    public static string Inject(UInt32 pid, string dllPath) {
        const UInt32 PROCESS_ALL = 0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400;
        const UInt32 MEM_COMMIT = 0x1000;
        const UInt32 MEM_RESERVE = 0x2000;
        const UInt32 PAGE_READWRITE = 0x04;

        IntPtr hProcess = OpenProcess(PROCESS_ALL, false, pid);
        if (hProcess == IntPtr.Zero) return "OPEN_PROCESS";

        byte[] bytes = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
        IntPtr remoteMem = VirtualAllocEx(hProcess, IntPtr.Zero, (UIntPtr)bytes.Length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remoteMem == IntPtr.Zero) {
            CloseHandle(hProcess);
            return "ALLOC";
        }

        UIntPtr written;
        if (!WriteProcessMemory(hProcess, remoteMem, bytes, (UInt32)bytes.Length, out written)) {
            CloseHandle(hProcess);
            return "WRITE";
        }

        IntPtr kernel32 = GetModuleHandle("kernel32.dll");
        IntPtr loadLibrary = GetProcAddress(kernel32, "LoadLibraryW");
        if (loadLibrary == IntPtr.Zero) {
            CloseHandle(hProcess);
            return "LOADLIB_ADDR";
        }

        UInt32 threadId;
        IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, UIntPtr.Zero, loadLibrary, remoteMem, 0, out threadId);
        if (hThread == IntPtr.Zero) {
            CloseHandle(hProcess);
            return "THREAD";
        }

        WaitForSingleObject(hThread, 15000);
        UInt32 exitCode;
        GetExitCodeThread(hThread, out exitCode);
        CloseHandle(hThread);
        CloseHandle(hProcess);

        return exitCode == 0 ? "LOADLIB_FAIL" : "OK";
    }
}
"@

Add-Type -TypeDefinition $source -Language CSharp
$result = [RemoteInjector]::Inject([uint32]$target.Id, $DllPath)

if ($result -eq 'OK') {
    Write-Output "OK:$($target.Id)"
    exit 0
}

Write-Output "ERROR:$result"
exit 1
