#include "Render.h"
#include "Trainer.h"
#include "Logger.h"
#include "libs/minhook/include/MinHook.h"
#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_impl_win32.h"
#include "libs/imgui/imgui_impl_dx11.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3d12.h>

#pragma comment(lib, "d3d12.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool g_menuOpen = false;
static bool g_imguiReady = false;
static bool g_isDX12 = false;
static int g_frameCount = 0;

static ID3D11Device* g_d11Device = nullptr;
static ID3D11DeviceContext* g_d11Context = nullptr;
static ID3D11RenderTargetView* g_d11RTV = nullptr;

static ID3D12Device* g_d12Device = nullptr;
static ID3D12CommandQueue* g_d12CmdQueue = nullptr;
static ID3D11Device* g_d11on12Device = nullptr;
static ID3D11DeviceContext* g_d11on12Context = nullptr;
static ID3D11On12Device* g_d11on12 = nullptr;
static ID3D11RenderTargetView* g_d12RTV = nullptr;

static HWND g_hwnd = nullptr;
static WNDPROC g_originalWndProc = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn g_originalPresent = nullptr;

typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffersFn g_originalResizeBuffers = nullptr;

namespace {

ImVec4 Accent() { return ImVec4(0.83f, 0.64f, 0.27f, 1.0f); }
ImVec4 AccentSoft() { return ImVec4(0.83f, 0.64f, 0.27f, 0.18f); }
ImVec4 Success() { return ImVec4(0.17f, 0.72f, 0.49f, 1.0f); }
ImVec4 Danger() { return ImVec4(0.82f, 0.29f, 0.26f, 1.0f); }
ImVec4 Muted() { return ImVec4(0.56f, 0.60f, 0.66f, 1.0f); }

void ApplyStyle() {
    ImGui::StyleColorsDark();
    auto& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18.0f, 18.0f);
    st.FramePadding = ImVec2(12.0f, 8.0f);
    st.ItemSpacing = ImVec2(12.0f, 10.0f);
    st.ItemInnerSpacing = ImVec2(10.0f, 6.0f);
    st.ScrollbarSize = 12.0f;
    st.WindowRounding = 16.0f;
    st.ChildRounding = 14.0f;
    st.FrameRounding = 10.0f;
    st.PopupRounding = 10.0f;
    st.GrabRounding = 10.0f;
    st.TabRounding = 10.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;

    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.06f, 0.08f, 0.94f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.09f, 0.12f, 0.96f);
    st.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.27f, 0.90f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.13f, 0.17f, 1.0f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.17f, 0.22f, 1.0f);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.20f, 0.26f, 1.0f);
    st.Colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.06f, 0.08f, 1.0f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.18f, 0.15f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.27f, 0.21f, 0.13f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = Accent();
    st.Colors[ImGuiCol_SliderGrab] = Accent();
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.92f, 0.75f, 0.40f, 1.0f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.17f, 0.22f, 1.0f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.20f, 0.16f, 1.0f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.21f, 0.13f, 1.0f);
    st.Colors[ImGuiCol_Separator] = ImVec4(0.19f, 0.20f, 0.24f, 1.0f);
    st.Colors[ImGuiCol_ResizeGrip] = AccentSoft();
    st.Colors[ImGuiCol_ResizeGripHovered] = Accent();
    st.Colors[ImGuiCol_ResizeGripActive] = Accent();
    st.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.0f);
    st.Colors[ImGuiCol_TextDisabled] = Muted();
    st.Colors[ImGuiCol_PlotHistogram] = Accent();
}

float Ratio(int value, int maxValue) {
    if (maxValue <= 0) return 0.0f;
    return std::clamp(static_cast<float>(value) / static_cast<float>(maxValue), 0.0f, 1.0f);
}

void DrawStatusPill(const char* label, const ImVec4& color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImVec2(ImGui::CalcTextSize(label).x + 24.0f, 28.0f);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.18f)), 14.0f);
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.42f)), 14.0f, 0, 1.0f);
    dl->AddCircleFilled(ImVec2(p.x + 12.0f, p.y + sz.y * 0.5f), 4.0f, ImGui::GetColorU32(color));
    dl->AddText(ImVec2(p.x + 22.0f, p.y + 6.0f), ImGui::GetColorU32(ImGuiCol_Text), label);
    ImGui::Dummy(sz);
}

