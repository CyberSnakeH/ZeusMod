# ZeusMod - Internal Trainer for Icarus

> **Status: Prototype / Work in Progress**
> This project is still in active development. Some features work, others are being refined.

## Overview

ZeusMod is an internal cheat/trainer for the game [Icarus](https://store.steampowered.com/app/1149460/ICARUS/) (UE4 survival game by RocketWerkz). It uses DLL injection + direct memory access with SDK offsets obtained from a UE4SS dump.

Unlike external trainers that use `ReadProcessMemory`/`WriteProcessMemory`, ZeusMod runs **inside** the game process for instant, reliable memory access.

## Features

| Feature | Status | Method |
|---------|--------|--------|
| God Mode | Working | NOP `SetHealth` write instruction (AOB from CE) + health freeze |
| Infinite Stamina | Working | Direct memory write (offset `0x278`) |
| Infinite Armor | Working | Direct memory write (offset `0x1E0`) |
| Infinite Oxygen | Working | Direct memory write (offset `0x328`) |
| Infinite Food | Working | Direct memory write (offset `0x330`) |
| Infinite Water | Working | Direct memory write (offset `0x32C`) |
| Speed Hack | Working | `CustomTimeDilation` on player actor (adjustable x0.5 - x10) |
| Free Craft | WIP | `RemoveItem` NOP works but UI still greys out button without resources |

## Architecture

```
ZeusMod/
├── IcarusInjector/       # Win32 GUI app - injects DLL into game process
│   ├── main.cpp          # Entry point, attach flow
│   ├── GUI.h/cpp         # Dark theme Win32 window
│   ├── Injector.h/cpp    # CreateRemoteThread + LoadLibraryW injection
│   └── ProcessUtils.h/cpp# Process finder, SeDebugPrivilege
│
├── IcarusInternal/       # DLL injected into game - the actual trainer
│   ├── SDK.h             # Minimal UE4 structs (not used currently, offset-based approach)
│   ├── UE4.h             # GWorld pattern scan, player hierarchy traversal
│   ├── Trainer.h/cpp     # Cheat logic, AOB scanning, code patching
│   ├── Overlay.h/cpp     # Win32 overlay window with checkboxes
│   └── dllmain.cpp       # DLL entry, thread management
│
├── Shared/               # Common types between injector and DLL
│   ├── SharedTypes.h     # CheatID enum, pipe protocol structs
│   └── PipeProtocol.h/cpp# Named pipe IPC (legacy, not used in current version)
│
├── dumps/                # UE4SS SDK dump headers
│   ├── Icarus.hpp        # All Icarus game classes with offsets
│   ├── Icarus_enums.hpp  # Game enums
│   ├── Engine.hpp        # UE4 engine classes
│   └── CoreUObject.hpp   # UE4 core object system
│
└── IcarusMod.sln         # Visual Studio solution
```

## How It Works

### 1. Player Detection
The trainer scans for the UE4 `GWorld` pointer using an AOB pattern (`48 8B 1D ?? ?? ?? ?? 48 85 DB 74`), then walks the object hierarchy:

```
GWorld → UWorld (+0x0D28) → UGameInstance
  → (+0x38) TArray<ULocalPlayer*> → [0]
  → (+0x30) APlayerController
  → (+0x260) AIcarusCharacter
  → (+0x5A8) UCharacterState (ActorState)
```

It validates the player by checking that Health and Stamina values are in reasonable ranges (50-5000).

### 2. SDK Offsets (from UE4SS dump)

**UActorState:**
| Offset | Field | Type |
|--------|-------|------|
| `0x1D8` | Health | int32 |
| `0x1DC` | MaxHealth | int32 |
| `0x1E0` | Armor | int32 |
| `0x1E4` | MaxArmor | int32 |
| `0x210` | CurrentAliveState | uint8 |

**UCharacterState (extends UActorState):**
| Offset | Field | Type |
|--------|-------|------|
| `0x278` | Stamina | int32 |
| `0x27C` | MaxStamina | int32 |

**USurvivalCharacterState (extends UCharacterState):**
| Offset | Field | Type |
|--------|-------|------|
| `0x328` | OxygenLevel | int32 |
| `0x32C` | WaterLevel | int32 |
| `0x330` | FoodLevel | int32 |
| `0x338` | MaxOxygen | int32 |
| `0x340` | MaxWater | int32 |
| `0x348` | MaxFood | int32 |

**AActor:**
| Offset | Field | Type |
|--------|-------|------|
| `0x098` | CustomTimeDilation | float |

### 3. Code Patching (God Mode)
The God Mode uses a CE-confirmed AOB to find the health write instruction in `UActorState::SetHealth`:

```asm
79 04           jns +4
33 C0           xor eax, eax
EB 09           jmp +9
41 8B C0        mov eax, r8d
41 3B D0        cmp edx, r8d
0F 4C C2        cmovl eax, edx
89 81 D8010000  mov [rcx+0x1D8], eax  ← NOP this (6 bytes)
```

NOPing the final `mov` prevents any health changes. The trainer also writes `Health = MaxHealth` in a dedicated high-frequency thread.

### 4. Free Craft (WIP)
Currently NOPs the write instruction in `UInventory::RemoveItem` to prevent item consumption:

```asm
41 2B CF        sub ecx, r15d
89 48 04        mov [rax+04], ecx  ← NOP this (3 bytes)
```

Items are not consumed when crafting, but the UI still requires at least 1 of each resource.

## How to Use

### Prerequisites
- Visual Studio 2022/2025 with C++ desktop workload
- Icarus (Steam version)
- Run as Administrator

### Build
1. Open `IcarusMod.sln` in Visual Studio
2. Set configuration to `Release | x64`
3. Build the solution
4. Copy `IcarusInternal.dll` next to `IcarusInjector.exe`

### Run
1. Launch Icarus via Steam
2. Enter a prospect (be in-game, not the menu)
3. Run `IcarusInjector.exe` as Administrator
4. Click "Attach to Icarus"
5. The overlay window appears with checkboxes
6. Press **N** to show/hide the overlay
7. Check the cheats you want to enable

## Obtaining SDK Dumps

The `dumps/` folder contains UE4SS SDK headers. To regenerate them:

1. Download [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS/releases)
2. Inject it into Icarus (we provide `InjectUE4SS.exe` in tools)
3. Press `Ctrl+H` in-game to dump the SDK
4. Headers are generated in `ue4ss/CXXHeaderDump/`

## Known Issues

- **Free Craft**: UI still requires at least 1 of each resource. Full zero-cost crafting needs UI verification bypass (WIP)
- **God Mode**: Debuff removal (injuries, poison) is partially working via `RemoveDebuffs()` but some effects may persist
- **Speed Hack**: Affects all player actions (mining, crafting animations) since it uses `CustomTimeDilation`
- **Game Updates**: AOB signatures and offsets may break after game updates. Re-run UE4SS dump to get new offsets

## Technical Notes

- **DX12**: Icarus uses DX12, not DX11. ImGui overlay via DX hook crashes the game. We use a Win32 overlay window instead.
- **Anti-cheat**: Icarus has no kernel anti-cheat. Standard DLL injection works.
- **Multiplayer**: This trainer is designed for single-player/private sessions only.
- **Offsets**: All offsets verified against Icarus April 2026 build via UE4SS CXXHeaderDump.

## License

For educational purposes only. Use at your own risk.
