#include <Windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include "GUI.h"
#include "ProcessUtils.h"
#include "Injector.h"
#include "ManualMap.h"
#include "SharedTypes.h"

using namespace IcarusMod;

static GUI g_gui;
static bool g_injected = false;
static DWORD g_gamePid = 0;

// Set by parsing CLI args in wWinMain. When true, DoAttach runs the
// pedagogical manual mapper (ManualMap::Map) instead of the standard
// LoadLibraryW path (Injector::Inject). Default is false — the standard
// path is faster, more reliable, and what the desktop launcher uses too.
static bool g_useManualMap = false;

static void DoAttach() {
    g_gui.SetStatusText(g_useManualMap
        ? L"Searching for Icarus (manual-map mode)..."
        : L"Searching for Icarus...");

    ProcessUtils::EnableDebugPrivilege();
    g_gamePid = ProcessUtils::FindProcessByName(TARGET_PROCESS);
    if (!g_gamePid) {
        g_gui.SetStatusText(L"Icarus not found! Launch the game first.");
        return;
    }

    wchar_t buf[256];
    wsprintfW(buf, L"Found Icarus (PID: %lu). %s ...", g_gamePid,
              g_useManualMap ? L"Manual-mapping" : L"Injecting");
    g_gui.SetStatusText(buf);

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dllPath(exePath);
    auto pos = dllPath.find_last_of(L'\\');
    if (pos != std::wstring::npos) dllPath = dllPath.substr(0, pos + 1);
    dllPath += L"IcarusInternal.dll";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_gui.SetStatusText(L"ERROR: IcarusInternal.dll not found!");
        return;
    }

    if (g_useManualMap) {
        ManualMap::MapStats stats{};
        auto result = ManualMap::Map(g_gamePid, dllPath, &stats);
        if (result == MapResult::Success) {
            wsprintfW(buf,
                L"Mapped @0x%llX  size=0x%zX  sections=%d  relocs=%d  imports=%d  tls=%d",
                stats.moduleBase, stats.imageSize,
                stats.sectionsMapped, stats.relocsApplied,
                stats.importsBound,  stats.tlsCallbacks);
            g_gui.SetStatusText(buf);
            g_gui.SetConnectionStatus(true);
            g_injected = true;
        } else {
            g_gui.SetStatusText(ManualMap::ResultToString(result));
        }
    } else {
        auto result = Injector::Inject(g_gamePid, dllPath);
        if (result == InjectionResult::Success) {
            g_gui.SetStatusText(L"Injected! Use overlay (N) to toggle cheats.");
            g_gui.SetConnectionStatus(true);
            g_injected = true;
        } else {
            g_gui.SetStatusText(Injector::ResultToString(result));
        }
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Parse CLI: --manual selects the manual-map path; everything else
    // keeps the standard LoadLibraryW behaviour. Argv lookup goes through
    // CommandLineToArgvW since this is a /SUBSYSTEM:WINDOWS app.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--manual") == 0 ||
                lstrcmpiW(argv[i], L"-m")       == 0) {
                g_useManualMap = true;
            }
        }
        LocalFree(argv);
    }

    if (!g_gui.Create(hInstance)) return 1;
    g_gui.Show();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_USER + 1 && !g_injected) {
            DoAttach();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);

        if (g_injected && g_gamePid && !ProcessUtils::IsProcessRunning(g_gamePid)) {
            g_injected = false; g_gamePid = 0;
            g_gui.SetConnectionStatus(false);
            g_gui.SetStatusText(L"Game closed. Click Attach.");
        }
    }
    return 0;
}
