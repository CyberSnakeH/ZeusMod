#include "Render.h"
#include "Trainer.h"
#include "cheats/TrainerInternal.h"
#include "UObjectLookup.h"
#include "Logger.h"
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "InterFont.h"        // embedded Inter Medium TTF blob (~411 KB)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_4.h>          // IDXGISwapChain3::GetCurrentBackBufferIndex
#include <vector>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool g_menuOpen = false;
static bool g_imguiReady = false;
static bool g_isDX12 = false;
static int g_frameCount = 0;

// ── D3D11 state (unchanged) ─────────────────────────────────────────────────
static ID3D11Device* g_d11Device = nullptr;
static ID3D11DeviceContext* g_d11Context = nullptr;
static ID3D11RenderTargetView* g_d11RTV = nullptr;

// ── D3D12 state (native backend, replaces the fragile D3D11On12 path) ──────
// Principle: render ImGui with its official D3D12 backend, using the game's
// own command queue (obtained via swapChain->GetDevice(IID_ID3D12CommandQueue))
// so that we submit on the same queue the game uses to present. No
// second-device, no second-queue, no cross-queue barrier races.
static ID3D12Device*            g_d12Device      = nullptr;
static ID3D12CommandQueue*      g_d12CmdQueue    = nullptr;   // borrowed from game
static ID3D12DescriptorHeap*    g_d12RtvHeap     = nullptr;   // one RTV / back buffer
static ID3D12DescriptorHeap*    g_d12SrvHeap     = nullptr;   // 1 SRV for ImGui font
static ID3D12GraphicsCommandList* g_d12CmdList   = nullptr;
static UINT                     g_d12BufferCount = 0;
static UINT                     g_d12RtvStride   = 0;
static DXGI_FORMAT              g_d12RtvFormat   = DXGI_FORMAT_UNKNOWN;
static std::vector<ID3D12Resource*>        g_d12BackBuffers;
static std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> g_d12RtvHandles;
static std::vector<ID3D12CommandAllocator*> g_d12CmdAllocators;
static bool                     g_d12ResourcesReady = false; // per-buffer RTVs etc.
static bool                     g_d12ImGuiInit   = false;    // backend init once

static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;
// Whether we have *ever* installed our WndProc hook. ResizeBuffers causes
// ImGui device objects to be torn down and InitDX11/InitDX12 to run again;
// we must NOT re-install the WndProc hook on that second pass, otherwise
// g_originalWndProc ends up pointing back to HookedWndProc itself and any
// message generates infinite recursion → EXCEPTION_STACK_OVERFLOW.
static bool g_wndProcHooked = false;

// Cached VK resolved from scan code 0x29 (key immediately below Esc,
// = ² on FR AZERTY, ~/` on US QWERTY, § on DE QWERTZ, etc.).
static UINT g_toggleVk = 0;

// Fallback queue-capture hook. UE4's D3D12 swapchains are created such that
// IDXGISwapChain::GetDevice(IID_ID3D12CommandQueue) returns E_NOINTERFACE
// (0x80004002), so we can't get the queue directly. Instead we hook
// ID3D12CommandQueue::ExecuteCommandLists on the live game device and
// capture the first DIRECT queue that submits.
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsFn)(
    ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;
static bool  g_executeHookInstalled = false;
static void* g_executeHookSlot      = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_originalPresent = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffersFn g_originalResizeBuffers = nullptr;

namespace {

// ──────────────────────────────────────────────────────────────────────
// ZeusMod overlay palette — mirrors app/src/renderer/styles/main.css
// ──────────────────────────────────────────────────────────────────────
// Electron app uses a charcoal / deep-navy base with cyan + purple as the
// primary accent pair. Accent gradients run cyan → purple. Greens/ambers
// are reserved for status/state changes.

constexpr ImVec4 BG_0       = ImVec4(0.039f, 0.051f, 0.078f, 1.00f); // #0a0d14
constexpr ImVec4 BG_1       = ImVec4(0.063f, 0.078f, 0.118f, 1.00f); // #10141e
constexpr ImVec4 BG_2       = ImVec4(0.082f, 0.102f, 0.153f, 1.00f); // #151a27
constexpr ImVec4 BG_3       = ImVec4(0.110f, 0.133f, 0.200f, 1.00f); // #1c2233
constexpr ImVec4 BG_ELEV    = ImVec4(0.133f, 0.165f, 0.239f, 1.00f); // #222a3d
constexpr ImVec4 BG_HOVER   = ImVec4(0.149f, 0.188f, 0.286f, 1.00f); // #263049

constexpr ImVec4 BORDER_1   = ImVec4(0.122f, 0.153f, 0.220f, 1.00f); // #1f2738
constexpr ImVec4 BORDER_2   = ImVec4(0.165f, 0.200f, 0.314f, 1.00f); // #2a3350

constexpr ImVec4 TEXT_1     = ImVec4(0.918f, 0.941f, 1.000f, 1.00f); // #eaf0ff
constexpr ImVec4 TEXT_2     = ImVec4(0.620f, 0.655f, 0.753f, 1.00f); // #9ea7c0
constexpr ImVec4 TEXT_3     = ImVec4(0.361f, 0.400f, 0.518f, 1.00f); // #5c6684
constexpr ImVec4 TEXT_4     = ImVec4(0.235f, 0.271f, 0.376f, 1.00f); // #3c4560

constexpr ImVec4 ACC_CYAN   = ImVec4(0.000f, 0.824f, 1.000f, 1.00f); // #00d2ff
constexpr ImVec4 ACC_CYAN_D = ImVec4(0.000f, 0.561f, 0.698f, 1.00f); // #008fb2
constexpr ImVec4 ACC_PURPLE = ImVec4(0.545f, 0.357f, 1.000f, 1.00f); // #8b5bff
constexpr ImVec4 ACC_GREEN  = ImVec4(0.098f, 0.882f, 0.525f, 1.00f); // #19e186
constexpr ImVec4 ACC_AMBER  = ImVec4(1.000f, 0.651f, 0.239f, 1.00f); // #ffa63d
constexpr ImVec4 ACC_RED    = ImVec4(1.000f, 0.290f, 0.369f, 1.00f); // #ff4a5e
constexpr ImVec4 TOGGLE_OFF = ImVec4(0.173f, 0.204f, 0.286f, 1.00f); // #2c3449

ImVec4 Accent()     { return ACC_CYAN; }
ImVec4 AccentSoft() { return ImVec4(ACC_CYAN.x, ACC_CYAN.y, ACC_CYAN.z, 0.18f); }
ImVec4 Success()    { return ACC_GREEN; }
ImVec4 Danger()     { return ACC_RED; }
ImVec4 Muted()      { return TEXT_2; }
ImVec4 Subtle()     { return TEXT_3; }

ImU32 ToU32(const ImVec4& c, float aMul = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * aMul));
}

// Linear blend two ImVec4 colors. t ∈ [0,1].
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// Load the embedded Inter Medium TTF. Inter is the same face the Electron
// shell uses (see app/src/renderer/styles/main.css), so the in-game overlay
// and the launcher app are typographically identical. Inter is bundled
// inside the DLL via InterFont.h (~411 KB binary) so the look is
// guaranteed to be pixel-identical on every machine — no dependency on
// whatever Windows ships, locale, or user font preferences.
//
// Inter is licensed under SIL OFL 1.1; redistribution alongside the DLL
// is allowed without restriction.
//
// MUST be called between ImGui::CreateContext() and the backend Init —
// the atlas is uploaded to GPU memory by the backend, so any font added
// after that is silently discarded.
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();   // drop any previous default font.

    // The TTF blob lives in static .rdata; ImGui copies it internally when
    // FontDataOwnedByAtlas is true (default). We pass FALSE because our
    // const buffer must not be free()'d at atlas teardown.
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH          = 3;
    cfg.OversampleV          = 1;
    cfg.PixelSnapH           = false;
    cfg.RasterizerMultiply   = 1.05f;   // small density boost — thin strokes
    cfg.GlyphExtraSpacing.x  = 0.2f;    // slightly looser tracking, "pro" feel

    ImFont* picked = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(InterFont_data),
        static_cast<int>(InterFont_size),
        17.0f, &cfg, io.Fonts->GetGlyphRangesDefault());
    if (picked) {
        io.FontDefault = picked;
        LOG_RENDER("font loaded: Inter Medium (embedded, %u bytes) @ 17 px",
            InterFont_size);
    } else {
        io.Fonts->AddFontDefault();
        LOG_RENDER("embedded Inter load failed — using ImGui default");
    }
}

