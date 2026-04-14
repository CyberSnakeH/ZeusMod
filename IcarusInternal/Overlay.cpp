// ============================================================================
// ZeusMod In-Game Overlay - Gaming/Neon Compact UI
// Custom GDI drawing, always-on-top, toggleable with N key
// ============================================================================
#include "Overlay.h"
#include "Trainer.h"
#include <cstdio>
#include <cmath>

#pragma comment(lib, "gdi32.lib")

// ============================================================================
// Neon Colors
// ============================================================================
namespace C {
    constexpr COLORREF BG       = RGB(8, 8, 16);
    constexpr COLORREF Panel    = RGB(16, 20, 32);
    constexpr COLORREF Surface  = RGB(24, 28, 44);
    constexpr COLORREF Cyan     = RGB(0, 200, 255);
    constexpr COLORREF Purple   = RGB(140, 80, 255);
    constexpr COLORREF Green    = RGB(0, 255, 120);
    constexpr COLORREF Red      = RGB(255, 60, 80);
    constexpr COLORREF TextPri  = RGB(230, 235, 255);
    constexpr COLORREF TextSec  = RGB(120, 130, 160);
    constexpr COLORREF Border   = RGB(40, 50, 70);
}

static constexpr int OVL_W = 280;
static constexpr int OVL_H = 432;

static HWND g_overlay = nullptr;
static bool g_visible = false;
static HBRUSH g_bgBrush = nullptr;
static HFONT g_fontTitle = nullptr;
static HFONT g_fontNormal = nullptr;
static HFONT g_fontSmall = nullptr;
static HFONT g_fontBold = nullptr;

// Toggle hit areas
struct ToggleArea { RECT rc; int idx; };
static ToggleArea g_toggles[10]{};

// ============================================================================
// Drawing helpers
// ============================================================================

static void DrawToggle(HDC hdc, int x, int y, bool on) {
    int w = 36, h = 18;
    COLORREF bg = on ? C::Green : C::Surface;
    COLORREF knob = on ? RGB(255,255,255) : C::TextSec;
    COLORREF border = on ? C::Green : C::Border;

    HBRUSH hBg = CreateSolidBrush(bg);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    SelectObject(hdc, hBg); SelectObject(hdc, hPen);
    RoundRect(hdc, x, y, x+w, y+h, h, h);
    DeleteObject(hBg); DeleteObject(hPen);

    int kx = on ? x+w-h+2 : x+2;
    HBRUSH hK = CreateSolidBrush(knob);
    HPEN np = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, hK); SelectObject(hdc, np);
    Ellipse(hdc, kx, y+2, kx+h-4, y+h-2);
    DeleteObject(hK); DeleteObject(np);
}

static void DrawSep(HDC hdc, int y) {
    for (int x = 10; x < OVL_W-10; x++) {
        float t = (float)(x-10) / (float)(OVL_W-20);
        float i = 1.0f - 2.0f * fabsf(t - 0.5f);
        SetPixel(hdc, x, y, RGB((int)(0*i), (int)(200*i*0.25f+40*(1-i)), (int)(255*i*0.25f+60*(1-i))));
    }
}

// ============================================================================
// Paint
// ============================================================================

