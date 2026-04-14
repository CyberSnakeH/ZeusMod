#include <Windows.h>
#include "Trainer.h"
#include "Overlay.h"
#include "Render.h"

static HMODULE g_module = nullptr;
static bool g_running = true;
static bool g_usingLegacyOverlay = false;

static DWORD WINAPI GodModeThread(LPVOID) {
    while (g_running) {
        auto& t = Trainer::Get();
        if (t.GodMode && t.IsReady()) {
            // Tight loop — no sleep, just yield
            for (int i = 0; i < 100 && t.GodMode; i++) {
                t.TickGodModefast();
            }
            Sleep(0); // Yield to other threads
        } else {
            Sleep(10);
        }
    }
    return 0;
}

static DWORD WINAPI MainThread(LPVOID) {
    Sleep(5000);

    Trainer::Get().Initialize();
    if (!Render::Initialize()) {
        printf("[RENDER] Falling back to legacy overlay.\n");
        g_usingLegacyOverlay = Overlay::Create();
    }

    // Start dedicated god mode thread
    CreateThread(nullptr, 0, GodModeThread, nullptr, 0, nullptr);

    while (g_running) {
        if (g_usingLegacyOverlay) {
            Overlay::ProcessMessages();

            static bool nWas = false;
            bool nNow = (GetAsyncKeyState(0x4E) & 0x8000) != 0;
            if (nNow && !nWas) Overlay::Toggle();
            nWas = nNow;
        }

        if (GetAsyncKeyState(VK_F10) & 1) g_running = false;

        Trainer::Get().Tick();

        Sleep(30);
    }

    if (g_usingLegacyOverlay) {
        Overlay::Destroy();
    } else {
        Render::Shutdown();
    }
    Trainer::Get().Shutdown();
    Sleep(500);
    FreeLibraryAndExitThread(g_module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