void ApplyStyle() {
    ImGui::StyleColorsDark();
    auto& st = ImGui::GetStyle();

    st.WindowPadding      = ImVec2(0.0f, 0.0f);     // we draw our own chrome
    st.WindowRounding     = 12.0f;
    st.WindowBorderSize   = 1.0f;
    st.ChildRounding      = 12.0f;
    st.ChildBorderSize    = 1.0f;
    st.FrameRounding      = 8.0f;
    st.FrameBorderSize    = 1.0f;
    st.FramePadding       = ImVec2(11.0f, 7.0f);
    st.ItemSpacing        = ImVec2(10.0f, 9.0f);
    st.ItemInnerSpacing   = ImVec2(8.0f, 5.0f);
    st.PopupRounding      = 10.0f;
    st.GrabRounding       = 8.0f;
    st.TabRounding        = 8.0f;
    st.ScrollbarRounding  = 6.0f;
    st.ScrollbarSize      = 10.0f;
    st.SeparatorTextBorderSize = 1.0f;

    st.Colors[ImGuiCol_WindowBg]            = BG_0;
    st.Colors[ImGuiCol_ChildBg]             = BG_1;
    st.Colors[ImGuiCol_PopupBg]             = BG_2;
    st.Colors[ImGuiCol_Border]              = BORDER_1;
    st.Colors[ImGuiCol_BorderShadow]        = ImVec4(0,0,0,0);
    st.Colors[ImGuiCol_FrameBg]             = BG_2;
    st.Colors[ImGuiCol_FrameBgHovered]      = BG_3;
    st.Colors[ImGuiCol_FrameBgActive]       = BG_ELEV;
    st.Colors[ImGuiCol_TitleBg]             = BG_1;
    st.Colors[ImGuiCol_TitleBgActive]       = BG_1;
    st.Colors[ImGuiCol_TitleBgCollapsed]    = BG_0;
    st.Colors[ImGuiCol_MenuBarBg]           = BG_1;
    st.Colors[ImGuiCol_ScrollbarBg]         = ImVec4(0,0,0,0);
    st.Colors[ImGuiCol_ScrollbarGrab]       = BORDER_2;
    st.Colors[ImGuiCol_ScrollbarGrabHovered]= BG_ELEV;
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ACC_CYAN_D;
    st.Colors[ImGuiCol_Button]              = BG_3;
    st.Colors[ImGuiCol_ButtonHovered]       = BG_HOVER;
    st.Colors[ImGuiCol_ButtonActive]        = ACC_CYAN_D;
    st.Colors[ImGuiCol_CheckMark]           = ACC_CYAN;
    st.Colors[ImGuiCol_SliderGrab]          = ACC_CYAN;
    st.Colors[ImGuiCol_SliderGrabActive]    = ACC_CYAN;
    st.Colors[ImGuiCol_Header]              = ImVec4(ACC_CYAN.x, ACC_CYAN.y, ACC_CYAN.z, 0.10f);
    st.Colors[ImGuiCol_HeaderHovered]       = ImVec4(ACC_CYAN.x, ACC_CYAN.y, ACC_CYAN.z, 0.18f);
    st.Colors[ImGuiCol_HeaderActive]        = ImVec4(ACC_CYAN.x, ACC_CYAN.y, ACC_CYAN.z, 0.26f);
    st.Colors[ImGuiCol_Separator]           = BORDER_1;
    st.Colors[ImGuiCol_SeparatorHovered]    = BORDER_2;
    st.Colors[ImGuiCol_SeparatorActive]     = ACC_CYAN;
    st.Colors[ImGuiCol_ResizeGrip]          = AccentSoft();
    st.Colors[ImGuiCol_ResizeGripHovered]   = ACC_CYAN;
    st.Colors[ImGuiCol_ResizeGripActive]    = ACC_CYAN;
    st.Colors[ImGuiCol_Text]                = TEXT_1;
    st.Colors[ImGuiCol_TextDisabled]        = TEXT_3;
    st.Colors[ImGuiCol_PlotHistogram]       = ACC_CYAN;
    st.Colors[ImGuiCol_PlotLines]           = ACC_PURPLE;
    st.Colors[ImGuiCol_NavHighlight]        = ACC_CYAN;
    st.Colors[ImGuiCol_DragDropTarget]      = ACC_CYAN;
}

float Ratio(int value, int maxValue) {
    if (maxValue <= 0) return 0.0f;
    return std::clamp(static_cast<float>(value) / static_cast<float>(maxValue), 0.0f, 1.0f);
}

// Soft drop-shadow under a card. Cheap multi-pass alpha-blended rect.
void DrawSoftShadow(ImDrawList* dl, ImVec2 p1, ImVec2 p2, float rounding, float intensity = 0.45f) {
    for (int i = 1; i <= 4; ++i) {
        float t = (float)i;
        ImU32 col = IM_COL32(0, 0, 0, (int)(intensity * 255.0f * (0.22f - 0.045f * t)));
        dl->AddRect(ImVec2(p1.x - t, p1.y - t + 2.0f),
                    ImVec2(p2.x + t, p2.y + t + 2.0f),
                    col, rounding + t, 0, 1.5f);
    }
}

// Draws a horizontal rectangle that fades from left→right between two colours.
// Used for the cyan→purple "active card" top accent and gradient buttons.
void DrawHGradient(ImDrawList* dl, ImVec2 p1, ImVec2 p2, const ImVec4& a, const ImVec4& b) {
    ImU32 ca = ImGui::GetColorU32(a);
    ImU32 cb = ImGui::GetColorU32(b);
    dl->AddRectFilledMultiColor(p1, p2, ca, cb, cb, ca);
}

// Pill-shaped status indicator: filled background + outline + colored dot + label.
// The dot subtly glows via two stacked circles.
void DrawStatusPill(const char* label, const ImVec4& color, float alphaBg = 0.14f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label);
    float h = ts.y + 12.0f;
    ImVec2 sz(ts.x + 30.0f, h);
    ImVec2 p2(p.x + sz.x, p.y + sz.y);
    float r = h * 0.5f;
    dl->AddRectFilled(p, p2, ToU32(color, alphaBg), r);
    dl->AddRect(p, p2, ToU32(color, 0.45f), r, 0, 1.0f);
    ImVec2 dot(p.x + 12.0f, p.y + h * 0.5f);
    dl->AddCircleFilled(dot, 5.5f, ToU32(color, 0.30f));   // glow
    dl->AddCircleFilled(dot, 3.5f, ToU32(color));
    dl->AddText(ImVec2(p.x + 22.0f, p.y + (h - ts.y) * 0.5f),
                ToU32(TEXT_1), label);
    ImGui::Dummy(sz);
}

