// ============================================================================
// ZeusMod Injector - Clean Launcher UI
// Just injects the DLL, cheats are controlled via in-game overlay
// ============================================================================
#include "GUI.h"
#include "resource.h"
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
    m_fontTitle = CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Semibold");

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

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20, &darkMode, sizeof(darkMode));
    return m_hwnd != nullptr;
}

void GUI::Show() { ShowWindow(m_hwnd, SW_SHOW); UpdateWindow(m_hwnd); }

void GUI::SetStatusText(const wchar_t* text) {
    wcsncpy_s(m_statusText, text, 255);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void GUI::SetAttachEnabled(bool) {}

void GUI::SetCheatState(uint32_t id, bool e) {
    if (id < CHEAT_COUNT) { m_cheatStates[id] = e; InvalidateRect(m_hwnd, nullptr, FALSE); }
}

void GUI::SetConnectionStatus(bool c) {
    m_connected = c;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ── Drawing ──

void GUI::DrawRoundedRect(HDC hdc, RECT rc, int r, COLORREF fill, COLORREF border) {
    HBRUSH hB = CreateSolidBrush(fill);
    HPEN hP = CreatePen(PS_SOLID, 1, border);
    SelectObject(hdc, hB); SelectObject(hdc, hP);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    DeleteObject(hB); DeleteObject(hP);
}

void GUI::DrawNeonButton(HDC hdc, RECT rc, const wchar_t* text, bool) {
    DrawRoundedRect(hdc, rc, 8, Colors::Surface, Colors::NeonCyan);
    HPEN gp = CreatePen(PS_SOLID, 1, RGB(0, 150, 200));
    SelectObject(hdc, gp);
    MoveToEx(hdc, rc.left + 8, rc.top + 1, nullptr);
    LineTo(hdc, rc.right - 8, rc.top + 1);
    DeleteObject(gp);
    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontBold);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

void GUI::DrawStatusPanel(HDC hdc, int y) {
    RECT pr = {20, y, WIN_W - 20, y + 36};
    DrawRoundedRect(hdc, pr, 8, Colors::Panel, Colors::Border);
    COLORREF dc = m_connected ? Colors::NeonGreen : Colors::NeonRed;
    HBRUSH db = CreateSolidBrush(dc);
    HPEN np = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, db); SelectObject(hdc, np);
    Ellipse(hdc, 32, y + 13, 40, y + 21);
    DeleteObject(db); DeleteObject(np);
    SetTextColor(hdc, Colors::TextPri);
    SelectObject(hdc, m_fontNormal);
    RECT tr = {48, y + 8, WIN_W - 30, y + 28};
    DrawTextW(hdc, m_statusText, -1, &tr, DT_LEFT | DT_SINGLELINE);
}

void GUI::DrawSeparator(HDC hdc, int y) {
    HPEN p = CreatePen(PS_SOLID, 1, Colors::Border);
    SelectObject(hdc, p);
    MoveToEx(hdc, 25, y, nullptr); LineTo(hdc, WIN_W - 25, y);
    DeleteObject(p);
}

void GUI::DrawToggle(HDC, int, int, bool) {}
void GUI::DrawCategoryHeader(HDC, int, const wchar_t*) {}
void GUI::DrawCheatRow(HDC, int, const wchar_t*, bool, int) {}

// ── Paint ──

void GUI::OnPaint(HDC hdc) {
    RECT full = {0, 0, WIN_W, WIN_H};
    HBRUSH bg = CreateSolidBrush(Colors::BG);
    FillRect(hdc, &full, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    // Header
    RECT hdr = {0, 0, WIN_W, 80};
    HBRUSH hb = CreateSolidBrush(Colors::HeaderGrad);
    FillRect(hdc, &hdr, hb);
    DeleteObject(hb);

    HPEN ap = CreatePen(PS_SOLID, 3, Colors::NeonCyan);
    SelectObject(hdc, ap);
    MoveToEx(hdc, 0, 0, nullptr); LineTo(hdc, WIN_W, 0);
    DeleteObject(ap);

    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontTitle);
    RECT tr = {0, 14, WIN_W, 46};
    DrawTextW(hdc, L"\x26A1 ZEUSMOD", -1, &tr, DT_CENTER | DT_SINGLELINE);

    SetTextColor(hdc, Colors::TextSec);
    SelectObject(hdc, m_fontSmall);
    RECT sr = {0, 50, WIN_W, 66};
    DrawTextW(hdc, L"v1.0  \x2500  Icarus Trainer  \x2500  Made by CyberSnake", -1, &sr, DT_CENTER | DT_SINGLELINE);

    HPEN hbp = CreatePen(PS_SOLID, 1, Colors::Border);
    SelectObject(hdc, hbp);
    MoveToEx(hdc, 0, 79, nullptr); LineTo(hdc, WIN_W, 79);
    DeleteObject(hbp);

    int y = 95;

    // Status
    DrawStatusPanel(hdc, y);
    y += 50;

    // Attach button
    m_btnRect = {40, y, WIN_W - 40, y + 40};
    DrawNeonButton(hdc, m_btnRect, m_connected ? L"\x2714  CONNECTED" : L"\x25B6  ATTACH TO ICARUS", false);
    y += 55;

    // Separator
    DrawSeparator(hdc, y);
    y += 15;

    // Instructions
    SetTextColor(hdc, Colors::TextSec);
    SelectObject(hdc, m_fontNormal);

    const wchar_t* lines[] = {
        L"1. Launch Icarus and enter a prospect",
        L"2. Click ATTACH TO ICARUS above",
        L"3. Press N in-game to toggle the overlay",
        L"4. Use the overlay to enable/disable cheats",
        L"",
        L"Available cheats:",
        L"  \x2022 God Mode, Infinite Stamina/Armor/Oxygen",
        L"  \x2022 No Hunger, Infinite Water, Free Craft",
        L"  \x2022 No Weight, Speed Hack, Time Lock",
    };

    for (auto& line : lines) {
        RECT lr = {30, y, WIN_W - 30, y + 18};
        DrawTextW(hdc, line, -1, &lr, DT_LEFT | DT_SINGLELINE);
        y += 20;
    }

    // Footer
    y = WIN_H - 30;
    DrawSeparator(hdc, y - 8);
    SetTextColor(hdc, RGB(60, 65, 80));
    SelectObject(hdc, m_fontSmall);
    RECT fr = {0, y, WIN_W, y + 18};
    DrawTextW(hdc, L"N = Overlay  \x2502  F10 = Detach  \x2502  github.com/CyberSnakeH/ZeusMod", -1, &fr, DT_CENTER | DT_SINGLELINE);
}

// ── WndProc ──

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
        if (gui) gui->m_hwnd = hwnd;
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
        POINT pt = {LOWORD(lParam), HIWORD(lParam)};
        if (PtInRect(&gui->m_btnRect, pt)) {
            PostMessageW(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GUI::OnCreate(HWND) {}

} // namespace IcarusMod