void DrawMetricCard(const char* title, int value, int maxValue, const ImVec4& color) {
    ImVec2 size(0.0f, 92.0f);
    ImGui::BeginChild(title, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    char valueText[32];
    std::snprintf(valueText, sizeof(valueText), "%d / %d", value, maxValue);
    ImGui::SetWindowFontScale(1.22f);
    ImGui::TextUnformatted(valueText);
    ImGui::SetWindowFontScale(1.0f);

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.13f, 0.17f, 1.0f));
    ImGui::ProgressBar(Ratio(value, maxValue), ImVec2(-1.0f, 10.0f));
    ImGui::PopStyleColor(2);
    ImGui::TextDisabled("Live telemetry");
    ImGui::EndChild();
}

void DrawFeatureRow(const char* id, const char* label, const char* description, bool* value) {
    ImGui::PushID(id);
    if (ImGui::BeginTable("row", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("toggle", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(label);
        ImGui::TextDisabled("%s", description);

        ImGui::TableSetColumnIndex(1);
        float y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(y + 8.0f);
        ImGui::Checkbox("##enabled", value);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PopID();
}

void DrawPresetButton(const char* label, float value, float* target) {
    if (ImGui::Button(label, ImVec2(72.0f, 0.0f))) {
        *target = value;
    }
}

void DrawMenu() {
    auto& trainer = Trainer::Get();
    trainer.SpeedMultiplier = std::clamp(trainer.SpeedMultiplier, 1.0f, 6.0f);
    trainer.LockedTime = std::clamp(trainer.LockedTime, 0.0f, 24.0f);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 winSize(920.0f, 640.0f);
    ImGui::SetNextWindowSize(winSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - winSize.x) * 0.5f,
        vp->WorkPos.y + (vp->WorkSize.y - winSize.y) * 0.5f), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("Icarus Control Center", &g_menuOpen, flags)) {
        ImGui::End();
        return;
    }

    ImGui::BeginChild("hero", ImVec2(0.0f, 98.0f), true);
    ImGui::TextColored(Accent(), "ICARUSMOD");
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted("Command Interface");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("High-clarity internal overlay for live Icarus runtime control.");
    ImGui::SameLine(ImGui::GetWindowWidth() - 170.0f);
    if (trainer.IsReady()) {
        DrawStatusPill("PLAYER ONLINE", Success());
    } else {
        DrawStatusPill("SEARCHING", Danger());
    }
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::BeginTable("metrics", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableNextColumn();
        DrawMetricCard("Health", trainer.GetHealth(), trainer.GetMaxHealth(), ImVec4(0.86f, 0.35f, 0.29f, 1.0f));
        ImGui::TableNextColumn();
        DrawMetricCard("Stamina", trainer.GetStamina(), trainer.GetMaxStamina(), ImVec4(0.26f, 0.67f, 0.46f, 1.0f));
        ImGui::TableNextColumn();
        DrawMetricCard("Armor", trainer.GetArmor(), trainer.GetMaxArmor(), ImVec4(0.35f, 0.55f, 0.86f, 1.0f));
        ImGui::EndTable();
    }

    ImGui::Spacing();

    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 0.48f);

        ImGui::TableNextColumn();
        ImGui::BeginChild("core", ImVec2(0.0f, 0.0f), true);
        ImGui::TextColored(Accent(), "Core Systems");
        ImGui::TextDisabled("Primary safeguards and core runtime controls.");
        ImGui::Spacing();
        DrawFeatureRow("god", "God Mode", "Stabilizes health and neutralizes harmful effects.", &trainer.GodMode);
        DrawFeatureRow("stam", "Infinite Stamina", "Removes stamina drain from the local character.", &trainer.InfiniteStamina);
        DrawFeatureRow("armor", "Infinite Armor", "Continuously restores the player's armor pool.", &trainer.InfiniteArmor);
        DrawFeatureRow("craft", "Free Craft", "Bypasses local craft checks and item consumption.", &trainer.FreeCraft);
        DrawFeatureRow("weight", "No Weight", "Forces inventory weight to zero.", &trainer.NoWeight);

        ImGui::Spacing();
        ImGui::TextColored(Accent(), "Mobility");
        ImGui::TextDisabled("Direct control over character time dilation.");
        ImGui::Spacing();
        ImGui::Checkbox("Enable Speed Override", &trainer.SpeedHack);
        ImGui::SliderFloat("Speed Multiplier", &trainer.SpeedMultiplier, 1.0f, 6.0f, "x%.1f");

        ImGui::Spacing();
        ImGui::TextColored(Accent(), "Progression");
        ImGui::TextDisabled("Experience pump — grants +50 000 XP/tick and clamps Level.");
        ImGui::Spacing();
        DrawFeatureRow("megaexp", "Mega Exp", "Grants XP continuously so the character levels up visibly.", &trainer.MegaExp);
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("world", ImVec2(0.0f, 0.0f), true);
        ImGui::TextColored(Accent(), "Survival & World");
        ImGui::TextDisabled("Biological resources and world time management.");
        ImGui::Spacing();
        DrawFeatureRow("oxygen", "Infinite Oxygen", "Keeps oxygen at its maximum level.", &trainer.InfiniteOxygen);
        DrawFeatureRow("food", "Infinite Food", "Keeps the nutrition bar full.", &trainer.InfiniteFood);
        DrawFeatureRow("water", "Infinite Water", "Keeps hydration fully restored.", &trainer.InfiniteWater);
        DrawFeatureRow("temp",  "Stable Temperature", "Clamps ModifiedInternalTemperature so biome temps are ignored.", &trainer.StableTemperature);
        ImGui::SliderInt("Target Temperature (C)", &trainer.StableTempValue, -40, 60, "%d C");

        ImGui::Spacing();
        ImGui::TextColored(Accent(), "Temporal Lock");
        ImGui::TextDisabled("Locks prospect time replication to a fixed target hour.");
        ImGui::Spacing();
        ImGui::Checkbox("Enable Time Lock", &trainer.TimeLock);
        ImGui::SliderFloat("Locked Hour", &trainer.LockedTime, 0.0f, 24.0f, "%.1f h");
        DrawPresetButton("06:00", 6.0f, &trainer.LockedTime);
        ImGui::SameLine();
        DrawPresetButton("12:00", 12.0f, &trainer.LockedTime);
        ImGui::SameLine();
        DrawPresetButton("18:00", 18.0f, &trainer.LockedTime);
        ImGui::SameLine();
        DrawPresetButton("00:00", 0.0f, &trainer.LockedTime);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(Muted(), "Runtime Notes");
        ImGui::BulletText("N: open or close the panel");
        ImGui::BulletText("F10: unload the module");
        ImGui::BulletText("The menu captures mouse and keyboard input while open");
        ImGui::EndChild();

        ImGui::EndTable();
    }

    ImGui::End();
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
    ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d11Device, g_d11Context);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    LOG_RENDER("DX11 ImGui initialized.");
    return true;
}