// Compact stat tile — title, big value, slim cyan progress bar, subtitle.
// Used in the top "metrics" row (HP / Stamina / Armor).
void DrawMetricCard(const char* title, int value, int maxValue, const ImVec4& color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(ImGui::GetContentRegionAvail().x, 96.0f);
    ImVec2 p2(p.x + size.x, p.y + size.y);

    DrawSoftShadow(dl, p, p2, 12.0f, 0.5f);
    dl->AddRectFilled(p, p2, ToU32(BG_2), 12.0f);
    dl->AddRect(p, p2, ToU32(BORDER_1), 12.0f, 0, 1.0f);
    // Subtle top-edge highlight (mimics CSS linear-gradient top sheen).
    dl->AddLine(ImVec2(p.x + 12.0f, p.y + 1.0f),
                ImVec2(p2.x - 12.0f, p.y + 1.0f),
                ToU32(ImVec4(1,1,1,0.04f)), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + 10.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    char valueText[40];
    std::snprintf(valueText, sizeof(valueText), "%d / %d", value, maxValue);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + 28.0f));
    ImGui::SetWindowFontScale(1.34f);
    ImGui::TextUnformatted(valueText);
    ImGui::SetWindowFontScale(1.0f);

    // Progress bar: rounded pill, gradient fill cyan→purple-tinted-color.
    ImVec2 barP1(p.x + 14.0f, p.y + size.y - 22.0f);
    ImVec2 barP2(p2.x - 14.0f, p.y + size.y - 14.0f);
    dl->AddRectFilled(barP1, barP2, ToU32(BG_0), 4.0f);
    float r = Ratio(value, maxValue);
    if (r > 0.0f) {
        ImVec2 fillP2(barP1.x + (barP2.x - barP1.x) * r, barP2.y);
        dl->AddRectFilledMultiColor(barP1, fillP2,
            ToU32(color), ToU32(Mix(color, ACC_CYAN, 0.35f)),
            ToU32(Mix(color, ACC_CYAN, 0.35f)), ToU32(color));
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + size.y - 11.0f));
    // Stops the metric card from claiming any extra ImGui-tracked layout
    // space — Dummy reserves the precise rectangle we drew into.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(size);
}

// CSS .toggle / .toggle-slider — a 44×24 rounded pill.
// OFF: matte slate fill, knob slightly inset.
// ON : cyan→purple-dim gradient fill with a soft cyan glow halo.
bool DrawToggleSwitch(const char* id, bool* v) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = 44.0f, h = 22.0f, knob = 16.0f;
    ImGui::InvisibleButton("##sw", ImVec2(w, h));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p2(p.x + w, p.y + h);
    if (*v) {
        // Glow halo
        dl->AddRectFilled(ImVec2(p.x - 2.0f, p.y - 2.0f),
                          ImVec2(p2.x + 2.0f, p2.y + 2.0f),
                          ToU32(ACC_CYAN, hovered ? 0.30f : 0.20f), h * 0.65f);
        // Cyan→purple gradient fill
        dl->AddRectFilledMultiColor(p, p2,
            ToU32(ACC_CYAN_D), ToU32(ACC_CYAN),
            ToU32(ACC_PURPLE), ToU32(Mix(ACC_CYAN_D, ACC_PURPLE, 0.5f)));
        dl->AddCircleFilled(ImVec2(p2.x - h * 0.5f, p.y + h * 0.5f),
                            knob * 0.5f, ToU32(ImVec4(1,1,1,1)));
    } else {
        dl->AddRectFilled(p, p2, ToU32(TOGGLE_OFF), h * 0.5f);
        dl->AddRect(p, p2, ToU32(BORDER_2), h * 0.5f, 0, 1.0f);
        dl->AddCircleFilled(ImVec2(p.x + h * 0.5f, p.y + h * 0.5f),
                            knob * 0.5f,
                            ToU32(ImVec4(0.89f, 0.92f, 1.0f, hovered ? 1.0f : 0.85f)));
    }
    ImGui::PopID();
    return clicked;
}

// Full cheat card: glyph chip, title, subtitle, body text, toggle.
// Active state lights up a 2-px cyan→purple bar at the top edge and
// switches the glyph chip to a gradient fill (mirrors .cheat-card.active
// in main.css).
//
// Card height is computed dynamically from the wrapped description so a
// long description never clips the bottom edge (the previous fixed
// 124-px height was cutting off the second line of multi-paragraph
// descriptions).
void DrawCheatCard(const char* id, const char* glyph, const char* title,
                   const char* sub, const char* desc, bool* value) {
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;

    constexpr float headerH      = 64.0f;   // glyph + title block
    constexpr float descTopPad   = 8.0f;    // space below the hairline divider
    constexpr float descBotPad   = 16.0f;   // space between description and card bottom
    float wrapW = w - 28.0f;
    ImVec2 descSize = ImGui::CalcTextSize(desc, nullptr, false, wrapW);
    float h = headerH + descTopPad + descSize.y + descBotPad;
    if (h < 110.0f) h = 110.0f;             // floor so single-line cards still feel substantial

    ImVec2 p2(p.x + w, p.y + h);
    bool active = *value;

    DrawSoftShadow(dl, p, p2, 12.0f, active ? 0.55f : 0.40f);
    dl->AddRectFilled(p, p2, ToU32(BG_2), 12.0f);
    dl->AddRect(p, p2,
        ToU32(active ? ACC_CYAN : BORDER_1, active ? 0.55f : 1.0f),
        12.0f, 0, active ? 1.5f : 1.0f);
    if (active) {
        // Top accent bar — cyan → purple
        DrawHGradient(dl,
            ImVec2(p.x + 1.0f, p.y + 1.0f),
            ImVec2(p2.x - 1.0f, p.y + 3.0f),
            ACC_CYAN, ACC_PURPLE);
    }

    // Glyph chip (36×36) on the left.
    ImVec2 chipP(p.x + 14.0f, p.y + 14.0f);
    ImVec2 chipP2(chipP.x + 36.0f, chipP.y + 36.0f);
    if (active) {
        dl->AddRectFilledMultiColor(chipP, chipP2,
            ToU32(ACC_CYAN), ToU32(ACC_PURPLE),
            ToU32(ACC_PURPLE), ToU32(ACC_CYAN));
    } else {
        dl->AddRectFilled(chipP, chipP2, ToU32(BG_3), 8.0f);
        dl->AddRect(chipP, chipP2, ToU32(BORDER_1), 8.0f, 0, 1.0f);
    }
    ImVec2 gs = ImGui::CalcTextSize(glyph);
    ImGui::SetCursorScreenPos(ImVec2(chipP.x + (36.0f - gs.x) * 0.5f,
                                     chipP.y + (36.0f - gs.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? BG_0 : ACC_CYAN);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();

    // Title + subtitle, right of the chip.
    ImGui::SetCursorScreenPos(ImVec2(p.x + 60.0f, p.y + 16.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_1);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    if (sub && *sub) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 60.0f, p.y + 33.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted(sub);
        ImGui::PopStyleColor();
    }

    // Toggle in the top-right corner.
    ImGui::SetCursorScreenPos(ImVec2(p2.x - 14.0f - 44.0f, p.y + 19.0f));
    DrawToggleSwitch("toggle", value);

    // Description below, separated by a hairline.
    float dividerY = p.y + headerH - 4.0f;
    dl->AddLine(ImVec2(p.x + 14.0f, dividerY),
                ImVec2(p2.x - 14.0f, dividerY),
                ToU32(BORDER_1), 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, dividerY + descTopPad));
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_2);
    ImGui::PushTextWrapPos(p2.x - 14.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // Reserve the layout box (with a small gap below) so subsequent ImGui
    // items flow correctly.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h + 10.0f));
    ImGui::PopID();
}

// Sidebar nav row with a left "active" rail. Returns true on click.
bool DrawSidebarItem(const char* glyph, const char* label, bool active) {
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 38.0f;
    ImGui::InvisibleButton("##nav", ImVec2(w, h));
    bool clicked = ImGui::IsItemClicked();
    bool hovered = ImGui::IsItemHovered();
    ImVec2 p2(p.x + w, p.y + h);

    if (active) {
        // Soft cyan/purple tinted background.
        dl->AddRectFilledMultiColor(p, p2,
            ToU32(ACC_CYAN, 0.10f), ToU32(ACC_PURPLE, 0.08f),
            ToU32(ACC_PURPLE, 0.08f), ToU32(ACC_CYAN, 0.10f));
        dl->AddRect(p, p2, ToU32(ACC_CYAN, 0.30f), 8.0f, 0, 1.0f);
        // Left rail bar with glow.
        dl->AddRectFilled(ImVec2(p.x - 2.0f, p.y + 8.0f),
                          ImVec2(p.x + 2.0f, p2.y - 8.0f),
                          ToU32(ACC_CYAN), 1.5f);
        dl->AddRectFilled(ImVec2(p.x - 4.0f, p.y + 8.0f),
                          ImVec2(p.x + 2.0f, p2.y - 8.0f),
                          ToU32(ACC_CYAN, 0.25f), 1.5f);
    } else if (hovered) {
        dl->AddRectFilled(p, p2, ToU32(BG_2), 8.0f);
    }

    ImVec4 textCol = active ? TEXT_1 : (hovered ? TEXT_1 : TEXT_2);
    ImVec2 gs = ImGui::CalcTextSize(glyph);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + (h - gs.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ACC_CYAN : textCol);
    ImGui::TextUnformatted(glyph);
    ImGui::PopStyleColor();
    ImVec2 ls = ImGui::CalcTextSize(label);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 40.0f, p.y + (h - ls.y) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, textCol);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    ImGui::PopID();
    return clicked;
}

