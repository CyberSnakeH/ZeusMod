#pragma once
#include <Windows.h>
#include <array>
#include "SharedTypes.h"

namespace IcarusMod {

// ============================================================================
// ZeusMod Neon Color Palette
// ============================================================================
namespace Colors {
    constexpr COLORREF BG         = RGB(8, 8, 16);
    constexpr COLORREF Panel      = RGB(16, 20, 32);
    constexpr COLORREF Surface    = RGB(24, 28, 44);
    constexpr COLORREF NeonCyan   = RGB(0, 200, 255);
    constexpr COLORREF NeonPurple = RGB(140, 80, 255);
    constexpr COLORREF NeonGreen  = RGB(0, 255, 120);
    constexpr COLORREF NeonRed    = RGB(255, 60, 80);
    constexpr COLORREF TextPri    = RGB(230, 235, 255);
    constexpr COLORREF TextSec    = RGB(120, 130, 160);
    constexpr COLORREF Border     = RGB(40, 50, 70);
    constexpr COLORREF HeaderGrad = RGB(12, 15, 28);
}

class GUI {
public:
    static constexpr int WIN_W = 380;
    static constexpr int WIN_H = 440;

    static constexpr int ID_BTN_ATTACH = 1001;
    static constexpr int ID_STATUS = 1002;

    GUI() = default;
    ~GUI();

    bool Create(HINSTANCE hInstance);
    void Show();
    HWND GetHwnd() const { return m_hwnd; }

    void SetStatusText(const wchar_t* text);
    void SetAttachEnabled(bool enabled);
    void SetCheatState(uint32_t cheatId, bool enabled);
    void SetConnectionStatus(bool connected);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HDC hdc);
    void OnCreate(HWND hwnd);

    // Custom drawing helpers
    void DrawRoundedRect(HDC hdc, RECT rc, int radius, COLORREF fill, COLORREF border);
    void DrawNeonButton(HDC hdc, RECT rc, const wchar_t* text, bool hovered);
    void DrawToggle(HDC hdc, int x, int y, bool state);
    void DrawCategoryHeader(HDC hdc, int y, const wchar_t* text);
    void DrawCheatRow(HDC hdc, int y, const wchar_t* name, bool state, int index);
    void DrawSeparator(HDC hdc, int y);
    void DrawStatusPanel(HDC hdc, int y);

    HWND m_hwnd = nullptr;
    HWND m_btnAttach = nullptr;
    RECT m_btnRect = {};
    bool m_connected = false;
    wchar_t m_statusText[256] = L"Not Connected";

    HBRUSH m_bgBrush = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontNormal = nullptr;
    HFONT m_fontSmall = nullptr;
    HFONT m_fontBold = nullptr;

    std::array<bool, CHEAT_COUNT> m_cheatStates{};

    // Hit testing for toggles
    struct ToggleHitArea { RECT rc; int cheatIdx; };
    std::array<ToggleHitArea, CHEAT_COUNT> m_toggleAreas{};
};

} // namespace IcarusMod
