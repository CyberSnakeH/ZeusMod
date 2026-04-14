# ZeusMod

<p align="center">
  <img src="Pictures/i1.png" width="280" alt="Injector desktop UI">
  &nbsp;&nbsp;
  <img src="Pictures/i2.png" width="600" alt="Runtime console and in-game overlay">
</p>

<p align="center">
  <img src="Pictures/i3.png" width="500" alt="Free Craft in action">
</p>

<p align="center">
  <strong>Internal runtime module, desktop injector, and Electron companion app for Icarus.</strong>
</p>

<p align="center">
  <a href="#overview">Overview</a> |
  <a href="#features">Features</a> |
  <a href="#architecture">Architecture</a> |
  <a href="#build">Build</a> |
  <a href="#usage">Usage</a> |
  <a href="CHANGELOG.md">Changelog</a>
</p>

## Overview

ZeusMod is a Windows-only Icarus internal project composed of:

- `IcarusInternal`: the injected DLL responsible for runtime feature logic, UE object lookup, overlays, and pipe-based command handling
- `IcarusInjector`: the native injector application used to attach the DLL to the game process
- `app`: the Electron desktop companion UI used to detect the game, inject the DLL, manage toggles, and deliver app updates
- `Shared`: shared enums, constants, and project types used by the native components

The current runtime favors Unreal reflection and native function lookup wherever possible. Gameplay offsets are resolved at runtime when available, while the low-level Unreal bootstrap still relies on stable engine layout assumptions.

## Features

### Runtime Features

- God Mode
- Infinite Stamina
- Infinite Armor
- Infinite Oxygen
- No Hunger / Thirst
- Free Craft
- No Weight Limit
- Speed Hack
- Time Lock

### Desktop App Features

- Icarus install detection through Steam library discovery
- Running-process detection for `Icarus-Win64-Shipping.exe`
- One-click DLL injection from the desktop UI
- GitHub release update checks with release notes, progress feedback, and installer handoff
- English-only desktop UX and English-only in-game ImGui overlay

## Architecture

```text
ZeusMod/
|- IcarusInjector/   # Native injector executable
|- IcarusInternal/   # Injected DLL, overlays, runtime hooks, UE lookup
|- Shared/           # Shared native headers and types
|- app/              # Electron desktop app and auto-update UI
|- Pictures/         # README screenshots
|- release/          # CI packaging staging folder
`- NotUsed/          # Archived tools, old trainer, dumps, and retired assets
```

### Native Runtime Flow

1. `IcarusInjector` launches and injects `IcarusInternal.dll`
2. `IcarusInternal` initializes UE bootstrap lookup
3. Runtime offsets and native functions are resolved through reflection/name lookup where available
4. The DLL exposes a named pipe (`\\.\pipe\ZeusModPipe`) for external toggles
5. The in-game overlay reads and writes trainer state live

### Desktop App Flow

1. The Electron app detects the local Steam install and the running Icarus process
2. The user attaches to the game from the desktop UI
3. The app forwards cheat toggles to the injected module through the pipe
4. The app checks GitHub Releases for a newer installer and can download it with progress reporting

## Update System

The desktop app includes a built-in updater flow aimed at the packaged Electron build:

- checks the latest GitHub Release from `CyberSnakeH/ZeusMod`
- compares the latest tag against the local app version
- surfaces release notes in-app
- downloads the preferred installer executable with progress feedback
- launches the installer and exits the current app instance

This is intentionally separate from the injected runtime. Native binaries are shipped alongside the app package and bundled into release assets.

## Build

### Requirements

- Windows 10 or Windows 11
- Visual Studio 2022 or newer with Desktop development for C++
- Node.js 20+ for the Electron app

### Native Solution

Open:

```powershell
start IcarusMod.sln
```

Active solution projects:

- `Shared`
- `IcarusInjector`
- `IcarusInternal`

Build configuration:

```text
Release | x64
```

### Electron App

From the `app` directory:

```powershell
npm install
npm run dist
```

The packaged app expects the injector and internal DLL to be present in `app/bin/`.

## Usage

1. Launch Icarus and load into a prospect.
2. Start the desktop app or run `IcarusInjector.exe`.
3. Attach to the running game.
4. Toggle features from the desktop app or the in-game overlay.
5. Use `N` to open or close the ImGui overlay.
6. Use `F10` to unload the module.

## Runtime Notes

- The project is intended for local/private play and research scenarios.
- The desktop app, native injector, and internal DLL currently target Windows x64.
- The runtime resolver already covers a large share of gameplay properties and native functions through Unreal reflection.
- Some low-level bootstrap and engine-structure assumptions still remain in the UE lookup layer, which is expected for this style of internal tooling.

## Repository Hygiene

Legacy material that is no longer part of the active build has been moved to [`NotUsed/`](NotUsed):

- archived SDK dumps
- retired tools
- legacy trainer project
- temporary third-party source drops

This keeps the active tree focused on the maintained injector, internal runtime, and desktop app.

## Changelog

Project history is tracked in [CHANGELOG.md](CHANGELOG.md).

## Disclaimer

This repository is provided for educational and research purposes only. Use at your own risk. It is not affiliated with RocketWerkz.