// Quick "preset" pill button (used by Temporal Lock 06:00 / 12:00 / etc.).
void DrawPresetButton(const char* label, float value, float* target) {
    ImGui::PushStyleColor(ImGuiCol_Button, BG_3);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BG_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ACC_CYAN_D);
    if (ImGui::Button(label, ImVec2(72.0f, 0.0f))) *target = value;
    ImGui::PopStyleColor(3);
}

// Section heading with a small leading colored bar (mirrors .cat-head h2 in css).
// Reserves layout space that fully covers the (scaled) title + subtitle line so
// the next ImGui widget never overlaps it. Title uses font scale 1.18 → about
// 21 px tall; subtitle is ~17 px tall; we add 14 px of breathing room below.
void DrawSectionHeader(const char* title, const char* subtitle = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();

    // Vertical accent bar
    dl->AddRectFilledMultiColor(
        ImVec2(p.x, p.y + 3.0f),
        ImVec2(p.x + 4.0f, p.y + 24.0f),
        ToU32(ACC_CYAN), ToU32(ACC_PURPLE),
        ToU32(ACC_PURPLE), ToU32(ACC_CYAN));

    // Title (we draw via the draw list so we can keep our own cursor advance
    // exact regardless of font-scale rounding inside ImGui::TextUnformatted).
    ImGui::SetWindowFontScale(1.18f);
    ImVec2 titleSize = ImGui::CalcTextSize(title);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_1);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

    float advance = titleSize.y + 6.0f;
    if (subtitle && *subtitle) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f, p.y + advance));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
        advance += ImGui::GetTextLineHeight() + 14.0f;
    } else {
        advance += 10.0f;
    }
    // Reset cursor + reserve the full block so the caller can keep flowing.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, advance));
}

// Begin a content card — child window with rounded BG_2 fill + border.
// Default height = 0 → auto-fit to inner content. Pass an explicit height
// only when you actually want to clip / scroll the inner region.
void BeginContentCard(const char* id, float height = 0.0f) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BG_2);
    ImGui::PushStyleColor(ImGuiCol_Border, BORDER_1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (height <= 0.0f) flags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild(id, ImVec2(0.0f, height), flags);
}
void EndContentCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));    // breathing room below every card
}

// Categories — each id maps to a content panel rendered on the right.
enum class Cat : int {
    Survival = 0,
    Resources,
    Movement,
    Multiplayer,
    Time,
    Items,
    Diagnostics,
    Count
};

struct CatInfo {
    const char* glyph;
    const char* label;
    const char* heading;
    const char* subhead;
};

constexpr CatInfo kCats[] = {
    { "+",   "Survival",     "Survival",
        "Health, stamina, armor and life-support overrides." },
    { "#",   "Resources",    "Resources & Crafting",
        "Free craft, infinite items, weight, progression." },
    { ">",   "Movement",     "Movement",
        "Speed override and time-dilation controls." },
    { "@",   "Multiplayer",  "Multiplayer",
        "Mirror survival writes to every player on the server." },
    { "T",   "Time Lock",    "Temporal Lock",
        "Pin world time to a fixed hour." },
    { "*",   "Item Spawner", "Item Spawner",
        "Give any inventory item from the resolved library." },
    { "?",   "Diagnostics",  "Diagnostics",
        "Reflection dumps and placement-bug tooling." },
};
static_assert(sizeof(kCats)/sizeof(kCats[0]) == (int)Cat::Count,
    "kCats size out of sync with Cat enum");

// ── Category panels ─────────────────────────────────────────────────
void DrawCat_Survival(Trainer& trainer) {
    DrawCheatCard("god", "+", "God Mode",
        "Per-player health rewrite",
        "Keeps the local player's HP at max every tick. Animals and AI "
        "still take damage normally — no global write hooks.",
        &trainer.GodMode);
    DrawCheatCard("stam", "S", "Infinite Stamina",
        "Local + remote players",
        "Pins Stamina to MaxStamina each tick. Mirrors to remote players "
        "if 'Apply To All Players' is on.",
        &trainer.InfiniteStamina);
    DrawCheatCard("armor", "A", "Infinite Armor",
        "Equip armor first",
        "Continuously refills armor to MaxArmor — has no effect when no "
        "armor piece is equipped.",
        &trainer.InfiniteArmor);
    DrawCheatCard("oxygen", "O", "Infinite Oxygen",
        "Underwater & oxygen biomes",
        "Keeps the oxygen meter full on the player's character state.",
        &trainer.InfiniteOxygen);
    DrawCheatCard("food", "F", "Infinite Food",
        "Nutrition bar pinned",
        "Restores food every tick. Hunger debuffs never trigger.",
        &trainer.InfiniteFood);
    DrawCheatCard("water", "W", "Infinite Water",
        "Hydration pinned",
        "Restores water every tick. Thirst debuffs never trigger.",
        &trainer.InfiniteWater);

    BeginContentCard("temp_card");
    DrawSectionHeader("Stable Temperature",
        "Clamp ModifiedInternalTemperature so biome temperatures are ignored.");
    ImGui::Checkbox("Enabled##temp", &trainer.StableTemperature);
    ImGui::SameLine(0.0f, 18.0f);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##temp_target", &trainer.StableTempValue, -40, 60,
        "Target  %d C");
    EndContentCard();
}

void DrawCat_Resources(Trainer& trainer) {
    DrawCheatCard("craft", "C", "Free Craft",
        "Bypass recipe ingredient checks",
        "Removes resource cost from local crafts. Output items are "
        "delivered with the correct dyn shape so they place / stack "
        "without corruption.",
        &trainer.FreeCraft);
    DrawCheatCard("items", "I", "Infinite Items",
        "Quantity + durability pinned",
        "Stops inventory items from decrementing on use and pins tool / "
        "armor durability at max (999,999).",
        &trainer.InfiniteItems);
    DrawCheatCard("weight", "K", "No Weight",
        "Encumbrance forced to zero",
        "Hooks AddModifierState so the encumbrance modifier never sticks.",
        &trainer.NoWeight);
    DrawCheatCard("megaexp", "X", "Mega Exp",
        "+50,000 XP / tick",
        "Pumps experience points into the talent system so the character "
        "levels up visibly while the cheat is on.",
        &trainer.MegaExp);
}

void DrawCat_Movement(Trainer& trainer) {
    DrawCheatCard("speedhack", ">", "Speed Override",
        "Custom time-dilation",
        "Multiplies the character's CustomTimeDilation by the slider value. "
        "Affects animation playback as well as movement.",
        &trainer.SpeedHack);

    BeginContentCard("speed_slider");
    DrawSectionHeader("Speed Multiplier", "1.0x = normal, 6.0x = fastest");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##speed", &trainer.SpeedMultiplier, 1.0f, 6.0f, "x%.1f");
    EndContentCard();
}

