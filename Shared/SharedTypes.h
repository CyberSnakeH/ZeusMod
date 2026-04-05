#pragma once
#include <cstdint>
#include <Windows.h>

namespace IcarusMod {

enum class CheatID : uint32_t {
    InfiniteHealth = 0,
    InfiniteStamina,
    InfiniteOxygen,
    NoHungerThirst,
    InfiniteResources,
    InstantCraft,
    ItemDuplication,
    DamageMultiplier,
    OneShotKill,
    NoRecoil,
    SpeedHack,
    FlyMode,
    COUNT
};

constexpr uint32_t CHEAT_COUNT = static_cast<uint32_t>(CheatID::COUNT);

struct CheatInfo {
    const wchar_t* name;
    const wchar_t* category;
    int hotkey; // VK_F1 - VK_F12
};

inline constexpr CheatInfo CHEAT_TABLE[] = {
    { L"Infinite Health",      L"Survival",  VK_F1  },
    { L"Infinite Stamina",     L"Survival",  VK_F2  },
    { L"Infinite Oxygen",      L"Survival",  VK_F3  },
    { L"No Hunger/Thirst",     L"Survival",  VK_F4  },
    { L"Infinite Resources",   L"Resources", VK_F5  },
    { L"Instant Craft",        L"Resources", VK_F6  },
    { L"Item Duplication",     L"Resources", VK_F7  },
    { L"Damage Multiplier x10",L"Combat",    VK_F8  },
    { L"One-Shot Kill",        L"Combat",    VK_F9  },
    { L"No Recoil",            L"Combat",    VK_F10 },
    { L"Speed Hack (x2)",      L"Movement",  VK_F11 },
    { L"Fly Mode / No Fall",   L"Movement",  VK_F12 },
};

static_assert(sizeof(CHEAT_TABLE) / sizeof(CHEAT_TABLE[0]) == CHEAT_COUNT);

constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\IcarusModPipe";
constexpr const wchar_t* TARGET_PROCESS = L"Icarus-Win64-Shipping.exe";

enum class PipeCommand : uint32_t {
    ToggleCheat = 1,
    GetStatus   = 2,
    StatusReport = 3,
    Shutdown    = 4,
};

#pragma pack(push, 1)
struct PipeMessage {
    PipeCommand command;
    uint32_t cheatId;
    uint32_t value;
    uint32_t reserved;
};
#pragma pack(pop)

} // namespace IcarusMod
