// ============================================================================
// ZeusMod Injector - Gaming/Neon UI
// Full custom GDI drawing, no standard controls except the hidden attach button
// ============================================================================
#include "GUI.h"
#include <cstdio>

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
    m_fontTitle = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontSmall = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = m_bgBrush;
    wc.lpszClassName = L"ZeusModClass";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    RECT rc = {0, 0, WIN_W, WIN_H};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    m_hwnd = CreateWindowExW(0, L"ZeusModClass", L"ZeusMod",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, this);

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

void GUI::SetAttachEnabled(bool enabled) {
    if (m_btnAttach) EnableWindow(m_btnAttach, enabled ? TRUE : FALSE);
}

void GUI::SetCheatState(uint32_t id, bool enabled) {
    if (id < CHEAT_COUNT) {
        m_cheatStates[id] = enabled;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void GUI::SetConnectionStatus(bool connected) {
    m_connected = connected;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ============================================================================
// Custom Drawing Helpers
// ============================================================================

void GUI::DrawRoundedRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border) {
    HBRUSH hFill = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, hFill);
    HPEN oldPen = (HPEN)SelectObject(hdc, hPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hFill);
    DeleteObject(hPen);
}

void GUI::DrawToggle(HDC hdc, int x, int y, bool state) {
    int w = 40, h = 20;
    COLORREF bgColor = state ? Colors::NeonGreen : Colors::Surface;
    COLORREF knobColor = state ? RGB(255, 255, 255) : Colors::TextSec;
    COLORREF borderColor = state ? Colors::NeonGreen : Colors::Border;

    // Track (capsule)
    HBRUSH hBg = CreateSolidBrush(bgColor);
    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    SelectObject(hdc, hBg);
    SelectObject(hdc, hPen);
    RoundRect(hdc, x, y, x + w, y + h, h, h);
    DeleteObject(hBg);
    DeleteObject(hPen);

    // Knob (circle)
    int knobX = state ? x + w - h + 2 : x + 2;
    int knobY = y + 2;
    int knobSize = h - 4;
    HBRUSH hKnob = CreateSolidBrush(knobColor);
    HPEN hPenNull = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, hKnob);
    SelectObject(hdc, hPenNull);
    Ellipse(hdc, knobX, knobY, knobX + knobSize, knobY + knobSize);
    DeleteObject(hKnob);
    DeleteObject(hPenNull);
}

void GUI::DrawCategoryHeader(HDC hdc, int y, const wchar_t* text) {
    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontBold);
    RECT rc = {20, y, WIN_W - 20, y + 16};
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE);
}

void GUI::DrawCheatRow(HDC hdc, int y, const wchar_t* name, bool state, int index) {
    // Status dot
    int dotX = 20, dotY = y + 5;
    HBRUSH dotBrush = CreateSolidBrush(state ? Colors::NeonGreen : Colors::TextSec);
    HPEN nullPen = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, dotBrush);
    SelectObject(hdc, nullPen);
    Ellipse(hdc, dotX, dotY, dotX + 8, dotY + 8);
    DeleteObject(dotBrush);
    DeleteObject(nullPen);

    // Name text
    SetTextColor(hdc, Colors::TextPri);
    SelectObject(hdc, m_fontNormal);
    RECT textRc = {35, y, WIN_W - 70, y + 18};
    DrawTextW(hdc, name, -1, &textRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Toggle switch
    int toggleX = WIN_W - 65;
    DrawToggle(hdc, toggleX, y - 1, state);

    // Store hit area for click detection
    if (index >= 0 && index < (int)m_toggleAreas.size()) {
        m_toggleAreas[index].rc = {toggleX, y - 1, toggleX + 40, y + 19};
        m_toggleAreas[index].cheatIdx = index;
    }
}

void GUI::DrawSeparator(HDC hdc, int y) {
    // Gradient-like separator: dim on edges, brighter in middle
    for (int x = 20; x < WIN_W - 20; x++) {
        float t = (float)(x - 20) / (float)(WIN_W - 40);
        float intensity = 1.0f - 2.0f * fabsf(t - 0.5f);
        int r = (int)(0 * intensity + 40 * (1 - intensity));
        int g = (int)(200 * intensity * 0.3f + 50 * (1 - intensity));
        int b = (int)(255 * intensity * 0.3f + 70 * (1 - intensity));
        SetPixel(hdc, x, y, RGB(r, g, b));
    }
}

void GUI::DrawStatusPanel(HDC hdc, int y) {
    RECT panelRc = {15, y, WIN_W - 15, y + 32};
    DrawRoundedRect(hdc, panelRc, 8, Colors::Panel, Colors::Border);

    // Status dot
    COLORREF dotColor = m_connected ? Colors::NeonGreen : Colors::NeonRed;
    HBRUSH dotBrush = CreateSolidBrush(dotColor);
    HPEN nullPen = CreatePen(PS_NULL, 0, 0);
    SelectObject(hdc, dotBrush);
    SelectObject(hdc, nullPen);
    Ellipse(hdc, 25, y + 12, 33, y + 20);
    DeleteObject(dotBrush);
    DeleteObject(nullPen);

    // Status text
    SetTextColor(hdc, Colors::TextPri);
    SelectObject(hdc, m_fontNormal);
    RECT textRc = {40, y + 7, WIN_W - 25, y + 25};
    DrawTextW(hdc, m_statusText, -1, &textRc, DT_LEFT | DT_SINGLELINE);
}

// ============================================================================
// Main Paint
// ============================================================================