void DrawCat_Multiplayer(Trainer& trainer) {
    DrawCheatCard("mp_broadcast", "@", "Apply To All Players",
        "Host-side mirror — only effective on the listen-server",
        "Mirrors GodMode / Stamina / Armor / Oxygen / Food / Water writes "
        "onto every player whose state class exactly matches the local "
        "player's. Animals and AI are excluded by class-pointer match. "
        "Has no effect on a client.",
        &trainer.ApplyToAllPlayers);

    BeginContentCard("mp_notes", 0.0f);
    DrawSectionHeader("Notes",
        "Why writes are gated on host-side authority.");
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_2);
    ImGui::TextWrapped(
        "Icarus uses authoritative server replication: any value the "
        "client writes locally to a SurvivalCharacterState gets clobbered "
        "on the next replication frame from the server. Enabling this "
        "from a client therefore has no effect — only the listening host "
        "can drive other players' values.\n\n"
        "Class-pointer matching is the only safe filter: AI / creature "
        "subclasses share the inheritance chain but reorder fields, so "
        "writing State_Health into them previously corrupted MaterialInstance "
        "and produced render-thread AVs. The mirror loop reads each "
        "candidate's UClass at +0x10 every tick and skips anything that "
        "isn't bit-compatible.");
    ImGui::PopStyleColor();
    EndContentCard();
}

void DrawCat_Time(Trainer& trainer) {
    DrawCheatCard("timelock", "T", "Time Lock",
        "Pin world hour",
        "Locks prospect time replication to the slider value. Useful for "
        "screenshots, building at golden hour, or staying in night-vision "
        "biomes without the day/night cycle moving on.",
        &trainer.TimeLock);

    BeginContentCard("time_slider");
    DrawSectionHeader("Locked Hour", "Slider snaps to nearest 0.1 h.");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##time", &trainer.LockedTime, 0.0f, 24.0f, "%.1f h");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    DrawPresetButton("06:00", 6.0f, &trainer.LockedTime);
    ImGui::SameLine(); DrawPresetButton("12:00", 12.0f, &trainer.LockedTime);
    ImGui::SameLine(); DrawPresetButton("18:00", 18.0f, &trainer.LockedTime);
    ImGui::SameLine(); DrawPresetButton("00:00",  0.0f, &trainer.LockedTime);
    EndContentCard();
}

void DrawCat_Items(Trainer& trainer) {
    (void)trainer;
    static char s_filterBuf[96] = "";
    static int  s_giveCount    = 1;
    static char s_lastGiven[96] = "";
    static bool s_lastOk = false;

    BeginContentCard("give_top");
    DrawSectionHeader("Give item",
        "Type to filter the resolved item-row list. Click a row to deliver.");

    ImGui::SetNextItemWidth(-200.0f);
    ImGui::InputTextWithHint("##filter",
        "start typing — wood, iron, ammo, beam, …",
        s_filterBuf, sizeof(s_filterBuf));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderInt("##count", &s_giveCount, 1, 999, "Count  %d");

    if (s_lastGiven[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, s_lastOk ? ACC_GREEN : ACC_RED);
        ImGui::Text("Last: %s  %s", s_lastGiven, s_lastOk ? "delivered" : "REJECTED");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted("Library size:");
        ImGui::SameLine();
        ImGui::Text("%zu items", Trainer_GiveItem_GetAllNames().size());
        ImGui::PopStyleColor();
    }
    EndContentCard();

    ImGui::Spacing();

    std::string filter;
    for (const char* p = s_filterBuf; *p; ++p)
        filter.push_back((char)tolower((unsigned char)*p));

    BeginContentCard("give_list", ImGui::GetContentRegionAvail().y);
    const auto& names = Trainer_GiveItem_GetAllNames();
    int shown = 0;
    for (const std::string& n : names) {
        if (!filter.empty() && n.find(filter) == std::string::npos) continue;
        if (shown++ > 400) {
            ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
            ImGui::TextUnformatted("…more results hidden, tighten filter");
            ImGui::PopStyleColor();
            break;
        }
        ImGui::PushID(n.c_str());
        ImVec2 rowP = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##row", ImVec2(rowW, 28.0f));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered) {
            dl->AddRectFilled(rowP,
                ImVec2(rowP.x + rowW, rowP.y + 28.0f),
                ToU32(BG_HOVER), 6.0f);
        }
        ImGui::SetCursorScreenPos(ImVec2(rowP.x + 10.0f, rowP.y + 6.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_1);
        ImGui::TextUnformatted(n.c_str());
        ImGui::PopStyleColor();
        // "Give" pill on the right edge.
        ImVec2 pillP(rowP.x + rowW - 80.0f, rowP.y + 4.0f);
        ImVec2 pillP2(pillP.x + 70.0f, pillP.y + 20.0f);
        DrawHGradient(dl, pillP, pillP2, ACC_CYAN_D, ACC_CYAN);
        const char* gv = "GIVE";
        ImVec2 gs = ImGui::CalcTextSize(gv);
        dl->AddText(ImVec2(pillP.x + (70.0f - gs.x) * 0.5f,
                           pillP.y + (20.0f - gs.y) * 0.5f),
                    ToU32(BG_0), gv);
        if (clicked) {
            bool ok = Trainer_GiveItem(n.c_str(), s_giveCount);
            std::snprintf(s_lastGiven, sizeof(s_lastGiven), "%s x%d",
                n.c_str(), s_giveCount);
            s_lastOk = ok;
        }
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted("no match");
        ImGui::PopStyleColor();
    }
    EndContentCard();
}

void DrawCat_Diagnostics(Trainer& /*trainer*/) {
    BeginContentCard("diag_buttons");
    DrawSectionHeader("One-shot reflection dumps",
        "Output goes to DebugView (DBWIN_BUFFER) — run DebugView64 as admin to capture.");

    ImGui::PushStyleColor(ImGuiCol_Button, BG_3);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, BG_HOVER);
    if (ImGui::Button("Dump Build / Deploy Functions", ImVec2(260.0f, 34.0f))) {
        printf("\n===== [DIAG] Dumping Build/Deploy functions (v2) =====\n");
        const char* known[] = {
            "BuildingSubsystem", "BuildingGridManagerSubsystem",
            "BuildingFunctionLibrary", "BuildingLookupLibrary",
            "BuildingPiecesLibrary", "BuildableLibrary",
            "BuildingBase", "BuildingGridBase",
            "GridObjectPlacementComponent",
            "PrebuildStructureFunctionLibrary",
            "BuildableComponent", "Deployable",
            "DeployableLibrary", "DeployableSubsystem",
            "DeployableManagerSubsystem", "DeployableSetupLibrary",
            "Inventory", "InventoryComponent",
            "IcarusInventoryComponent", "InventoryItemLibrary",
            "IcarusPlayerState",
            "IcarusCheatManager", "CheatManager",
        };
        for (const char* k : known) {
            uintptr_t cls = UObjectLookup::FindClassByName(k);
            if (cls) {
                printf("[DIAG] === %s @ 0x%p ===\n", k, (void*)cls);
                UObjectLookup::DumpFunctionsOf(cls, 400);
            } else {
                printf("[DIAG] class not found: %s\n", k);
            }
        }
        printf("===== [DIAG] end Build/Deploy dump (v2) =====\n\n");
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
    ImGui::TextUnformatted("Heavy — iterates ~25 known classes' UFunction children.");
    ImGui::PopStyleColor();
    ImGui::PopStyleColor(2);
    EndContentCard();
}

