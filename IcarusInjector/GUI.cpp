// ============================================================================
// ZeusMod Injector - Premium Gaming UI
// Custom borderless window, full GDI drawing, neon accents
// ============================================================================
#include "GUI.h"
#include "resource.h"
#include <cstdio>
#include <cmath>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace IcarusMod {

GUI::~GUI() {
    if (m_bgBrush) DeleteObject(m_bgBrush);
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_fontNormal) DeleteObject(m_fontNormal);
    if (m_fontSmall) DeleteObject(m_fontSmall);
    if (m_fontBold) DeleteObject(m_fontBold);
}

bool GUI::Create(HINSTANCE hInstance) {
    m_bgBrush = CreateSolidBrush(Colors::BG);
    m_fontTitle = CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontNormal = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Semibold");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_bgBrush;
    wc.lpszClassName = L"ZeusModClass";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ZEUSMOD));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) return false;

    RECT rc = {0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    m_hwnd = CreateWindowExW(0, L"ZeusModClass", L"ZeusMod - Icarus Trainer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, this);

    // Dark title bar (Windows 10/11)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));

    return m_hwnd != nullptr;
}

void GUI::Show() {
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

void GUI::SetStatusText(const wchar_t* text) {
    wcsncpy_s(m_statusText, text, 255);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void GUI::SetAttachEnabled(bool e) {
    if (m_btnAttach) EnableWindow(m_btnAttach, e ? TRUE : FALSE);
}

void GUI::SetCheatState(uint32_t id, bool e) {
    if (id < CHEAT_COUNT) { m_cheatStates[id] = e; InvalidateRect(m_hwnd, nullptr, FALSE); }
}

void GUI::SetConnectionStatus(bool c) {
    m_connected = c; InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ============================================================================
// Drawing Primitives
// ============================================================================

void GUI::DrawRoundedRect(HDC hdc, RECT rc, int r, COLORREF fill, COLORREF border) {
    HBRUSH hB = CreateSolidBrush(fill);
    HPEN hP = CreatePen(PS_SOLID, 1, border);
    SelectObject(hdc, hB); SelectObject(hdc, hP);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    DeleteObject(hB); DeleteObject(hP);
}

void GUI::DrawToggle(HDC hdc, int x, int y, bool on) {
    int w = 44, h = 22;
    // Track
    COLORREF trackColor = on ? RGB(0, 200, 100) : Colors::Surface;
    COLORREF borderColor = on ? RGB(0, 180, 90) : Colors::Border;
    HBRUSH hBg = CreateSolidBrush(trackColor);
    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    SelectObject(hdc, hBg); SelectObject(hdc, hPen);
    RoundRect(hdc, x, y, x+w, y+h, h, h);
    DeleteObject(hBg); DeleteObject(hPen);

    // Knob with shadow effect
    int kx = on ? x + w - h + 3 : x + 3;
    int ky = y + 3;
    int ksz = h - 6;

    // Shadow
    HBRUSH shBr = CreateSolidBrush(RGB(0,0,0));
    HPEN np = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, shBr); SelectObject(hdc, np);
    Ellipse(hdc, kx+1, ky+1, kx+ksz+1, ky+ksz+1);
    DeleteObject(shBr);

    // Knob
    COLORREF knobC = on ? RGB(255,255,255) : RGB(140,145,160);
    HBRUSH kBr = CreateSolidBrush(knobC);
    SelectObject(hdc, kBr);
    Ellipse(hdc, kx, ky, kx+ksz, ky+ksz);
    DeleteObject(kBr);
    DeleteObject(np);

    // ON/OFF text
    SetTextColor(hdc, on ? RGB(255,255,255) : Colors::TextSec);
    SelectObject(hdc, m_fontSmall);
    RECT tRc = {on ? x+4 : x+h, y+3, on ? x+w-h : x+w-4, y+h-3};
    DrawTextW(hdc, on ? L"ON" : L"OFF", -1, &tRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

void GUI::DrawCategoryHeader(HDC hdc, int y, const wchar_t* text) {
    // Category bar
    RECT barRc = {15, y, WIN_W - 15, y + 22};
    DrawRoundedRect(hdc, barRc, 4, RGB(12, 16, 28), Colors::Border);

    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontBold);
    RECT rc = {24, y + 3, WIN_W - 24, y + 19};
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE);
}

void GUI::DrawCheatRow(HDC hdc, int y, const wchar_t* name, bool state, int index) {
    // Hover effect background
    RECT rowRc = {15, y-2, WIN_W - 15, y + 22};
    if (state) {
        // Subtle green tint when active
        DrawRoundedRect(hdc, rowRc, 4, RGB(10, 18, 14), Colors::BG);
    }

    // Status dot with glow
    int dotX = 24, dotY = y + 5;
    COLORREF dotC = state ? Colors::NeonGreen : RGB(60, 65, 80);

    // Glow (larger circle, semi-transparent)
    if (state) {
        HBRUSH glowBr = CreateSolidBrush(RGB(0, 80, 40));
        HPEN np2 = CreatePen(PS_NULL, 0, 0);
        SelectObject(hdc, glowBr); SelectObject(hdc, np2);
        Ellipse(hdc, dotX-3, dotY-3, dotX+13, dotY+13);
        DeleteObject(glowBr); DeleteObject(np2);
    }

    HBRUSH dotBr = CreateSolidBrush(dotC);
    HPEN np = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, dotBr); SelectObject(hdc, np);
    Ellipse(hdc, dotX, dotY, dotX+10, dotY+10);
    DeleteObject(dotBr); DeleteObject(np);

    // Name
    SetTextColor(hdc, state ? Colors::TextPri : RGB(160, 165, 180));
    SelectObject(hdc, m_fontNormal);
    RECT textRc = {42, y, WIN_W - 75, y + 20};
    DrawTextW(hdc, name, -1, &textRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Toggle
    int tx = WIN_W - 68;
    DrawToggle(hdc, tx, y - 1, state);

    if (index >= 0 && index < (int)m_toggleAreas.size()) {
        m_toggleAreas[index].rc = {tx - 5, y - 5, tx + 50, y + 25};
        m_toggleAreas[index].cheatIdx = index;
    }
}

void GUI::DrawSeparator(HDC hdc, int y) {
    HPEN pen = CreatePen(PS_SOLID, 1, Colors::Border);
    SelectObject(hdc, pen);
    MoveToEx(hdc, 20, y, nullptr);
    LineTo(hdc, WIN_W - 20, y);
    DeleteObject(pen);
}

void GUI::DrawStatusPanel(HDC hdc, int y) {
    RECT panelRc = {15, y, WIN_W - 15, y + 36};
    DrawRoundedRect(hdc, panelRc, 8, Colors::Panel, Colors::Border);

    // Status dot
    COLORREF dotC = m_connected ? Colors::NeonGreen : Colors::NeonRed;
    HBRUSH dotBr = CreateSolidBrush(dotC);
    HPEN np = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, dotBr); SelectObject(hdc, np);
    Ellipse(hdc, 26, y + 13, 36, y + 23);
    DeleteObject(dotBr); DeleteObject(np);

    // Text
    SetTextColor(hdc, Colors::TextPri);
    SelectObject(hdc, m_fontNormal);
    RECT tRc = {44, y + 8, WIN_W - 25, y + 28};
    DrawTextW(hdc, m_statusText, -1, &tRc, DT_LEFT | DT_SINGLELINE);
}

void GUI::DrawNeonButton(HDC hdc, RECT rc, const wchar_t* text, bool hovered) {
    COLORREF fill = hovered ? RGB(0, 30, 40) : Colors::Surface;
    DrawRoundedRect(hdc, rc, 8, fill, Colors::NeonCyan);

    // Inner glow line at top
    HPEN glowPen = CreatePen(PS_SOLID, 1, RGB(0, 150, 200));
    SelectObject(hdc, glowPen);
    MoveToEx(hdc, rc.left + 8, rc.top + 1, nullptr);
    LineTo(hdc, rc.right - 8, rc.top + 1);
    DeleteObject(glowPen);

    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontBold);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

// ============================================================================
// Main Paint
// ============================================================================

void GUI::OnPaint(HDC hdc) {
    RECT full = {0, 0, WIN_W, WIN_H};
    HBRUSH bg = CreateSolidBrush(Colors::BG);
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    int y = 0;

    // ─── Header ───
    RECT hdrRc = {0, 0, WIN_W, 80};
    HBRUSH hdrBg = CreateSolidBrush(Colors::HeaderGrad);
    FillRect(hdc, &hdrRc, hdrBg);
    DeleteObject(hdrBg);

    // Top accent
    HPEN acPen = CreatePen(PS_SOLID, 3, Colors::NeonCyan);
    SelectObject(hdc, acPen);
    MoveToEx(hdc, 0, 0, nullptr); LineTo(hdc, WIN_W, 0);
    DeleteObject(acPen);

    // Title
    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontTitle);
    RECT titleRc = {0, 14, WIN_W, 46};
    DrawTextW(hdc, L"\x26A1 ZEUSMOD", -1, &titleRc, DT_CENTER | DT_SINGLELINE);

    // Subtitle
    SetTextColor(hdc, Colors::TextSec);
    SelectObject(hdc, m_fontSmall);
    RECT subRc = {0, 50, WIN_W, 66};
    DrawTextW(hdc, L"v1.0  \x2500  Icarus Trainer  \x2500  Made by CyberSnake", -1, &subRc, DT_CENTER | DT_SINGLELINE);

    // Bottom border of header
    HPEN hdrBorder = CreatePen(PS_SOLID, 1, Colors::Border);
    SelectObject(hdc, hdrBorder);
    MoveToEx(hdc, 0, 79, nullptr); LineTo(hdc, WIN_W, 79);
    DeleteObject(hdrBorder);

    y = 90;

    // ─── Status ───
    DrawStatusPanel(hdc, y);
    y += 48;

    // ─── Attach Button ───
    RECT btnRc = {30, y, WIN_W - 30, y + 38};
    DrawNeonButton(hdc, btnRc, L"\x25B6  ATTACH TO ICARUS", false);
    y += 52;

    // ─── Cheats ───
    DrawCategoryHeader(hdc, y, L"\x2694  SURVIVAL"); y += 30;
    DrawCheatRow(hdc, y, L"God Mode", m_cheatStates[0], 0); y += 28;
    DrawCheatRow(hdc, y, L"Infinite Stamina", m_cheatStates[1], 1); y += 28;
    DrawCheatRow(hdc, y, L"Infinite Armor", m_cheatStates[2], 2); y += 28;
    DrawCheatRow(hdc, y, L"Infinite Oxygen", m_cheatStates[3], 3); y += 28;

    y += 6;
    DrawCategoryHeader(hdc, y, L"\x2728  RESOURCES"); y += 30;
    DrawCheatRow(hdc, y, L"No Hunger / Thirst", m_cheatStates[4], 4); y += 28;
    DrawCheatRow(hdc, y, L"Infinite Water", m_cheatStates[5], 5); y += 28;
    DrawCheatRow(hdc, y, L"Free Craft", m_cheatStates[6], 6); y += 28;
    DrawCheatRow(hdc, y, L"No Weight Limit", m_cheatStates[8], 8); y += 28;

    y += 6;
    DrawCategoryHeader(hdc, y, L"\x26A1  MOVEMENT & WORLD"); y += 30;
    DrawCheatRow(hdc, y, L"Speed Hack", m_cheatStates[7], 7); y += 28;
    DrawCheatRow(hdc, y, L"Time Lock", m_cheatStates[9], 9); y += 28;

    // ─── Footer ───
    y = WIN_H - 30;
    DrawSeparator(hdc, y - 8);
    SetTextColor(hdc, RGB(80, 85, 100));
    SelectObject(hdc, m_fontSmall);
    RECT footRc = {0, y, WIN_W, y + 18};
    DrawTextW(hdc, L"N = Toggle Overlay  \x2502  F10 = Detach  \x2502  Right-Click = Options", -1, &footRc, DT_CENTER | DT_SINGLELINE);
}

// ============================================================================
// WndProc
// ============================================================================

LRESULT CALLBACK GUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GUI* gui = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        gui = static_cast<GUI*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(gui));
    } else {
        gui = reinterpret_cast<GUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_CREATE:
        if (gui) { gui->m_hwnd = hwnd; gui->OnCreate(hwnd); }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, GUI::WIN_W, GUI::WIN_H);
        SelectObject(mem, bmp);
        if (gui) gui->OnPaint(mem);
        BitBlt(hdc, 0, 0, GUI::WIN_W, GUI::WIN_H, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        if (!gui) break;
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        POINT pt = {mx, my};

        // Attach button
        RECT btnRc = {30, 138, GUI::WIN_W - 30, 176};
        if (PtInRect(&btnRc, pt)) {
            PostMessageW(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }

        // Toggles
        for (auto& ta : gui->m_toggleAreas) {
            if (PtInRect(&ta.rc, pt) && ta.cheatIdx >= 0) {
                gui->m_cheatStates[ta.cheatIdx] = !gui->m_cheatStates[ta.cheatIdx];
                InvalidateRect(hwnd, nullptr, FALSE);
                PostMessageW(hwnd, WM_USER + 2, ta.cheatIdx, gui->m_cheatStates[ta.cheatIdx]);
                return 0;
            }
        }
        break;
    }

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GUI::OnCreate(HWND hwnd) {
    m_btnAttach = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD, 0, 0, 1, 1, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_ATTACH)),
        nullptr, nullptr);
}

} // namespace IcarusMod
