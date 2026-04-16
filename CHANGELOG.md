# Changelog

All notable changes to ZeusMod are documented here.
The format is loosely based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.0] - 2026-04-17

### Added — Survival

- **Stable Temperature** toggle clamps the player's
  `SurvivalCharacterState::ModifiedInternalTemperature` every tick to a
  configurable integer value (default 20°C). This is the exact int the HUD
  thermometer reads, so the player is no longer affected by hot biomes,
  cold storms, night drops, or altitude.

### Added — Progression

- **Mega Exp** grants `+50 000` XP per tick on
  `PlayerCharacterState::TotalExperience` so the character levels up
  visibly through the game's own level-up gates instead of jumping to
  max in one frame.

### Added — Plumbing

- `ResolveAllOffsets()` now resolves
  `SurvivalCharacterState::ModifiedInternalTemperature`,
  `PlayerCharacterState::TotalExperience`, and `Controller::PlayerState`
  through `UObjectLookup::FindPropertyOffset`.
- `FindPlayer()` caches the `PlayerState` pointer alongside the existing
  character / actor state pointers so progression features don't have to
  re-walk the world graph every tick.
- In-game overlay grows from 10 to 12 toggle rows (OVL_H 432 → 484) and
  the renderer + hit-test tables are driven by a single `OVL_ROW_COUNT`
  constant to avoid two-loop drift in future edits.
- Desktop UI gets a new **PROGRESSION** category (Mega Exp) and a new
  **Stable Temperature** row under **SURVIVAL**.
- Pipe protocol gains new commands: `temp`, `temp_val`, `megaexp`.

### Changed

- Bumped desktop app to **1.4.0**.

## [1.3.2] - 2026-04-15

### Fixed

- In-app updater: clicking **Download and install** now actually launches
  the downloaded NSIS installer instead of silently killing it. The previous
  build called `execFile(installer, [], { detached: true, windowsHide: true })`
  immediately followed by `app.quit()`, which:
  - left the child's stdio pipes attached to the parent (`stdio: 'pipe'` by
    default), so the installer process was torn down with the Electron app,
  - and applied `CREATE_NO_WINDOW` (`windowsHide: true`) on a GUI installer,
    which can suppress the NSIS wizard window.
  The handler now uses `stdio: 'ignore'` and calls `child.unref()` so the
  installer survives the parent quitting and its UAC + wizard UI appears
  normally. After clicking **Finish**, the NSIS *Run ZeusMod* checkbox
  (default on) relaunches the new version.

## [1.3.1] - 2026-04-15

### Fixed

- The packaged `ZeusMod.exe` now actually carries the ZeusMod icon. The
  previous build set `signAndEditExecutable: false` in the electron-builder
  config, which silently disabled `rcedit` and prevented the icon from
  being embedded into the executable resource section. Removing that flag
  lets electron-builder embed the icon, so the desktop shortcut, taskbar,
  Start Menu and Explorer file icon all show ZeusMod artwork.
- The bundled `.ico` is now a true multi-resolution icon
  (16 / 24 / 32 / 48 / 64 / 128 / 256), so Windows picks a crisp size in
  every context instead of downscaling 256 → 16.

### Changed

- Bumped desktop app to **1.3.1**.

## [1.3.0] - 2026-04-15

### Added

- Free Craft now works on **0 / N** recipes — the crafting UI accepts inputs
  the player does not own, and the validation chain is bypassed end-to-end
  (`CanQueueItem`, `HasSufficientResource`, `GetResourceRecipeValidity`,
  `CanSatisfyRecipeQueryInput`).
- Player-owned inventory filter on the item-count hooks: only the local
  player's `Inventory` returns the unlimited fake count, so deployable
  inventories (extractors, processors, benches) keep their real contents and
  outputs are delivered correctly.
- Tick subsystem auto-registration: when the queued recipe context is missing
  from `DeployableTickSubsystem`'s active processor list, ZeusMod inserts a
  valid `FWeakObjectPtr { ObjectIndex, SerialNumber }` into the unreflected
  `+0x60` `TArray`, and processing proceeds normally.
- `ProcessingItem` pre-populator: when `AddProcessingRecipe` runs while the
  game's internal `ValidateQueueItem` would have rejected the queue entry,
  ZeusMod copies the queued slot into `ProcessingItem` (`+0x1C0`) so the
  in-game `Process` tick can advance progress.
- Embedded ZeusMod application icon (256×256) wired into:
  - the Electron `BrowserWindow`
  - the NSIS installer header / installer icon / uninstaller icon
  - the desktop shortcut and Start Menu entry
- Companion app now ships the **latest** internal DLL and external injector
  inside `app/bin/`, plus the PowerShell remote-thread injector in
  `app/scripts/inject.ps1`, so a fresh install is fully self-contained.

### Changed

- Bumped the desktop app to **1.3.0** (`package.json`, `BrowserWindow`,
  installer artifact name).
- The DLL injection IPC handler now points at `bin/IcarusInternal.dll` and
  `scripts/inject.ps1` from the packaged resources directory rather than the
  development tree, so the installed app no longer depends on repo paths.
- README rewritten as a user-facing GitHub-style document with feature list,
  architecture diagram, build steps, screenshots and changelog reference.

### Fixed

- Crash on inject caused by patching an out-of-range probe address: the
  installer for byte patches now validates that the target falls inside the
  module's plausible code range before writing.
- `__try` / `__except` mixed with C++ object unwinding (C2712): isolated raw
  memory probes into helper functions that do not construct C++ objects.
- Script VM reentrance crash (`UObject::CallFunction` with
  `0xFFFFFFFFFFFFFFFF`) when re-entering UFunction thunks from a hook —
  the affected re-entry path is now avoided and replaced with direct context
  manipulation.

### Patch resilience

- All UFunction entry points are still resolved at runtime by **name** via
  `UObjectLookup::FindNativeFunction` (UE reflection). UPROPERTY offsets are
  still resolved at runtime through the FField chain. Most game updates
  should not require a rebuild.

## [1.2.0] - 2026-04-14

### Added

- Unified solution layout around `Shared`, `IcarusInjector`, and
  `IcarusInternal`.
- Professionalized Electron update flow with explicit update state reporting,
  release-note display, manual re-check support, installer download progress
  and a release-page fallback.
- English-only desktop UI strings and English-only ImGui overlay strings for
  the active runtime menu.

### Changed

- Refined the Electron app header to expose update status and a manual check
  action.
- Reworked the update modal into a clearer release workflow.
- Rewrote the top-level documentation for the current project layout and
  active build path.
- Archived deprecated assets into `NotUsed/`.

### Removed

- `IcarusTrainer` from the active solution.
- Retired tools, SDK dumps, and old root offset artifacts (moved to
  `NotUsed/`).

## Earlier repository state

Before the 1.2.0 cleanup pass, the repository still contained archived SDK
dump material at the top level, an unused trainer project in the active
solution, and outdated references to older AOB-generation tooling in the
main tree. Those assets are preserved under `NotUsed/`.