void DrawMenu() {
    auto& trainer = Trainer::Get();
    trainer.SpeedMultiplier = std::clamp(trainer.SpeedMultiplier, 1.0f, 6.0f);
    trainer.LockedTime      = std::clamp(trainer.LockedTime, 0.0f, 24.0f);

    static int s_activeCat = (int)Cat::Survival;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 winSize(1080.0f, 720.0f);
    ImGui::SetNextWindowSize(winSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + (vp->WorkSize.x - winSize.x) * 0.5f,
               vp->WorkPos.y + (vp->WorkSize.y - winSize.y) * 0.5f),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(900.0f, 560.0f),
                                        ImVec2(2400.0f, 1600.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGuiWindowFlags wflags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("##zeusmod_root", &g_menuOpen, wflags)) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    // ── Title bar (drag handle, brand, status pill, close) ──────────
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        float ww = ImGui::GetWindowSize().x;
        const float titleH = 46.0f;
        ImVec2 tp1 = wp;
        ImVec2 tp2(wp.x + ww, wp.y + titleH);
        dl->AddRectFilledMultiColor(tp1, tp2,
            ToU32(BG_1), ToU32(BG_1),
            ToU32(BG_0), ToU32(BG_0));
        dl->AddLine(ImVec2(tp1.x, tp2.y), ImVec2(tp2.x, tp2.y),
                    ToU32(BORDER_1), 1.0f);

        // Brand mark — a small cyan→purple gradient square.
        ImVec2 logoP(wp.x + 16.0f, wp.y + 13.0f);
        dl->AddRectFilledMultiColor(logoP,
            ImVec2(logoP.x + 20.0f, logoP.y + 20.0f),
            ToU32(ACC_CYAN), ToU32(ACC_PURPLE),
            ToU32(ACC_PURPLE), ToU32(ACC_CYAN));
        dl->AddRectFilled(logoP, ImVec2(logoP.x + 20.0f, logoP.y + 20.0f),
            IM_COL32(0,0,0,40), 4.0f);

        ImGui::SetCursorPos(ImVec2(46.0f, 8.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_1);
        ImGui::TextUnformatted("ZeusMod");
        ImGui::PopStyleColor();
        ImGui::SetCursorPos(ImVec2(46.0f, 24.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted("Icarus  ·  v1.5.1");
        ImGui::PopStyleColor();

        // Status pill on the right side.
        const char* statusLabel = trainer.IsReady() ? "PLAYER ONLINE" : "SEARCHING";
        ImVec4 statusColor      = trainer.IsReady() ? ACC_GREEN : ACC_AMBER;
        ImVec2 ts = ImGui::CalcTextSize(statusLabel);
        ImGui::SetCursorPos(ImVec2(ww - (ts.x + 60.0f), 11.0f));
        DrawStatusPill(statusLabel, statusColor);
    }

    // ── Two-column body: sidebar | content ──────────────────────────
    ImGui::SetCursorPos(ImVec2(0.0f, 47.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, BG_1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 14.0f));
    ImGui::BeginChild("sidebar", ImVec2(220.0f, 0.0f), 0,
        ImGuiWindowFlags_NoScrollbar);
    {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::SetCursorPosX(14.0f);
        ImGui::TextUnformatted("CHEAT CATEGORIES");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        for (int i = 0; i < (int)Cat::Count; ++i) {
            if (DrawSidebarItem(kCats[i].glyph, kCats[i].label,
                                s_activeCat == i)) {
                s_activeCat = i;
            }
        }

        // Footer: live HP / Stamina mini-readout.
        float footerY = ImGui::GetWindowHeight() - 96.0f;
        ImGui::SetCursorPosY(footerY);
        ImGui::PushStyleColor(ImGuiCol_Separator, BORDER_1);
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        ImGui::SetCursorPosX(14.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_3);
        ImGui::TextUnformatted("LIVE TELEMETRY");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(14.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_2);
        ImGui::Text("HP   %d / %d",
            trainer.GetHealth(), trainer.GetMaxHealth());
        ImGui::SetCursorPosX(14.0f);
        ImGui::Text("STA  %d / %d",
            trainer.GetStamina(), trainer.GetMaxStamina());
        ImGui::SetCursorPosX(14.0f);
        ImGui::Text("ARM  %d / %d",
            trainer.GetArmor(), trainer.GetMaxArmor());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // Content panel
    ImGui::SetCursorPos(ImVec2(220.0f, 47.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 22.0f));
    ImGui::BeginChild("content", ImVec2(0.0f, 0.0f), 0);
    {
        // Status strip — three metric cards across the top of every panel.
        if (ImGui::BeginTable("metrics", 3,
                ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_NoPadOuterX |
                ImGuiTableFlags_NoPadInnerX,
                ImVec2(0.0f, 0.0f), 12.0f)) {
            ImGui::TableNextColumn();
            DrawMetricCard("Health",  trainer.GetHealth(),  trainer.GetMaxHealth(),  ACC_RED);
            ImGui::TableNextColumn();
            DrawMetricCard("Stamina", trainer.GetStamina(), trainer.GetMaxStamina(), ACC_GREEN);
            ImGui::TableNextColumn();
            DrawMetricCard("Armor",   trainer.GetArmor(),   trainer.GetMaxArmor(),   ACC_PURPLE);
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0.0f, 14.0f));

        // Section heading + subtitle for the current category.
        const CatInfo& ci = kCats[s_activeCat];
        DrawSectionHeader(ci.heading, ci.subhead);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        switch ((Cat)s_activeCat) {
            case Cat::Survival:    DrawCat_Survival(trainer);    break;
            case Cat::Resources:   DrawCat_Resources(trainer);   break;
            case Cat::Movement:    DrawCat_Movement(trainer);    break;
            case Cat::Multiplayer: DrawCat_Multiplayer(trainer); break;
            case Cat::Time:        DrawCat_Time(trainer);        break;
            case Cat::Items:       DrawCat_Items(trainer);       break;
            case Cat::Diagnostics: DrawCat_Diagnostics(trainer); break;
            default: break;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar();
}

LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_menuOpen && g_imguiReady) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 0;
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
        if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR || msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
            return 0;
    }
    return CallWindowProcW(g_originalWndProc, hWnd, msg, wParam, lParam);
}

bool InitDX11(IDXGISwapChain* swapChain) {
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_d11Device))) || !g_d11Device)
        return false;

    g_d11Device->GetImmediateContext(&g_d11Context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer)
        return false;

    g_d11Device->CreateRenderTargetView(backBuffer, nullptr, &g_d11RTV);
    backBuffer->Release();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    LoadFonts();      // must happen BEFORE backend Init — atlas upload point
    ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d11Device, g_d11Context);
    if (!g_wndProcHooked) {
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(HookedWndProc)));
        g_wndProcHooked = true;
    }
    LOG_RENDER("DX11 ImGui initialized.");
    return true;
}

// ID3D12CommandQueue::ExecuteCommandLists hook — captures the game's first
// DIRECT queue and stores it in g_d12CmdQueue. Chained call to the original
// so the game continues normally.
void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT num, ID3D12CommandList* const* lists) {
    if (!g_d12CmdQueue && queue) {
        D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            g_d12CmdQueue = queue;
            g_d12CmdQueue->AddRef();        // balanced in Shutdown()
            LOG_RENDER("captured D3D12 DIRECT queue via hook @ %p", (void*)queue);
        }
    }
    g_originalExecuteCommandLists(queue, num, lists);
}

// Install the ExecuteCommandLists hook on the live game device. A throwaway
// queue is created on the SAME device just to read the vtable slot (10 on
// ID3D12CommandQueue, stable across Win10/11), then released. Having
// g_d12Device already populated by the caller is required.
bool InstallExecuteCommandListsHook() {
    if (g_executeHookInstalled || !g_d12Device) return g_executeHookInstalled;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmpQ = nullptr;
    HRESULT hr = g_d12Device->CreateCommandQueue(
        &qd, __uuidof(ID3D12CommandQueue),
        reinterpret_cast<void**>(&tmpQ));
    if (FAILED(hr) || !tmpQ) {
        LOG_RENDER("temp queue for vtable read failed: 0x%lX", hr);
        return false;
    }
    g_executeHookSlot = (*reinterpret_cast<void***>(tmpQ))[10];
    tmpQ->Release();

    MH_STATUS cr = MH_CreateHook(
        g_executeHookSlot, HookedExecuteCommandLists,
        reinterpret_cast<void**>(&g_originalExecuteCommandLists));
    if (cr != MH_OK && cr != MH_ERROR_ALREADY_CREATED) {
        LOG_RENDER("MH_CreateHook(ExecuteCommandLists) failed: %d", static_cast<int>(cr));
        return false;
    }
    MH_STATUS en = MH_EnableHook(g_executeHookSlot);
    if (en != MH_OK && en != MH_ERROR_ENABLED) {
        LOG_RENDER("MH_EnableHook(ExecuteCommandLists) failed: %d", static_cast<int>(en));
        return false;
    }
    g_executeHookInstalled = true;
    LOG_RENDER("ExecuteCommandLists hook armed — waiting for queue capture");
    return true;
}

