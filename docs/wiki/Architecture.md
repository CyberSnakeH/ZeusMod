# Architecture

This page describes how ZeusMod's four components fit together, what
runs in which process, and how data flows between them.

---

## Component diagram

```
                    ┌──────────────────────────────────────────┐
                    │              Operator's PC               │
                    └──────────────────────────────────────────┘
                                     │
         ┌───────────────────────────┼───────────────────────────┐
         ▼                           ▼                           ▼
┌──────────────────┐         ┌───────────────────┐       ┌──────────────────┐
│  ZeusMod.exe     │         │  inspect.py       │       │  Icarus-Win64-   │
│  (Electron)      │         │  (Python client)  │       │  Shipping.exe    │
│                  │         │                   │       │                  │
│  • main.js       │         │  • REPL           │       │   ┌───────────┐  │
│  • preload.js    │         │  • Batch / watch  │       │   │IcarusInt- │  │
│  • renderer/     │         │  • JSON mode      │       │   │ernal.dll  │  │
│  • injector.js   │         │                   │       │   └─────┬─────┘  │
│    (koffi FFI)   │         │                   │       │         │        │
└────────┬─────────┘         └──────────┬────────┘       │         │        │
         │                              │                │   ┌─────▼─────┐  │
         │ IPC (IPCMain/Render)         │ named pipe     │   │  MinHook  │  │
         │                              │                │   │  detours  │  │
         │  ┌───────────────────────────┼────────────────┼─► │  on UFunc │  │
         │  │  named pipe               │                │   │  thunks   │  │
         │  │  \\.\pipe\ZeusModPipe     │                │   └─────┬─────┘  │
         │  │  \\.\pipe\ZeusModDbg      │                │         │        │
         │  ▼                           ▼                │   ┌─────▼─────┐  │
         └──► pipe client (Electron) ◄──── pipe client ──┼───►│   UE4     │  │
                                                         │   │reflection │  │
                                                         │   │ (FField,  │  │
                                                         │   │  UFunc…)  │  │
                                                         │   └───────────┘  │
                                                         └──────────────────┘
```

Two pipes, two client roles:

| Pipe                             | Server                          | Clients                          | Purpose                                             |
|----------------------------------|---------------------------------|----------------------------------|-----------------------------------------------------|
| `\\.\pipe\ZeusModPipe`           | DLL (PipeServer thread)         | Electron launcher                | Cheat on/off, multiplier values, attach heartbeat   |
| `\\.\pipe\ZeusModDbg`            | DLL (DbgServer thread)          | `scripts/inspect.py`             | Reflection + memory access (read, write, scan, …)   |

The two pipes share the same wire format but different command
vocabularies — see [Pipe Protocol](Pipe-Protocol.md).

---

## Processes and their lifetimes

| Process                               | Lifetime                                                                                                          |
|---------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `ZeusMod.exe`                         | From user double-click to user close. Can be relaunched without reinjecting.                                      |
| `Icarus-Win64-Shipping.exe`           | The game. Completely unmodified on disk.                                                                          |
| `IcarusInternal.dll`                  | Loaded into the game via `LoadLibraryW`. Unloads on <kbd>F10</kbd> or on game exit.                               |
| Trainer thread (inside the DLL)       | Spawned in `DllMain(DLL_PROCESS_ATTACH)`. Runs the main trainer loop until <kbd>F10</kbd>.                        |
| Pipe-server thread                    | Spawned alongside the trainer thread. Blocks on `ConnectNamedPipe`; per-client dispatcher.                        |
| Debug-pipe server thread              | Same pattern as the cheat pipe, separate pipe name.                                                               |
| `IcarusInjector.exe`                  | Optional headless path. Lives only long enough to fire `CreateRemoteThread(LoadLibraryW)` and report an exit code. |

The Electron app and Icarus are **independent processes**. Killing one
does not affect the other; the DLL keeps running inside Icarus whether
or not ZeusMod is open.

---

## Data flow: "User flips No Weight ON"

1. User clicks the checkbox in the **Inventory** panel.
2. `renderer/js/app.js` → `handleToggleChange("weight", true)`.
3. `window.zeusmod.toggleCheat("weight", "1")` (preload bridge).
4. `ipcMain.handle("cheat:toggle")` picks it up in `main.js`.
5. `main.js` writes the line `weight:1\n` to
   `\\.\pipe\ZeusModPipe` via the standard pipe client.
6. Inside the game, `PipeServer::HandleLine("weight:1")` dispatches
   to `Trainer::OnCheatChange("weight", "1")`.
7. The trainer sets `Trainer::Get().NoWeight = true` and, if not
   already installed, calls `InstallWeightHook()`:
   - `UObjectLookup::FindNativeFunction("IcarusFunctionLibrary", "AddModifierState")`
   - `MinHook → CreateHook(target, &HookAddModifierState, &g_origAddModifierState)`
   - `MH_EnableHook(target)`
8. From the next tick onward, whenever the game internally calls
   `AddModifierState`, our detour inspects the `inModifier` struct.
   If the row is `"Overburdened"` and `NoWeight` is true, the detour
   **swallows the call** and returns `false` — the modifier is never
   applied.