static void OnPaint(HDC hdc) {
    RECT full = {0, 0, OVL_W, OVL_H};
    HBRUSH bg = CreateSolidBrush(C::BG);
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    auto& t = Trainer::Get();
    int y = 0;

    // Header
    RECT hdrRc = {0, 0, OVL_W, 34};
    HBRUSH hdrBg = CreateSolidBrush(C::Panel);
    FillRect(hdc, &hdrRc, hdrBg);
    DeleteObject(hdrBg);

    // Top accent line
    HPEN ap = CreatePen(PS_SOLID, 2, C::Cyan);
    SelectObject(hdc, ap);
    MoveToEx(hdc, 0, 0, nullptr); LineTo(hdc, OVL_W, 0);
    DeleteObject(ap);

    SetTextColor(hdc, C::Cyan);
    SelectObject(hdc, g_fontTitle);
    RECT tRc = {10, 8, OVL_W, 30};
    DrawTextW(hdc, L"\x26A1 ZEUSMOD", -1, &tRc, DT_LEFT | DT_SINGLELINE);

    y = 38;

    // Stats bar
    if (t.IsReady()) {
        SetTextColor(hdc, C::TextPri);
        SelectObject(hdc, g_fontSmall);
        wchar_t stats[128];
        wsprintfW(stats, L"HP: %d/%d  \x2022  STA: %d/%d  \x2022  ARM: %d/%d",
            t.GetHealth(), t.GetMaxHealth(),
            t.GetStamina(), t.GetMaxStamina(),
            t.GetArmor(), t.GetMaxArmor());
        RECT sRc = {0, y, OVL_W, y+16};
        DrawTextW(hdc, stats, -1, &sRc, DT_CENTER | DT_SINGLELINE);
    } else {
        SetTextColor(hdc, C::Red);
        SelectObject(hdc, g_fontSmall);
        RECT sRc = {0, y, OVL_W, y+16};
        DrawTextW(hdc, L"Searching for player...", -1, &sRc, DT_CENTER | DT_SINGLELINE);
    }
    y += 20;
    DrawSep(hdc, y);
    y += 8;

    // Cheat rows
    struct CheatRow { const wchar_t* name; bool* flag; };
    CheatRow rows[] = {
        {L"God Mode",        &t.GodMode},
        {L"Inf. Stamina",    &t.InfiniteStamina},
        {L"Inf. Armor",      &t.InfiniteArmor},
        {L"Inf. Oxygen",     &t.InfiniteOxygen},
        {L"Inf. Food",       &t.InfiniteFood},
        {L"Inf. Water",      &t.InfiniteWater},
        {L"Speed Hack",      &t.SpeedHack},
        {L"Free Craft",      &t.FreeCraft},
        {L"No Weight",       &t.NoWeight},
        {L"Time Lock",       &t.TimeLock},
    };

    for (int i = 0; i < 10; i++) {
        // Dot
        COLORREF dotC = *rows[i].flag ? C::Green : C::TextSec;
        HBRUSH db = CreateSolidBrush(dotC);
        HPEN dnp = CreatePen(PS_NULL, 0, 0);
        SelectObject(hdc, db); SelectObject(hdc, dnp);
        Ellipse(hdc, 14, y+5, 22, y+13);
        DeleteObject(db); DeleteObject(dnp);

        // Name
        SetTextColor(hdc, C::TextPri);
        SelectObject(hdc, g_fontNormal);
        RECT nRc = {28, y, OVL_W-55, y+18};
        DrawTextW(hdc, rows[i].name, -1, &nRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        // Toggle
        int tx = OVL_W - 50;
        DrawToggle(hdc, tx, y, *rows[i].flag);
        g_toggles[i] = {{tx, y, tx+36, y+18}, i};

        y += 26;

        // Time display after Time Lock
        if (i == 9) {
            SetTextColor(hdc, C::Purple);
            SelectObject(hdc, g_fontBold);
            int hour = (int)t.LockedTime;
            int min = (int)((t.LockedTime - hour) * 60);
            wchar_t tm[32];
            swprintf(tm, 32, L"%02d:%02d", hour, min);
            RECT tmRc = {OVL_W-100, y-26, OVL_W-55, y-8};
            DrawTextW(hdc, tm, -1, &tmRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        // Speed multiplier line after Speed Hack
        if (i == 6) {
            SetTextColor(hdc, C::Purple);
            SelectObject(hdc, g_fontBold);
            wchar_t spd[32];
            swprintf(spd, 32, L"x%.1f", t.SpeedMultiplier);
            RECT spRc = {OVL_W-100, y-26, OVL_W-55, y-8};
            DrawTextW(hdc, spd, -1, &spRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
    }

    // Bottom
    y = OVL_H - 28;
    DrawSep(hdc, y);
    y += 6;
    SetTextColor(hdc, C::TextSec);
    SelectObject(hdc, g_fontSmall);
    RECT fRc = {0, y, OVL_W, y+16};
    DrawTextW(hdc, L"Made by CyberSnake  \x2502  N / F10", -1, &fRc, DT_CENTER | DT_SINGLELINE);
}

// ============================================================================
// WndProc
// ============================================================================

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, OVL_W, OVL_H);
        SelectObject(mem, bmp);
        OnPaint(mem);
        BitBlt(hdc, 0, 0, OVL_W, OVL_H, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        POINT pt = {mx, my};
        auto& t = Trainer::Get();

        bool* flags[] = {
            &t.GodMode, &t.InfiniteStamina, &t.InfiniteArmor, &t.InfiniteOxygen,
            &t.InfiniteFood, &t.InfiniteWater, &t.SpeedHack, &t.FreeCraft, &t.NoWeight, &t.TimeLock
        };
        const char* names[] = {
            "GodMode", "Stamina", "Armor", "Oxygen", "Food",
            "Water", "Speed", "Craft", "Weight", "TimeLock"
        };

        for (int i = 0; i < 10; i++) {
            if (PtInRect(&g_toggles[i].rc, pt)) {
                *flags[i] = !*flags[i];
                printf("[TOGGLE] %s: %s\n", names[i], *flags[i] ? "ON" : "OFF");
                if (i == 9 && *flags[i]) {
                    printf("[TIME] Current target: %.0f:00 (right click to cycle 06 -> 12 -> 18 -> 00)\n",
                        t.LockedTime);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        break;
    }

    case WM_RBUTTONDOWN: {
        // Right click on time lock to cycle time: 6 (dawn) → 12 (noon) → 18 (dusk) → 0 (midnight)
        auto& t2 = Trainer::Get();
        if (t2.TimeLock) {
            if (t2.LockedTime < 6.0f) t2.LockedTime = 6.0f;
            else if (t2.LockedTime < 12.0f) t2.LockedTime = 12.0f;
            else if (t2.LockedTime < 18.0f) t2.LockedTime = 18.0f;
            else t2.LockedTime = 0.0f;
            printf("[TIME] Set to %.0f:00\n", t2.LockedTime);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_CLOSE:
        Overlay::Toggle();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Public API
// ============================================================================

bool Overlay::Create() {
    g_bgBrush = CreateSolidBrush(C::BG);
    g_fontTitle = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold = CreateFontW(-12, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"ZeusModOverlay";
    RegisterClassExW(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    g_overlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ZeusModOverlay", L"ZeusMod",
        WS_POPUP | WS_BORDER,
        screenW - OVL_W - 20, 50, OVL_W, OVL_H,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_overlay) return false;
    ShowWindow(g_overlay, SW_SHOW);
    g_visible = true;

    // Refresh timer for live stats
    SetTimer(g_overlay, 1, 200, nullptr);

    printf("[OVERLAY] Created.\n");
    return true;
}

void Overlay::Destroy() {
    if (g_overlay) {
        KillTimer(g_overlay, 1);
        DestroyWindow(g_overlay);
        g_overlay = nullptr;
    }
    UnregisterClassW(L"ZeusModOverlay", GetModuleHandleW(nullptr));
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_fontNormal) DeleteObject(g_fontNormal);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontBold) DeleteObject(g_fontBold);
}

void Overlay::ProcessMessages() {
    MSG msg;
    while (PeekMessageW(&msg, g_overlay, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_TIMER) {
            InvalidateRect(g_overlay, nullptr, FALSE);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Overlay::IsVisible() { return g_visible; }

void Overlay::Toggle() {
    g_visible = !g_visible;
    ShowWindow(g_overlay, g_visible ? SW_SHOW : SW_HIDE);
}