// Release per-buffer DX12 resources (RTVs, back buffer refs, allocators).
// Kept separate so ResizeBuffers can flush them and rebuild next Present
// without tearing down the ImGui backend or re-capturing the queue.
void ReleaseDX12PerBuffer() {
    for (auto* bb : g_d12BackBuffers)    if (bb) bb->Release();
    for (auto* a  : g_d12CmdAllocators) if (a)  a->Release();
    g_d12BackBuffers.clear();
    g_d12RtvHandles.clear();
    g_d12CmdAllocators.clear();
    if (g_d12CmdList) { g_d12CmdList->Release(); g_d12CmdList = nullptr; }
    if (g_d12RtvHeap) { g_d12RtvHeap->Release(); g_d12RtvHeap = nullptr; }
    g_d12ResourcesReady = false;
}

// Create per-buffer DX12 resources against the current swapchain dimensions.
bool CreateDX12PerBuffer(IDXGISwapChain* swapChain,
                         const DXGI_SWAP_CHAIN_DESC& desc) {
    g_d12BufferCount = desc.BufferCount;
    g_d12RtvFormat   = desc.BufferDesc.Format;

    // RTV heap for each back buffer
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = g_d12BufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_d12Device->CreateDescriptorHeap(
            &rtvHeapDesc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void**>(&g_d12RtvHeap)))) {
        LOG_RENDER("DX12 RTV heap creation failed"); return false;
    }
    g_d12RtvStride = g_d12Device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // RTV per back buffer + matching command allocator
    g_d12BackBuffers.resize(g_d12BufferCount, nullptr);
    g_d12RtvHandles.resize(g_d12BufferCount);
    g_d12CmdAllocators.resize(g_d12BufferCount, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_d12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_d12BufferCount; ++i) {
        if (FAILED(swapChain->GetBuffer(
                i, __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(&g_d12BackBuffers[i])))) {
            LOG_RENDER("DX12 GetBuffer(%u) failed", i); return false;
        }
        g_d12Device->CreateRenderTargetView(g_d12BackBuffers[i], nullptr, h);
        g_d12RtvHandles[i] = h;
        h.ptr += g_d12RtvStride;

        if (FAILED(g_d12Device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                __uuidof(ID3D12CommandAllocator),
                reinterpret_cast<void**>(&g_d12CmdAllocators[i])))) {
            LOG_RENDER("DX12 cmd allocator[%u] failed", i); return false;
        }
    }

    // Single command list, reset per-frame against the current allocator
    if (FAILED(g_d12Device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d12CmdAllocators[0],
            nullptr, __uuidof(ID3D12GraphicsCommandList),
            reinterpret_cast<void**>(&g_d12CmdList)))) {
        LOG_RENDER("DX12 cmd list creation failed"); return false;
    }
    g_d12CmdList->Close();   // start closed; Reset() opens it each frame
    g_d12ResourcesReady = true;
    return true;
}

// Native D3D12 overlay init — uses the official imgui_impl_dx12 backend
// with the game's own command queue. No D3D11On12, no second device, no
// cross-queue barrier races.
bool InitDX12(IDXGISwapChain* swapChain) {
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D12Device),
                                      reinterpret_cast<void**>(&g_d12Device));
    if (FAILED(hr) || !g_d12Device) {
        LOG_RENDER("GetDevice(DX12) failed: 0x%lX", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    // Capture the game's DIRECT queue so ImGui submissions share the same
    // timeline as the game's Present.
    //
    //   Primary : IDXGISwapChain::GetDevice(IID_ID3D12CommandQueue).
    //             Works when DXGI was asked to create the swapchain with the
    //             queue as the "device" (CreateSwapChainForHwnd(queue, …)).
    //             UE4 sometimes does this, sometimes doesn't depending on
    //             feature level / hwnd vs composition path.
    //   Fallback: hook ID3D12CommandQueue::ExecuteCommandLists on the live
    //             game device and steal the first DIRECT queue that submits.
    //             Mandatory on UE4 builds where GetDevice returns E_NOINTERFACE.
    if (!g_d12CmdQueue) {
        hr = swapChain->GetDevice(__uuidof(ID3D12CommandQueue),
                                  reinterpret_cast<void**>(&g_d12CmdQueue));
        if (SUCCEEDED(hr) && g_d12CmdQueue) {
            LOG_RENDER("captured DX12 queue from swapchain @ %p",
                       (void*)g_d12CmdQueue);
        } else {
            g_d12CmdQueue = nullptr;
            // Install the fallback hook on the *live* game device exactly once,
            // then wait for the capture on a subsequent Present.
            if (!g_executeHookInstalled) {
                InstallExecuteCommandListsHook();
            }
            static int warned = 0;
            if ((warned++ % 180) == 0) {   // ~3s at 60 fps
                LOG_RENDER("waiting for DX12 queue capture via hook "
                           "(GetDevice failed: 0x%lX, frame %d)", hr, warned);
            }
            return false;   // retry next frame
        }
    }

    // SRV heap that the ImGui font texture lives in. Shader-visible because
    // imgui_impl_dx12 binds it via SetGraphicsRootDescriptorTable.
    if (!g_d12SrvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = 1;     // just the font
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_d12Device->CreateDescriptorHeap(
                &srvHeapDesc, __uuidof(ID3D12DescriptorHeap),
                reinterpret_cast<void**>(&g_d12SrvHeap)))) {
            LOG_RENDER("DX12 SRV heap creation failed"); return false;
        }
    }

    if (!g_d12ResourcesReady && !CreateDX12PerBuffer(swapChain, desc)) {
        return false;
    }

    if (!g_d12ImGuiInit) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        LoadFonts();   // must precede the DX12 backend init — atlas upload
        ApplyStyle();
        ImGui_ImplWin32_Init(g_hwnd);

        ImGui_ImplDX12_InitInfo info{};
        info.Device            = g_d12Device;
        info.CommandQueue      = g_d12CmdQueue;
        info.NumFramesInFlight = static_cast<int>(g_d12BufferCount);
        info.RTVFormat         = g_d12RtvFormat;
        info.DSVFormat         = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_d12SrvHeap;
        info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*,
                                       D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
            *outCpu = g_d12SrvHeap->GetCPUDescriptorHandleForHeapStart();
            *outGpu = g_d12SrvHeap->GetGPUDescriptorHandleForHeapStart();
        };
        info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
                                      D3D12_CPU_DESCRIPTOR_HANDLE,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {};
        if (!ImGui_ImplDX12_Init(&info)) {
            LOG_RENDER("ImGui_ImplDX12_Init failed"); return false;
        }
        g_d12ImGuiInit = true;
    }

    if (!g_wndProcHooked) {
        g_originalWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(HookedWndProc)));
        g_wndProcHooked = true;
    }

    LOG_RENDER("DX12 native backend ready (buffers=%u format=0x%X)",
               g_d12BufferCount, g_d12RtvFormat);
    return true;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    ++g_frameCount;

    if (!g_imguiReady) {
        ID3D11Device* testDev = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&testDev))) && testDev) {
            testDev->Release();
            g_isDX12 = false;
            g_imguiReady = InitDX11(swapChain);
        } else {
            g_isDX12 = true;
            g_imguiReady = InitDX12(swapChain);
        }

        if (!g_imguiReady) {
            static bool logged = false;
            if (!logged) {
                LOG_RENDER("ImGui init failed. Overlay unavailable.");
                logged = true;
            }
            return g_originalPresent(swapChain, syncInterval, flags);
        }
    }

    // Menu toggle: key immediately below Esc. Layout-independent via scan code.
    // Scan code 0x29 = ² on FR AZERTY, ~/` on US QWERTY, § on DE QWERTZ, etc.
    if (!g_toggleVk) {
        g_toggleVk = MapVirtualKeyW(0x29, /*MAPVK_VSC_TO_VK*/ 1);
        if (!g_toggleVk) g_toggleVk = VK_OEM_7;   // safe default
    }
    static bool togglePrev = false;
    bool togglePressed = (GetAsyncKeyState(static_cast<int>(g_toggleVk)) & 0x8000) != 0;
    if (togglePressed && !togglePrev) {
        g_menuOpen = !g_menuOpen;
        printf("[MENU] %s\n", g_menuOpen ? "OPEN" : "CLOSED");
    }
    togglePrev = togglePressed;

    if (g_menuOpen && g_imguiReady) {
        if (g_isDX12) {
            // ── Native DX12 render path ──────────────────────────────────
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = true;
            DrawMenu();
            ImGui::Render();

            // Resolve current back-buffer index via IDXGISwapChain3.
            UINT bufIdx = 0;
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(
                    __uuidof(IDXGISwapChain3),
                    reinterpret_cast<void**>(&sc3))) && sc3) {
                bufIdx = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            }
            if (bufIdx >= g_d12BufferCount || !g_d12ResourcesReady ||
                !g_d12CmdList || !g_d12CmdAllocators[bufIdx] ||
                !g_d12BackBuffers[bufIdx]) {
                return g_originalPresent(swapChain, syncInterval, flags);
            }

            ID3D12CommandAllocator* alloc = g_d12CmdAllocators[bufIdx];
            alloc->Reset();
            g_d12CmdList->Reset(alloc, nullptr);

            // Transition PRESENT → RENDER_TARGET
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource   = g_d12BackBuffers[bufIdx];
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_d12CmdList->ResourceBarrier(1, &barrier);

            g_d12CmdList->OMSetRenderTargets(
                1, &g_d12RtvHandles[bufIdx], FALSE, nullptr);
            ID3D12DescriptorHeap* heaps[] = { g_d12SrvHeap };
            g_d12CmdList->SetDescriptorHeaps(1, heaps);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_d12CmdList);

            // Transition RENDER_TARGET → PRESENT
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            g_d12CmdList->ResourceBarrier(1, &barrier);
            g_d12CmdList->Close();

            ID3D12CommandList* lists[] = { g_d12CmdList };
            g_d12CmdQueue->ExecuteCommandLists(1, lists);
        } else {
            // ── DX11 render path (unchanged) ─────────────────────────────
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = true;
            DrawMenu();
            ImGui::Render();
            if (g_d11RTV) g_d11Context->OMSetRenderTargets(1, &g_d11RTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT swapFlags) {
    // Release per-buffer resources — DXGI requires all references on the
    // swapchain buffers to be gone before ResizeBuffers can proceed.
    if (g_d11RTV) { g_d11RTV->Release(); g_d11RTV = nullptr; }
    if (g_isDX12) {
        ReleaseDX12PerBuffer();
    } else if (g_imguiReady) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }

    HRESULT hr = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, swapFlags);
    // Trigger a re-init of per-buffer resources in the next HookedPresent.
    // In DX12 we keep the ImGui backend + device + queue + SRV heap alive
    // (no need to re-upload fonts, re-create PSOs, etc.); only the RTVs and
    // command allocators are rebuilt.
    g_imguiReady = false;
    return hr;
}

