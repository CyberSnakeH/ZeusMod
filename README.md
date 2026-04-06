<p align="center">
  <h1 align="center">⚡ ZeusMod</h1>
  <p align="center">
    <strong>Internal Trainer for Icarus</strong><br>
    <em>Made by CyberSnake</em>
  </p>
  <p align="center">
    <img src="https://img.shields.io/badge/Game-Icarus-blue?style=for-the-badge" alt="Game">
    <img src="https://img.shields.io/badge/Engine-Unreal%20Engine%204-orange?style=for-the-badge" alt="Engine">
    <img src="https://img.shields.io/badge/Language-C++-00599C?style=for-the-badge&logo=cplusplus" alt="C++">
    <img src="https://img.shields.io/badge/Status-Prototype-yellow?style=for-the-badge" alt="Status">
  </p>
</p>

---

## 📥 Download

> **[⬇ Download Latest Release](https://github.com/CyberSnakeH/ZeusMod/releases)**

Download `ZeusMod-v1.0.zip`, extract, and run `IcarusInjector.exe` as Administrator.

---

## ✨ Features

| Feature | Description | Method |
|:--------|:------------|:-------|
| 🛡️ **God Mode** | Invincible — health stays at max, no debuffs | AOB NOP on `SetHealth` write instruction |
| ⚡ **Infinite Stamina** | Sprint forever | Direct memory write (offset `0x278`) |
| 🛡️ **Infinite Armor** | Armor never breaks | Direct memory write (offset `0x1E0`) |
| 💨 **Infinite Oxygen** | Breathe underwater forever | Direct memory write (offset `0x328`) |
| 🍖 **No Hunger/Thirst** | Never starve or dehydrate | Direct memory write (offsets `0x330`, `0x32C`) |
| 🔨 **Free Craft** | Craft anything without resources (shows 9999/0) | Patches `GetScaledRecipeInputCount`, `FindItemCountByType`, `ConsumeItem` |
| 🎒 **No Weight Limit** | Carry unlimited items | Patches `GetTotalWeight` to return 0 |
| 🏃 **Speed Hack** | Adjustable speed (x0.5 to x10) | `CustomTimeDilation` on player actor |
| 🌅 **Time Lock** | Lock time of day (dawn/noon/dusk/midnight) | Writes `TimeOfDay` float on `GameState` |

---

## 🚀 How to Use

1. **Launch Icarus** via Steam and enter a prospect (be in-game)
2. **Run `IcarusInjector.exe`** as Administrator
3. Click **ATTACH TO ICARUS**
4. The in-game overlay appears — click toggles to enable cheats
5. Press **N** to show/hide the overlay
6. **Right-click** the overlay to cycle Time Lock hours
7. Press **F10** to detach and exit

---

## 🏗️ Architecture

```
ZeusMod/
├── IcarusInjector/          # Win32 GUI — DLL injection
│   ├── main.cpp             # Attach flow, process detection
│   ├── GUI.h/cpp            # Custom GDI neon UI with toggles
│   ├── Injector.h/cpp       # CreateRemoteThread + LoadLibraryW
│   └── ProcessUtils.h/cpp   # Process finder, SeDebugPrivilege
│
├── IcarusInternal/          # DLL injected into game process
│   ├── UE4.h                # GWorld scanner, player hierarchy walker
│   ├── Trainer.h/cpp        # All cheat logic, AOB scanning, code patching
│   ├── Overlay.h/cpp        # In-game Win32 overlay with toggles
│   └── dllmain.cpp          # DLL entry, thread management
│
├── Shared/                  # Common types
│   └── SharedTypes.h        # Cheat IDs, target process name
│
└── dumps/                   # UE4SS SDK dump headers
    ├── Icarus.hpp           # All game classes with offsets
    ├── Icarus_enums.hpp     # Game enumerations
    ├── Engine.hpp           # UE4 engine classes
    └── CoreUObject.hpp      # UE4 core object system
```

---

## 🔬 How Signatures Were Found

### Step 1: SDK Dump with UE4SS

We used [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) to dump the complete SDK of Icarus at runtime. UE4SS was injected into the game process, then `Ctrl+H` generated C++ headers with all class definitions and offsets.

This gave us the exact memory layout of every game class:

```cpp
// UActorState — from UE4SS CXXHeaderDump/Icarus.hpp
int32 Health;              // 0x1D8
int32 MaxHealth;           // 0x1DC
int32 Armor;               // 0x1E0

// UCharacterState (extends UActorState)
int32 Stamina;             // 0x278
int32 MaxStamina;          // 0x27C

// USurvivalCharacterState (extends UCharacterState)
int32 OxygenLevel;         // 0x328
int32 WaterLevel;          // 0x32C
int32 FoodLevel;           // 0x330
```

### Step 2: Finding Write Instructions with Cheat Engine

For each stat, we used Cheat Engine to find which instruction **writes** to it:

1. Scan the stat value (e.g., health as `4 Bytes`)
2. Get hit by a mob → rescan the new value
3. Right-click the address → **"Find out what writes to this address"**
4. Click **"Show disassembler"** to see the full instruction context

Example — **SetHealth** (confirmed via CE):
```asm
; UActorState::SetHealth+1B
79 04           jns +4
33 C0           xor eax, eax
EB 09           jmp +9
41 8B C0        mov eax, r8d
41 3B D0        cmp edx, r8d
0F 4C C2        cmovl eax, edx
89 81 D8010000  mov [rcx+0x1D8], eax  ← NOP this (6 bytes) = God Mode
```

Example — **ConsumeItem** (confirmed via CE during crafting):
```asm
; UInventory::ConsumeItem+266
48 3B F9        cmp rdi, rcx
75 F2           jne -14
44 29 66 04     sub [rsi+04], r12d    ← NOP this (4 bytes) = items not consumed
E9 B1000000     jmp +0xB1
```

### Step 3: Finding Function Addresses with x64dbg

For functions we couldn't find via CE (crafting cost calculations, weight, etc.), we used **x64dbg** with the game's PDB symbols:

1. Attach x64dbg to `Icarus-Win64-Shipping.exe`
2. Go to **Symbols** tab → filter by function name
3. x64dbg resolves the address from the PDB

Functions found via x64dbg:
| Function | Offset | Patch |
|:---------|:-------|:------|
| `GetScaledRecipeInputCount` | `0x18167A0` | `xor eax,eax; ret` (return 0 = zero cost) |
| `GetScaledRecipeResourceItemCount` | `0x1816820` | `xor eax,eax; ret` (return 0) |
| `FindItemCountByType` | `0x190EF30` | `mov eax, 9999; ret` (always have items) |
| `GetTotalWeight` | `0x191F9E0` | `xor eax,eax; ret` (return 0 = no weight) |

### Step 4: Player Detection via GWorld

The trainer finds the local player automatically by scanning for the GWorld pointer:

```
AOB: 48 8B 1D ?? ?? ?? ?? 48 85 DB 74

GWorld → UWorld (+0x0D28) → UGameInstance
  → (+0x38) TArray<ULocalPlayer*> → [0]
  → (+0x30) APlayerController
  → (+0x260) AIcarusCharacter
  → (+0x5A8) UCharacterState
```

All hierarchy offsets were verified against the UE4SS SDK dump.

---

## 🔧 Building from Source

### Prerequisites
- Visual Studio 2022+ with C++ desktop workload
- Windows 10/11 SDK

### Build
```bash
# Clone
git clone https://github.com/CyberSnakeH/ZeusMod.git
cd ZeusMod

# Open solution in Visual Studio
start IcarusMod.sln

# Build → Release | x64
# Copy IcarusInternal.dll next to IcarusInjector.exe
```

### Regenerating SDK Dumps
```
1. Download UE4SS from https://github.com/UE4SS-RE/RE-UE4SS/releases
2. Inject into Icarus (use our InjectUE4SS tool or proxy DLL)
3. Press Ctrl+H in-game to dump SDK headers
4. Headers appear in ue4ss/CXXHeaderDump/
```

---

## ⚠️ Technical Notes

- **Rendering**: Icarus supports both DX11 and DX12. The overlay uses a **Win32 window** instead of DX hooks to avoid crashes with either renderer.
- **Anti-cheat**: Icarus has no kernel-level anti-cheat. Standard DLL injection works.
- **Multiplayer**: Designed for single-player / private sessions only.
- **Game updates**: AOB signatures are version-resilient, but fixed offsets (from x64dbg) may break after updates. Re-run UE4SS + x64dbg to get new offsets.
- **Offsets**: Verified against Icarus April 2026 build.

---

## 📜 License

For educational and research purposes only. Use at your own risk. Not affiliated with RocketWerkz.

---

<p align="center">
  <em>Made with ⚡ by <a href="https://github.com/CyberSnakeH">CyberSnake</a></em>
</p>
