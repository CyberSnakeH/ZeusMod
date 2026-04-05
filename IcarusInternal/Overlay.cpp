// ============================================================================
// Overlay Window - Transparent always-on-top Win32 window
// No DX hooks, no crashes, works with any rendering API
// ============================================================================
#include "Overlay.h"
#include "Trainer.h"
#include <cstdio>

#pragma comment(lib, "gdi32.lib")

static HWND g_overlay = nullptr;
static bool g_visible = false;
static HWND g_gameWindow = nullptr;

// Control IDs
static constexpr int ID_CHK_GODMODE = 1001;
static constexpr int ID_CHK_STAMINA = 1002;
static constexpr int ID_CHK_ARMOR   = 1003;
static constexpr int ID_CHK_OXYGEN  = 1004;
static constexpr int ID_CHK_FOOD    = 1005;
static constexpr int ID_CHK_WATER   = 1006;
static constexpr int ID_CHK_SPEED   = 1007;
static constexpr int ID_CHK_CRAFT   = 1008;
static constexpr int ID_BTN_SPEED_DOWN = 1009;
static constexpr int ID_BTN_SPEED_UP   = 1012;
static constexpr int ID_LBL_STATUS  = 1010;
static constexpr int ID_LBL_SPEED   = 1011;

static HBRUSH g_bgBrush = nullptr;
static HFONT g_font = nullptr;
static HFONT g_fontBold = nullptr;

static constexpr COLORREF CLR_BG    = RGB(20, 20, 28);
static constexpr COLORREF CLR_TEXT  = RGB(220, 220, 230);
static constexpr COLORREF CLR_GREEN = RGB(80, 220, 100);
static constexpr COLORREF CLR_RED   = RGB(220, 80, 80);