void GUI::OnPaint(HDC hdc) {
    // Background
    RECT fullRc = {0, 0, WIN_W, WIN_H};
    HBRUSH bgBrush = CreateSolidBrush(Colors::BG);
    FillRect(hdc, &fullRc, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    int y = 0;

    // ── Header zone ──
    RECT headerRc = {0, 0, WIN_W, 70};
    HBRUSH headerBrush = CreateSolidBrush(Colors::HeaderGrad);
    FillRect(hdc, &headerRc, headerBrush);
    DeleteObject(headerBrush);

    // Neon accent line at top
    HPEN accentPen = CreatePen(PS_SOLID, 2, Colors::NeonCyan);
    SelectObject(hdc, accentPen);
    MoveToEx(hdc, 0, 0, nullptr);
    LineTo(hdc, WIN_W, 0);
    DeleteObject(accentPen);

    // Title
    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontTitle);
    RECT titleRc = {0, 12, WIN_W, 40};
    DrawTextW(hdc, L"\x26A1 ZEUSMOD", -1, &titleRc, DT_CENTER | DT_SINGLELINE);

    // Subtitle
    SetTextColor(hdc, Colors::TextSec);
    SelectObject(hdc, m_fontSmall);
    RECT subRc = {0, 42, WIN_W, 58};
    DrawTextW(hdc, L"v1.0  \x2022  Icarus Trainer  \x2022  Internal", -1, &subRc, DT_CENTER | DT_SINGLELINE);

    y = 78;

    // ── Status Panel ──
    DrawStatusPanel(hdc, y);
    y += 44;

    // ── Attach Button ──
    RECT btnRc = {40, y, WIN_W - 40, y + 36};
    DrawRoundedRect(hdc, btnRc, 8, Colors::Surface, Colors::NeonCyan);
    SetTextColor(hdc, Colors::NeonCyan);
    SelectObject(hdc, m_fontBold);
    DrawTextW(hdc, L"ATTACH TO ICARUS", -1, &btnRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    y += 50;

    // ── Separator ──
    DrawSeparator(hdc, y);
    y += 12;

    // ── SURVIVAL Category ──
    DrawCategoryHeader(hdc, y, L"SURVIVAL");
    y += 22;
    DrawCheatRow(hdc, y, L"God Mode", m_cheatStates[0], 0); y += 26;
    DrawCheatRow(hdc, y, L"Infinite Stamina", m_cheatStates[1], 1); y += 26;
    DrawCheatRow(hdc, y, L"Infinite Armor", m_cheatStates[2], 2); y += 26;
    DrawCheatRow(hdc, y, L"Infinite Oxygen", m_cheatStates[3], 3); y += 26;

    // ── Separator ──
    y += 4;
    DrawSeparator(hdc, y);
    y += 12;

    // ── RESOURCES Category ──
    DrawCategoryHeader(hdc, y, L"RESOURCES");
    y += 22;
    DrawCheatRow(hdc, y, L"No Hunger / Thirst", m_cheatStates[4], 4); y += 26;
    DrawCheatRow(hdc, y, L"Infinite Water", m_cheatStates[5], 5); y += 26;
    DrawCheatRow(hdc, y, L"Free Craft", m_cheatStates[6], 6); y += 26;

    // ── Separator ──
    y += 4;
    DrawSeparator(hdc, y);
    y += 12;

    // ── MOVEMENT Category ──
    DrawCategoryHeader(hdc, y, L"MOVEMENT");
    y += 22;
    DrawCheatRow(hdc, y, L"Speed Hack", m_cheatStates[7], 7); y += 26;

    // Speed multiplier display
    SetTextColor(hdc, Colors::NeonPurple);
    SelectObject(hdc, m_fontBold);
    RECT spdRc = {150, y - 26, 250, y - 8};
    // TODO: display actual speed multiplier

    // ── Bottom bar ──
    y = WIN_H - 35;
    DrawSeparator(hdc, y);
    y += 8;
    SetTextColor(hdc, Colors::TextSec);
    SelectObject(hdc, m_fontSmall);
    RECT footRc = {0, y, WIN_W, y + 16};
    DrawTextW(hdc, L"N = Toggle Overlay  \x2022  F10 = Detach", -1, &footRc, DT_CENTER | DT_SINGLELINE);
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
        if (gui) {
            gui->m_hwnd = hwnd;
            gui->OnCreate(hwnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // Double buffer
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBMP = CreateCompatibleBitmap(hdc, GUI::WIN_W, GUI::WIN_H);
        SelectObject(memDC, memBMP);
        if (gui) gui->OnPaint(memDC);
        BitBlt(hdc, 0, 0, GUI::WIN_W, GUI::WIN_H, memDC, 0, 0, SRCCOPY);
        DeleteObject(memBMP);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // Prevent flicker

    case WM_LBUTTONDOWN: {
        if (!gui) break;
        int mx = LOWORD(lParam), my = HIWORD(lParam);

        // Check attach button area (40, 122, WIN_W-40, 158)
        RECT btnRc = {40, 122, GUI::WIN_W - 40, 158};
        POINT pt = {mx, my};
        if (PtInRect(&btnRc, pt)) {
            PostMessageW(hwnd, WM_USER + 1, 0, 0);
            return 0;
        }

        // Check toggle areas
        for (auto& ta : gui->m_toggleAreas) {
            if (PtInRect(&ta.rc, pt) && ta.cheatIdx >= 0) {
                // Toggle this cheat
                gui->m_cheatStates[ta.cheatIdx] = !gui->m_cheatStates[ta.cheatIdx];
                InvalidateRect(hwnd, nullptr, FALSE);
                // Notify parent (send cheat toggle message)
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
    // Hidden attach button for legacy support
    m_btnAttach = CreateWindowExW(0, L"BUTTON", L"",
        WS_CHILD, 0, 0, 1, 1, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_ATTACH)),
        nullptr, nullptr);
}

} // namespace IcarusMod