bool InitDX12(IDXGISwapChain* swapChain) {
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&g_d12Device));
    if (FAILED(hr) || !g_d12Device) {
        LOG_RENDER("GetDevice(DX12) failed: 0x%lX", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    if (!g_d12CmdQueue) {
        D3D12_COMMAND_QUEUE_DESC qDesc{};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = g_d12Device->CreateCommandQueue(&qDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_d12CmdQueue));
        if (FAILED(hr) || !g_d12CmdQueue) {
            LOG_RENDER("DX12 queue creation failed: 0x%lX", hr);
            return false;
        }
    }

    hr = D3D11On12CreateDevice(
        g_d12Device,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0,
        reinterpret_cast<IUnknown**>(&g_d12CmdQueue), 1,
        0,
        &g_d11on12Device,
        &g_d11on12Context,
        nullptr);

    if (FAILED(hr) || !g_d11on12Device || !g_d11on12Context) {
        LOG_RENDER("D3D11On12CreateDevice failed: 0x%lX", hr);
        return false;
    }

    hr = g_d11on12Device->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void**>(&g_d11on12));
    if (FAILED(hr) || !g_d11on12) {
        LOG_RENDER("QI ID3D11On12Device failed: 0x%lX", hr);
        return false;
    }

    ID3D12Resource* d12BackBuffer = nullptr;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&d12BackBuffer));
    if (FAILED(hr) || !d12BackBuffer) {
        LOG_RENDER("DX12 GetBuffer failed: 0x%lX", hr);
        return false;
    }

    D3D11_RESOURCE_FLAGS flags11 = { D3D11_BIND_RENDER_TARGET };
    ID3D11Resource* wrappedBuffer = nullptr;
    hr = g_d11on12->CreateWrappedResource(
        d12BackBuffer,
        &flags11,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT,
        __uuidof(ID3D11Resource),
        reinterpret_cast<void**>(&wrappedBuffer));
    d12BackBuffer->Release();

    if (FAILED(hr) || !wrappedBuffer) {
        LOG_RENDER("CreateWrappedResource failed: 0x%lX", hr);
        return false;
    }

    hr = g_d11on12Device->CreateRenderTargetView(wrappedBuffer, nullptr, &g_d12RTV);
    wrappedBuffer->Release();
    if (FAILED(hr) || !g_d12RTV) {
        LOG_RENDER("DX12 RTV creation failed: 0x%lX", hr);
        return false;
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyStyle();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_d11on12Device, g_d11on12Context);
    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    LOG_RENDER("DX12 ImGui initialized.");
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

    static bool nWasPressed = false;
    bool nPressed = (GetAsyncKeyState(0x4E) & 0x8000) != 0;
    if (nPressed && !nWasPressed) {
        g_menuOpen = !g_menuOpen;
        printf("[MENU] %s\n", g_menuOpen ? "OPEN" : "CLOSED");
    }
    nWasPressed = nPressed;

    if (g_menuOpen && g_imguiReady) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::GetIO().MouseDrawCursor = true;

        DrawMenu();

        ImGui::Render();
        if (g_isDX12 && g_d12RTV) {
            g_d11on12Context->OMSetRenderTargets(1, &g_d12RTV, nullptr);
        } else if (g_d11RTV) {
            g_d11Context->OMSetRenderTargets(1, &g_d11RTV, nullptr);
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (g_isDX12 && g_d11on12Context) {
            g_d11on12Context->Flush();
        }
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT swapFlags) {
    if (g_d11RTV) { g_d11RTV->Release(); g_d11RTV = nullptr; }
    if (g_d12RTV) { g_d12RTV->Release(); g_d12RTV = nullptr; }
    if (g_imguiReady) ImGui_ImplDX11_InvalidateDeviceObjects();

    HRESULT hr = g_originalResizeBuffers(swapChain, bufferCount, width, height, format, swapFlags);
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

    LOG_RENDER("DirectX overlay armed. Press N to open menu.");
    return true;
}

void Render::Shutdown() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imguiReady) {
        if (g_originalWndProc && g_hwnd) {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWndProc));
        }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (g_d11RTV) { g_d11RTV->Release(); g_d11RTV = nullptr; }
    if (g_d12RTV) { g_d12RTV->Release(); g_d12RTV = nullptr; }
    if (g_d11on12) { g_d11on12->Release(); g_d11on12 = nullptr; }
    if (g_d11on12Context) { g_d11on12Context->Release(); g_d11on12Context = nullptr; }
    if (g_d11on12Device) { g_d11on12Device->Release(); g_d11on12Device = nullptr; }
    if (g_d12CmdQueue) { g_d12CmdQueue->Release(); g_d12CmdQueue = nullptr; }
    if (g_d12Device) { g_d12Device->Release(); g_d12Device = nullptr; }
    if (g_d11Context) { g_d11Context->Release(); g_d11Context = nullptr; }
    if (g_d11Device) { g_d11Device->Release(); g_d11Device = nullptr; }

    g_originalWndProc = nullptr;
    g_hwnd = nullptr;
    g_imguiReady = false;
    g_menuOpen = false;
}

bool Render::IsMenuOpen() {
    return g_menuOpen;
}