// Find the game window
static HWND FindGameWindow() {
    return FindWindowW(nullptr, L"Icarus  ");  // Icarus window title (may have trailing spaces)
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        wchar_t title[256];
        GetWindowTextW(hwnd, title, 256);
        if (wcsstr(title, L"Icarus") && hwnd != g_overlay) {
            *(HWND*)lParam = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindGameWindowByPID() {
    HWND result = nullptr;
    EnumWindows(EnumWindowsProc, (LPARAM)&result);
    return result;
}

// ============================================================================
// WndProc
// ============================================================================
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            auto& t = Trainer::Get();
            switch (LOWORD(wParam)) {
            case ID_CHK_GODMODE:
                t.GodMode = (SendDlgItemMessageW(hwnd, ID_CHK_GODMODE, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] God Mode: %s\n", t.GodMode ? "ON" : "OFF");
                break;
            case ID_CHK_STAMINA:
                t.InfiniteStamina = (SendDlgItemMessageW(hwnd, ID_CHK_STAMINA, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Infinite Stamina: %s\n", t.InfiniteStamina ? "ON" : "OFF");
                break;
            case ID_CHK_ARMOR:
                t.InfiniteArmor = (SendDlgItemMessageW(hwnd, ID_CHK_ARMOR, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Infinite Armor: %s\n", t.InfiniteArmor ? "ON" : "OFF");
                break;
            case ID_CHK_OXYGEN:
                t.InfiniteOxygen = (SendDlgItemMessageW(hwnd, ID_CHK_OXYGEN, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Infinite Oxygen: %s\n", t.InfiniteOxygen ? "ON" : "OFF");
                break;
            case ID_CHK_FOOD:
                t.InfiniteFood = (SendDlgItemMessageW(hwnd, ID_CHK_FOOD, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Infinite Food: %s\n", t.InfiniteFood ? "ON" : "OFF");
                break;
            case ID_CHK_WATER:
                t.InfiniteWater = (SendDlgItemMessageW(hwnd, ID_CHK_WATER, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Infinite Water: %s\n", t.InfiniteWater ? "ON" : "OFF");
                break;
            case ID_CHK_SPEED:
                t.SpeedHack = (SendDlgItemMessageW(hwnd, ID_CHK_SPEED, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (!t.SpeedHack) t.m_originalWalkSpeed = 0; // Reset on disable so it re-reads
                printf("[TOGGLE] Speed Hack: %s (x%.1f)\n", t.SpeedHack ? "ON" : "OFF", t.SpeedMultiplier);
                break;
            case ID_BTN_SPEED_DOWN:
                t.SpeedMultiplier -= 0.5f;
                if (t.SpeedMultiplier < 0.5f) t.SpeedMultiplier = 0.5f;
                { wchar_t b[32]; swprintf(b, 32, L"x%.1f", t.SpeedMultiplier);
                  SetDlgItemTextW(hwnd, ID_LBL_SPEED, b); }
                printf("[SPEED] Multiplier: x%.1f\n", t.SpeedMultiplier);
                break;
            case ID_BTN_SPEED_UP:
                t.SpeedMultiplier += 0.5f;
                if (t.SpeedMultiplier > 10.0f) t.SpeedMultiplier = 10.0f;
                { wchar_t b[32]; swprintf(b, 32, L"x%.1f", t.SpeedMultiplier);
                  SetDlgItemTextW(hwnd, ID_LBL_SPEED, b); }
                printf("[SPEED] Multiplier: x%.1f\n", t.SpeedMultiplier);
                break;
            case ID_CHK_CRAFT:
                t.FreeCraft = (SendDlgItemMessageW(hwnd, ID_CHK_CRAFT, BM_GETCHECK, 0, 0) == BST_CHECKED);
                printf("[TOGGLE] Free Craft: %s\n", t.FreeCraft ? "ON" : "OFF");
                break;
            }
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, CLR_BG);
        SetTextColor(hdc, CLR_TEXT);
        return (LRESULT)g_bgBrush;
    }

    case WM_CLOSE:
        Overlay::Toggle(); // Hide instead of close
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // Draw title bar accent
        RECT rc = {0, 0, 300, 3};
        HBRUSH accent = CreateSolidBrush(RGB(100, 140, 255));
        FillRect(hdc, &rc, accent);
        DeleteObject(accent);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Create overlay
// ============================================================================
bool Overlay::Create() {
    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"IcarusModOverlay";
    RegisterClassExW(&wc);

    // Position: top-right corner
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int winW = 310, winH = 430;

    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"IcarusModOverlay", L"IcarusMod",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        screenW - winW - 20, 50, winW, winH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_overlay) return false;

    int y = 10;

    // Title
    HWND title = CreateWindowExW(0, L"STATIC", L"IcarusMod Trainer",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, y, winW, 22, g_overlay, nullptr, nullptr, nullptr);
    SendMessageW(title, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    y += 28;

    // Status
    HWND status = CreateWindowExW(0, L"STATIC", L"Searching...",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, y, winW, 18, g_overlay, (HMENU)(INT_PTR)ID_LBL_STATUS, nullptr, nullptr);
    SendMessageW(status, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 28;

    // Separator
    CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        10, y, winW - 20, 2, g_overlay, nullptr, nullptr, nullptr);
    y += 10;

    // Checkboxes
    HWND chk1 = CreateWindowExW(0, L"BUTTON", L"God Mode (Health + No Debuffs)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_GODMODE, nullptr, nullptr);
    SendMessageW(chk1, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk2 = CreateWindowExW(0, L"BUTTON", L"Infinite Stamina",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_STAMINA, nullptr, nullptr);
    SendMessageW(chk2, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk3 = CreateWindowExW(0, L"BUTTON", L"Infinite Armor",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_ARMOR, nullptr, nullptr);
    SendMessageW(chk3, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk4 = CreateWindowExW(0, L"BUTTON", L"Infinite Oxygen",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_OXYGEN, nullptr, nullptr);
    SendMessageW(chk4, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk5 = CreateWindowExW(0, L"BUTTON", L"Infinite Food",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_FOOD, nullptr, nullptr);
    SendMessageW(chk5, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk6 = CreateWindowExW(0, L"BUTTON", L"Infinite Water",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_WATER, nullptr, nullptr);
    SendMessageW(chk6, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 26;

    HWND chk7 = CreateWindowExW(0, L"BUTTON", L"Speed Hack",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, 120, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_SPEED, nullptr, nullptr);
    SendMessageW(chk7, WM_SETFONT, (WPARAM)g_font, TRUE);

    // Speed - / label / + buttons
    HWND btnDown = CreateWindowExW(0, L"BUTTON", L"-",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        140, y, 30, 22, g_overlay, (HMENU)(INT_PTR)ID_BTN_SPEED_DOWN, nullptr, nullptr);
    SendMessageW(btnDown, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    HWND lblSpd = CreateWindowExW(0, L"STATIC", L"x2.0",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        172, y, 50, 22, g_overlay, (HMENU)(INT_PTR)ID_LBL_SPEED, nullptr, nullptr);
    SendMessageW(lblSpd, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

    HWND btnUp = CreateWindowExW(0, L"BUTTON", L"+",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        224, y, 30, 22, g_overlay, (HMENU)(INT_PTR)ID_BTN_SPEED_UP, nullptr, nullptr);
    SendMessageW(btnUp, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    y += 26;

    HWND chk8 = CreateWindowExW(0, L"BUTTON", L"Free Craft (no resources needed)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        15, y, winW - 30, 22, g_overlay, (HMENU)(INT_PTR)ID_CHK_CRAFT, nullptr, nullptr);
    SendMessageW(chk8, WM_SETFONT, (WPARAM)g_font, TRUE);
    y += 30;

    // Info
    HWND info = CreateWindowExW(0, L"STATIC", L"Press N to show/hide | F10 to detach",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, y, winW, 18, g_overlay, nullptr, nullptr, nullptr);
    SendMessageW(info, WM_SETFONT, (WPARAM)g_font, TRUE);

    ShowWindow(g_overlay, SW_SHOW);
    UpdateWindow(g_overlay);
    g_visible = true;

    printf("[OVERLAY] Window created.\n");
    return true;
}

void Overlay::Destroy() {
    if (g_overlay) {
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
    }
    UnregisterClassW(L"IcarusModOverlay", GetModuleHandleW(nullptr));
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
}

void Overlay::ProcessMessages() {
    MSG msg;
    while (PeekMessageW(&msg, g_overlay, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Update status label
    if (g_overlay) {
        auto& t = Trainer::Get();
        HWND lbl = GetDlgItem(g_overlay, ID_LBL_STATUS);
        if (lbl) {
            if (t.IsReady()) {
                wchar_t buf[128];
                wsprintfW(buf, L"HP: %d/%d | STA: %d/%d | ARM: %d/%d",
                    t.GetHealth(), t.GetMaxHealth(),
                    t.GetStamina(), t.GetMaxStamina(),
                    t.GetArmor(), t.GetMaxArmor());
                SetWindowTextW(lbl, buf);
            } else {
                SetWindowTextW(lbl, L"Searching for player...");
            }
        }
    }
}

bool Overlay::IsVisible() { return g_visible; }

void Overlay::Toggle() {
    g_visible = !g_visible;
    ShowWindow(g_overlay, g_visible ? SW_SHOW : SW_HIDE);
    printf("[OVERLAY] %s\n", g_visible ? "SHOWN" : "HIDDEN");
}
