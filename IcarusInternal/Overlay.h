#pragma once
#include <Windows.h>

namespace Overlay {
    bool Create();
    void Destroy();
    void ProcessMessages();
    bool IsVisible();
    void Toggle();
}
