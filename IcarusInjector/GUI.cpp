#include "GUI.h"
#include <CommCtrl.h>

#pragma comment(lib, "comctl32.lib")

namespace IcarusMod {

static constexpr COLORREF COLOR_BG       = RGB(30, 30, 36);
static constexpr COLORREF COLOR_PANEL    = RGB(40, 40, 48);
static constexpr COLORREF COLOR_TEXT     = RGB(220, 220, 230);
static constexpr COLORREF COLOR_ACCENT   = RGB(100, 140, 255);
static constexpr COLORREF COLOR_GREEN    = RGB(50, 205, 80);
static constexpr COLORREF COLOR_RED      = RGB(180, 50, 50);
static constexpr COLORREF COLOR_CATEGORY = RGB(160, 160, 180);

GUI::~GUI() {
    if (m_bgBrush)    DeleteObject(m_bgBrush);
    if (m_greenBrush) DeleteObject(m_greenBrush);
    if (m_redBrush)   DeleteObject(m_redBrush);
    if (m_fontTitle)  DeleteObject(m_fontTitle);
    if (m_fontNormal) DeleteObject(m_fontNormal);
    if (m_fontSmall)  DeleteObject(m_fontSmall);
}

bool GUI::Create(HINSTANCE hInstance) {
    m_bgBrush    = CreateSolidBrush(COLOR_BG);
    m_greenBrush = CreateSolidBrush(COLOR_GREEN);
    m_redBrush   = CreateSolidBrush(COLOR_RED);

    m_fontTitle  = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontNormal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    m_fontSmall  = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_bgBrush;
    wc.lpszClassName = L"IcarusModClass";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) return false;

    RECT rc = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    m_hwnd = CreateWindowExW(
        0, L"IcarusModClass", L"IcarusMod Trainer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, this
    );

    return m_hwnd != nullptr;
}

void GUI::Show() {
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
}

void GUI::SetCheatState(uint32_t cheatId, bool enabled) {
    if (cheatId >= CHEAT_COUNT) return;
    m_cheatStates[cheatId] = enabled;
    if (m_cheatIndicators[cheatId]) {
        InvalidateRect(m_cheatIndicators[cheatId], nullptr, TRUE);
    }
}

void GUI::SetConnectionStatus(bool connected) {
    SetStatusText(connected ? L"Connected to Icarus" : L"Not connected");
}

void GUI::SetStatusText(const wchar_t* text) {
    if (m_lblStatus) {
        SetWindowTextW(m_lblStatus, text);
    }
}

void GUI::SetAttachEnabled(bool enabled) {
    if (m_btnAttach) {
        EnableWindow(m_btnAttach, enabled ? TRUE : FALSE);
    }
}

LRESULT CALLBACK GUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GUI* gui = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        gui = static_cast<GUI*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(gui));
    }
    else {
        gui = reinterpret_cast<GUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_CREATE:
        if (gui) {
            gui->m_hwnd = hwnd;  // Set hwnd early so OnCreate can use it
            gui->OnCreate(hwnd);
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, COLOR_BG);
        SetTextColor(hdc, COLOR_TEXT);
        if (gui) return reinterpret_cast<LRESULT>(gui->m_bgBrush);
        break;
    }

    case WM_DRAWITEM: {
        auto dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis && gui && dis->CtlID >= static_cast<UINT>(GUI::ID_CHEAT_BASE) &&
            dis->CtlID < static_cast<UINT>(GUI::ID_CHEAT_BASE + CHEAT_COUNT)) {
            uint32_t idx = dis->CtlID - GUI::ID_CHEAT_BASE;
            HBRUSH brush = gui->m_cheatStates[idx] ? gui->m_greenBrush : gui->m_redBrush;

            // Draw rounded indicator
            HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, brush);
            HPEN pen = CreatePen(PS_NULL, 0, 0);
            HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
            Ellipse(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                    dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldPen);
            SelectObject(dis->hDC, oldBrush);
            DeleteObject(pen);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (gui && LOWORD(wParam) == ID_BTN_ATTACH && HIWORD(wParam) == BN_CLICKED) {
            // Forward to parent message loop via custom message
            PostMessageW(hwnd, WM_USER + 1, 0, 0);
        }
        break;

    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void GUI::OnCreate(HWND hwnd) {
    int y = 15;

    // Title
    HWND title = CreateWindowExW(0, L"STATIC", L"ICARUS MOD TRAINER",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        0, y, WINDOW_WIDTH, 28, hwnd, nullptr, nullptr, nullptr);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(m_fontTitle), TRUE);
    y += 35;

    // Status label
    m_lblStatus = CreateWindowExW(0, L"STATIC", L"Not connected - Launch Icarus then click Attach",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        10, y, WINDOW_WIDTH - 20, 20, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS_LABEL)),
        nullptr, nullptr);
    SendMessageW(m_lblStatus, WM_SETFONT, reinterpret_cast<WPARAM>(m_fontSmall), TRUE);
    y += 28;

    // Attach button
    m_btnAttach = CreateWindowExW(0, L"BUTTON", L"Attach to Icarus",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        (WINDOW_WIDTH - 200) / 2, y, 200, 32,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_ATTACH)),
        nullptr, nullptr);
    SendMessageW(m_btnAttach, WM_SETFONT, reinterpret_cast<WPARAM>(m_fontNormal), TRUE);
    y += 48;

    // Separator
    CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
        15, y, WINDOW_WIDTH - 30, 2, hwnd, nullptr, nullptr, nullptr);
    y += 12;

    // Cheat list
    const wchar_t* lastCategory = nullptr;
    for (uint32_t i = 0; i < CHEAT_COUNT; i++) {
        const auto& info = CHEAT_TABLE[i];

        // Category header
        if (!lastCategory || wcscmp(lastCategory, info.category) != 0) {
            HWND cat = CreateWindowExW(0, L"STATIC", info.category,
                WS_CHILD | WS_VISIBLE,
                20, y, 200, 18, hwnd, nullptr, nullptr, nullptr);
            SendMessageW(cat, WM_SETFONT, reinterpret_cast<WPARAM>(m_fontSmall), TRUE);
            y += 22;
            lastCategory = info.category;
        }

        // Status indicator (owner-drawn circle)
        m_cheatIndicators[i] = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            25, y + 2, 12, 12,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHEAT_BASE + i)),
            nullptr, nullptr);

        // Cheat name + hotkey
        wchar_t label[128];
        wsprintfW(label, L"%s   [F%d]", info.name, (info.hotkey - VK_F1 + 1));
        m_cheatLabels[i] = CreateWindowExW(0, L"STATIC", label,
            WS_CHILD | WS_VISIBLE,
            45, y, WINDOW_WIDTH - 70, 18, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(m_cheatLabels[i], WM_SETFONT, reinterpret_cast<WPARAM>(m_fontNormal), TRUE);

        y += 26;
    }
}

} // namespace IcarusMod
