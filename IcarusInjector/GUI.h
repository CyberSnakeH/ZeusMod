#pragma once
#include <Windows.h>
#include <array>
#include "../Shared/SharedTypes.h"

namespace IcarusMod {

class GUI {
public:
    static constexpr int WINDOW_WIDTH = 420;
    static constexpr int WINDOW_HEIGHT = 640;

    GUI() = default;
    ~GUI();

    bool Create(HINSTANCE hInstance);
    void Show();
    HWND GetHwnd() const { return m_hwnd; }

    void SetCheatState(uint32_t cheatId, bool enabled);
    void SetConnectionStatus(bool connected);
    void SetStatusText(const wchar_t* text);
    void SetAttachEnabled(bool enabled);

    // Control IDs
    static constexpr int ID_BTN_ATTACH = 1001;
    static constexpr int ID_STATUS_LABEL = 1002;
    static constexpr int ID_CHEAT_BASE = 2000;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnCreate(HWND hwnd);

    HWND m_hwnd = nullptr;
    HWND m_btnAttach = nullptr;
    HWND m_lblStatus = nullptr;
    std::array<HWND, CHEAT_COUNT> m_cheatLabels{};
    std::array<HWND, CHEAT_COUNT> m_cheatIndicators{};
    std::array<bool, CHEAT_COUNT> m_cheatStates{};
    HBRUSH m_bgBrush = nullptr;
    HBRUSH m_greenBrush = nullptr;
    HBRUSH m_redBrush = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontNormal = nullptr;
    HFONT m_fontSmall = nullptr;
};

} // namespace IcarusMod
