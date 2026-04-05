#include <Windows.h>
#include "GUI.h"
#include "ProcessUtils.h"
#include "Injector.h"
#include "../Shared/SharedTypes.h"

using namespace IcarusMod;

static GUI g_gui;
static bool g_injected = false;
static DWORD g_gamePid = 0;

static void DoAttach() {
    g_gui.SetStatusText(L"Searching for Icarus...");
    g_gui.SetAttachEnabled(false);

    ProcessUtils::EnableDebugPrivilege();
    g_gamePid = ProcessUtils::FindProcessByName(TARGET_PROCESS);
    if (!g_gamePid) {
        g_gui.SetStatusText(L"Icarus not found! Launch the game first.");
        g_gui.SetAttachEnabled(true);
        return;
    }

    wchar_t buf[256];
    wsprintfW(buf, L"Found Icarus (PID: %lu). Injecting...", g_gamePid);
    g_gui.SetStatusText(buf);

    // Get DLL path (IcarusInternal.dll next to injector)
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dllPath(exePath);
    auto pos = dllPath.find_last_of(L'\\');
    if (pos != std::wstring::npos) dllPath = dllPath.substr(0, pos + 1);
    dllPath += L"IcarusInternal.dll";

    // Check DLL exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_gui.SetStatusText(L"ERROR: IcarusInternal.dll not found!");
        g_gui.SetAttachEnabled(true);
        return;
    }

    auto result = Injector::Inject(g_gamePid, dllPath);
    if (result == InjectionResult::Success) {
        g_gui.SetStatusText(L"Injected! Use F1-F3 in game. F10=Detach.");
        g_injected = true;
    } else {
        wsprintfW(buf, L"Injection failed: %s", Injector::ResultToString(result));
        g_gui.SetStatusText(buf);
        g_gui.SetAttachEnabled(true);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
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
            g_gui.SetStatusText(L"Game closed. Click Attach.");
            g_gui.SetAttachEnabled(true);
        }
    }
    return 0;
}
