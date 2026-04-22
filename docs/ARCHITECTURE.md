# ZeusMod — Architecture

This document describes how the four ZeusMod components fit together and the
flow of control from the user pressing *Inject* to a patched byte landing in
the live Icarus process.

## Components

| Component          | Kind                 | Location             | Role                                                                 |
|--------------------|----------------------|----------------------|----------------------------------------------------------------------|
| `Shared`           | C++ static library   | `native/shared/`     | IPC protocol and shared enums used by both native executables        |
| `IcarusInjector`   | C++ executable       | `native/injector/`   | Standalone native injector (`CreateRemoteThread` + `LoadLibraryW`)   |
| `IcarusInternal`   | C++ dynamic library  | `native/internal/`   | Injected DLL: hooks, UE reflection, ImGui overlay, IPC pipe server   |
| `app/`             | Electron application | `app/`               | Desktop launcher, install detection, IPC client, GitHub auto-updater |

The repo also has two top-level directories of non-code material:

- `docs/` — this file, build docs, screenshots.
- `.github/` — CI workflow and release-note templates.

External dependencies (ImGui + MinHook) live under `native/third_party/`, which
is gitignored and fetched at build time by CI.

## Native tree layout

Each of the three native projects is organised the same way: project file at
the top, `src/` split into purpose-specific subfolders, and MSBuild-relative
outputs under `bin/` and `obj/`.

```
native/
├── shared/
│   ├── include/{SharedTypes.h, PipeProtocol.h}     Public API
│   ├── src/PipeProtocol.cpp                        Implementation
│   └── Shared.vcxproj
│
├── injector/
│   ├── src/
│   │   ├── main.cpp                                WinMain, attach loop
│   │   ├── core/{Injector, ProcessUtils}           CreateRemoteThread, PID lookup
│   │   └── ui/{GUI}                                Win32 status window
│   ├── resources/{resource.h, zeusmod.rc, zeusmod.ico, generate_icon.py}
│   └── IcarusInjector.vcxproj
│
└── internal/
    ├── src/
    │   ├── core/dllmain.cpp                        DllMain entry + init thread
    │   ├── game/                                   UE 4.27 reflection layer
    │   │   ├── SDK.h                               Extract of the UE SDK dump
    │   │   ├── UE4.h                               UE4 structs + PatternScan
    │   │   └── UObjectLookup.{h,cpp}               GUObjectArray/FNamePool scan,
    │   │                                            UFunction/UProperty by name
    │   ├── hooks/Render.{h,cpp}                    DX11/DX11on12 swapchain hook
    │   ├── ui/Overlay.{h,cpp}                      ImGui menu, cheat toggles
    │   ├── cheats/Trainer.{h,cpp}                  All cheat logic + IPC pipe server
    │   └── util/Logger.{h,cpp}                     File + debug-output logging
    └── IcarusInternal.vcxproj
```

Each `vcxproj` adds its `src/*` subfolders to `AdditionalIncludeDirectories`,
so cross-folder includes stay flat (`#include "Trainer.h"`, not
`#include "../cheats/Trainer.h"`).

## Runtime flow

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         app/  (Electron renderer)                        │
│  • detects Steam install, Icarus process, update state                   │
│  • on Inject → spawns PowerShell script                                  │
└─────────────────────┬────────────────────────────────────────────────────┘
                      │ IPC (named pipe \\.\pipe\ZeusModPipe)
┌─────────────────────▼────────────────────────────────────────────────────┐
│                 app/scripts/inject.ps1                                   │
│  • OpenProcess + VirtualAllocEx + WriteProcessMemory                     │
│  • CreateRemoteThread(LoadLibraryW, "IcarusInternal.dll")                │
└─────────────────────┬────────────────────────────────────────────────────┘
                      │ LoadLibrary
┌─────────────────────▼────────────────────────────────────────────────────┐
│          native/internal/  (IcarusInternal.dll, injected)                │
│                                                                          │
│   core/dllmain    →   game/UObjectLookup (AOB-scan GUObjectArray, FNamePool) │
│                   →   cheats/Trainer::Initialize                         │
│                          └─ resolves offsets + UFunction addrs by name   │
│                          └─ installs MinHook detours                     │
│                          └─ starts IPC pipe server                       │
│                   →   hooks/Render::Initialize (DX11 present hook)       │
│                   →   ui/Overlay (renders ImGui menu inside the hook)    │
└──────────────────────────────────────────────────────────────────────────┘
```

### Reflection-driven resolution

The trainer deliberately avoids hardcoded byte patterns where possible.
`UObjectLookup` scans for the `GUObjectArray` and `FNamePool` globals, then
exposes:

- `FindNativeFunction(className, fnName)` — walks `UClass → UFunction`, follows
  the script-native thunk using HDE64 disassembly, returns the C++ impl.
- `FindPropertyOffset(className, propName)` — walks the `FField` chain.

Most patches are installed through the native-first + AOB-fallback helper
`ResolveNativeOrAob` inside `cheats/Trainer.cpp`, so non-engine patches on
Icarus generally do not require a rebuild.

A few low-level UE 4.27 layout assumptions remain (the unreflected `+0x60`
active-processor TArray on `DeployableTickSubsystem`, `FUObjectItem` serial
offset, `UStruct.Children` offset). These are stable within UE 4.27 but would
need re-validation if Icarus moved to UE5.

## Known technical debt

- **`cheats/Trainer.cpp` is ~3 600 LOC.** Every patch lives on a single
  `Trainer` singleton and accesses private state (backups, patch addresses,
  hook pointers). Splitting it into per-feature files (`GodMode`, `Speed`,
  `Craft`, `Time`, `Temperature`, …) is desirable but non-trivial — it
  requires either a shared `PatchRegistry`/`ICheat` base or a friend pattern
  to preserve the current locking/order semantics. Treated as a follow-up so
  the refactor does not ship alongside a directory reorg.
- The Electron renderer (`app/src/renderer/js/app.js`) is a single file and
  could be split into IPC / cheat-UI / updater modules.
