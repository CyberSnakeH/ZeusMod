#pragma once
// ============================================================================
// DX11 Hook + ImGui Rendering
// Hooks IDXGISwapChain::Present to render ImGui overlay
// Uses MinHook for function hooking
// ============================================================================

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi.h>

#pragma comment(lib, "d3d12.lib")
#include "imgui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Render {
    bool Initialize();
    void Shutdown();
    bool IsMenuOpen();
}