bool GetSwapChainVTable(void** vTable) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0,
        GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"IcarusModDummy", nullptr };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 2;
    scd.BufferDesc.Height = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &swap,
        &dev,
        &fl,
        &ctx);

    if (FAILED(hr) || !swap) {
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    std::memcpy(vTable, *reinterpret_cast<void***>(swap), 18 * sizeof(void*));
    swap->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return true;
}

} // namespace

bool Render::Initialize() {
    void* vTable[18]{};
    if (!GetSwapChainVTable(vTable)) {
        LOG_RENDER("Failed to acquire swapchain vtable.");
        return false;
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_RENDER("MinHook init failed: %d", static_cast<int>(init));
        return false;
    }

    MH_STATUS createPresent = MH_CreateHook(vTable[8], HookedPresent, reinterpret_cast<void**>(&g_originalPresent));
    if (createPresent != MH_OK && createPresent != MH_ERROR_ALREADY_CREATED) {
        LOG_RENDER("Present hook creation failed: %d", static_cast<int>(createPresent));
        return false;
    }

    MH_STATUS createResize = MH_CreateHook(vTable[13], HookedResizeBuffers, reinterpret_cast<void**>(&g_originalResizeBuffers));
    if (createResize != MH_OK && createResize != MH_ERROR_ALREADY_CREATED) {
        LOG_RENDER("ResizeBuffers hook creation failed: %d", static_cast<int>(createResize));
        return false;
    }

    MH_STATUS enablePresent = MH_EnableHook(vTable[8]);
    MH_STATUS enableResize = MH_EnableHook(vTable[13]);
    if ((enablePresent != MH_OK && enablePresent != MH_ERROR_ENABLED) ||
        (enableResize != MH_OK && enableResize != MH_ERROR_ENABLED)) {
        LOG_RENDER("Hook enabling failed: present=%d resize=%d", static_cast<int>(enablePresent), static_cast<int>(enableResize));
        return false;
    }

    // NOTE: do NOT create a temp D3D12 device here. Creating one at
    // DllMain time races against the game's D3D12 init and can cause
    // adapter/driver conflicts that manifest as DEVICE_REMOVED.
    // The ExecuteCommandLists hook, if needed, is installed lazily
    // inside InitDX12() once the game's live D3D12 device is available.

    LOG_RENDER("DirectX overlay armed. Press the key below Esc (² on FR AZERTY) to toggle the menu.");
    return true;
}

void Render::Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    // Restore the WndProc before ImGui tears down so late-arriving messages
    // reach the game's handler instead of a dangling hook.
    if (g_wndProcHooked && g_originalWndProc && g_hwnd) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_originalWndProc));
    }

    if (g_imguiReady || g_d12ImGuiInit) {
        if (g_d12ImGuiInit) ImGui_ImplDX12_Shutdown();
        else                ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    // ── DX11 cleanup ────────────────────────────────────────────────────
    if (g_d11RTV)     { g_d11RTV->Release();     g_d11RTV = nullptr; }
    if (g_d11Context) { g_d11Context->Release(); g_d11Context = nullptr; }
    if (g_d11Device)  { g_d11Device->Release();  g_d11Device = nullptr; }

    // ── DX12 cleanup ────────────────────────────────────────────────────
    // (anonymous-namespace helper; called via qualified name here since
    // Shutdown lives at file scope, not inside the anon namespace).
    extern void ReleaseDX12PerBuffer_NS();  // forward (see below)
    // Direct inline release since we can't invoke the anon-NS helper
    // from here without a trampoline.
    for (auto* bb : g_d12BackBuffers)   if (bb) bb->Release();
    for (auto* a  : g_d12CmdAllocators) if (a)  a->Release();
    g_d12BackBuffers.clear();
    g_d12RtvHandles.clear();
    g_d12CmdAllocators.clear();
    if (g_d12CmdList) { g_d12CmdList->Release(); g_d12CmdList = nullptr; }
    if (g_d12RtvHeap) { g_d12RtvHeap->Release(); g_d12RtvHeap = nullptr; }
    if (g_d12SrvHeap) { g_d12SrvHeap->Release(); g_d12SrvHeap = nullptr; }
    // CmdQueue borrowed from game — AddRef'd internally by GetDevice; matching
    // Release restores refcount. Do NOT null-out unless we've released.
    if (g_d12CmdQueue) { g_d12CmdQueue->Release(); g_d12CmdQueue = nullptr; }
    if (g_d12Device)   { g_d12Device->Release();   g_d12Device   = nullptr; }

    g_d12ResourcesReady   = false;
    g_d12ImGuiInit        = false;
    g_executeHookInstalled= false;
    g_executeHookSlot     = nullptr;
    g_originalExecuteCommandLists = nullptr;
    g_originalWndProc     = nullptr;
    g_wndProcHooked       = false;
    g_hwnd                = nullptr;
    g_imguiReady          = false;
    g_menuOpen            = false;
    g_toggleVk            = 0;
}

bool Render::IsMenuOpen() {
    return g_menuOpen;
}