Every cheat follows this exact pattern. Some additionally run
per-tick clamps on `UPROPERTY`s (Stamina, Health, Oxygen, Food,
Water, Temperature).

---

## Code layout

```
native/
├── shared/                 ← libZeusMod shared types + pipe helpers
│   ├── include/
│   │   ├── PipeProtocol.h
│   │   └── SharedTypes.h
│   └── src/PipeProtocol.cpp
├── injector/               ← IcarusInjector.exe (standalone CLI + minimal GUI)
│   ├── src/
│   │   ├── core/
│   │   │   ├── Injector.cpp / .h      ← CreateRemoteThread path
│   │   │   └── ProcessUtils.cpp / .h  ← PID lookup, privilege elevation
│   │   ├── ui/GUI.cpp / .h            ← Optional WinForms window
│   │   └── main.cpp                   ← CLI entry point
│   └── resources/                     ← App icon + .rc
└── internal/               ← IcarusInternal.dll (the payload)
    ├── src/
    │   ├── core/dllmain.cpp           ← DllMain, trainer thread boot
    │   ├── cheats/
    │   │   ├── Trainer.cpp / .h       ← Main loop, per-cheat toggles
    │   │   ├── TrainerResolve.cpp     ← Runtime offset resolution
    │   │   ├── TrainerFreeCraft.cpp   ← Crafting chain hooks
    │   │   ├── TrainerGiveItem.cpp    ← MakeItemTemplate → AddItem
    │   │   └── TrainerDiagnostics.cpp ← Debug pipe handler surface
    │   ├── game/
    │   │   ├── UE4.h                  ← Minimal UE 4.27 primitives
    │   │   ├── UObjectLookup.cpp/.h   ← FField walker, FName resolver
    │   │   └── SDK.h                  ← Small hand-written SDK slice
    │   ├── hooks/Render.cpp/.h        ← Swapchain Present hook for overlay
    │   ├── ui/Overlay.cpp/.h          ← ImGui menu wiring
    │   └── util/Logger.cpp/.h         ← Ring-buffered in-game log
    └── IcarusInternal.vcxproj
```

The Electron app mirrors this split:

```
app/
├── src/
│   ├── main/
│   │   ├── main.js                    ← ipcMain handlers, window lifecycle
│   │   ├── injector.js                ← koffi FFI binding (LoadLibraryW injection)
│   │   └── preload.js                 ← contextBridge window.zeusmod.*
│   ├── renderer/
│   │   ├── index.html                 ← Sidebar, cards, update modal
│   │   ├── js/app.js                  ← Renderer-side logic
│   │   └── styles/main.css            ← Cyan/purple theme
│   └── assets/icon.ico
├── bin/                               ← Bundled DLL + CLI injector
└── package.json
```

---

## Why reflection, not signatures?

Every single byte-AOB pattern we've tried against Icarus has broken
within two patches. Reflection-driven resolution is the difference
between "works for six weeks then needs a full reverse-engineering
session" and "works across content patches for months".

Concretely, for every UFunction we hook, we do:

```cpp
uint8_t* target = UObjectLookup::FindNativeFunction(
    /* className */ "SurvivalCharacter",
    /* funcName  */ "SetHealth");
```

Under the hood that looks up the class in `GObjects`, walks
`UStruct::Children` to find the `UFunction`, reads its
`Func` pointer (the generated thunk), and walks the thunk's
instructions to the C++ `exec` body. Result: a stable pointer that
survives re-layout of the function within the compiled binary.

See [Reflection Internals](Reflection-Internals.md).

---

## Non-reflected assumptions (and where they live)

A handful of UE 4.27 layout assumptions remain. They are **stable
within UE 4.27** but would need re-validation if Icarus ever moves
to UE5:

| Assumption                                                  | Lives in                                           |
|-------------------------------------------------------------|----------------------------------------------------|
| `FUObjectItem` serial-number offset (`+0x14`)               | `UObjectLookup::GetObjectByIndex`                  |
| `UStruct::Children` offset (`+0x40`)                        | `UObjectLookup::WalkFields`                        |
| `FName::ComparisonIndex` layout                             | `UObjectLookup::ResolveFNameByIndex`               |
| `DeployableTickSubsystem +0x60` active-processor `TArray`   | `TrainerFreeCraft::InjectProcessorEntry`           |
| `UMG_EncumbranceBar_C` widget offsets (PlayerWeight `+0x304`) | `TrainerResolve::LocateEncumbranceWidgetOffsets` |
| Windows x64 calling convention + MinHook allocation strategy | Everywhere we install a detour                    |

Anything not in this table is resolved by name or by type.

---

## See also

- [Pipe Protocol](Pipe-Protocol.md) — the exact wire format for both pipes.
- [Hook Catalog](Hook-Catalog.md) — one entry per MinHook detour.
- [Reflection Internals](Reflection-Internals.md) — how `UObjectLookup` finds things.
- [Memory Layout](Memory-Layout.md) — every resolved offset we rely on.
