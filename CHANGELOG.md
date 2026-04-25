# Changelog

All notable changes to ZeusMod are documented here.
The format is loosely based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.1] - 2026-04-25

### Highlights

- **D3D12 backend rewrite — overlay no longer crashes on DX12 prospects.**
  The previous D3D11On12 interop bridge produced
  `DXGI_ERROR_DEVICE_REMOVED 0x887A002B` on most modern GPUs because
  Icarus' command queue was being recycled mid-frame under the bridge.
  `Render.cpp` now uses the official `imgui_impl_dx12` backend directly
  and captures the game's live `ID3D12CommandQueue` via an
  `ExecuteCommandLists` MinHook (with an `IDXGISwapChain3::GetDevice`
  fallback for older driver paths). The overlay is rendered straight
  onto the back buffer in the game's own queue — no temporary device,
  no inter-API copies, no DEVICE_REMOVED.
- **Menu key changed from `N` to `²`** (the key directly below `Esc`).
  Resolved at runtime via `MapVirtualKeyW(0x29, MAPVK_VSC_TO_VK)` so it
  picks up the right virtual-key on every keyboard layout (AZERTY,
  QWERTZ, Dvorak, …). Previously on AZERTY the menu shared the chat
  key, swallowing player input.
- **FreeCraft items now stack and are placeable.** Three independent
  bugs in the in-bag delivery path were fixed:
  1. **Wall / beam placement** — the trainer's `findTemplateFItemData`
     used to clone the *first occupied slot* as the new item's
     `FItemData`. When that slot held a resource (Stone, Fiber, …) its
     1-entry dyn array was inherited by walls and foundations,
     producing items the server-side `DeployableComponent::Server_RequestDeploy`
     validator silently refused. Replaced with a category-matched
     selector (`Structure` / `Tool` / `Ammo` / `Liquid` / …) that
     clones a same-shape template, so a freecrafted Wood_Wall inherits
     the canonical 9-entry dyn shape `{0,1,2,3,4,5,6,7,12}`.
  2. **"Broken" icon on placeable items** — an earlier "pick the
     largest dyn template" heuristic had the failure mode of cloning
     ammo (11-entry shape with `type=8/10/11` ammo-specific keys and
     `Durability=0`). The category gate above eliminates that path.
  3. **Workbench / Fabricator stacking** — crafting at a station no
     longer outputs one slot per unit. `Trainer_AddItemToProcessor`
     now patches the cloned dyn `type=7 ItemableStack` to the
     requested count and fires `ProcessingComponent::AddItem` exactly
     once, so 200 rounds land in 2 stacks of 100 instead of 200 stacks
     of 1.
- **GodMode is player-only and works across the lobby.** The
  `PatchSetHealth` instruction-NOP that previously made every actor
  invincible (animals included) is gone. Health is now pinned via a
  per-tick rewrite to `m_actorState->Health = MaxHealth` plus
  `AliveState = 0` in a tight 100-iteration thread loop. With
  *Apply To All Players* on, the same write is mirrored onto every
  remote `SurvivalCharacterState` whose `UClass*` matches the local
  player's exactly — animals and AI stay killable.
- **Stability — three crashes traced and eliminated.**
  - `EXCEPTION_ACCESS_VIOLATION` in
    `TFastReferenceCollector::ProcessObjectArray` (UE4 GC sweep,
    parallel thread) was caused by writing `State_Health` /
    `State_AliveState` into AI / creature subclasses of
    `SurvivalCharacterState` whose layouts re-purpose those offsets
    as pointer fields. The multiplayer mirror loop now filters by
    UClass-pointer equality (read at `+0x10` every tick), and the
    refresh interval was tightened from 60 ticks to 10.
  - `EXCEPTION_ACCESS_VIOLATION` in
    `UMaterialInstance::SetMIParameterValueName` (render thread, NULL
    deref at `+0x84`) was caused by `RemoveDebuffs` brute-force
    writing `0.0f` into every BlueprintCreatedComponent whose
    offsets `Mod_Lifetime` / `Mod_Remaining` happened to contain
    plausible floats — including MaterialInstance internals. Fixed
    with a `UModifierStateComponent` class-pointer filter (walks the
    Super chain at `cls + 0x40`).
  - `RemoveDebuffs` and the multiplayer mirror loop now share a
    single `IsPlausiblePlayerState` validator + class-pointer match,
    so neither can leak writes into unrelated UObjects again.
- **Overlay redesign — Electron parity.** The in-game overlay now
  matches the launcher's typography and palette: cyan / purple
  accent pair, navy `#0a0d14` backdrop, sidebar nav with active rail,
  cheat cards with auto-grown height, custom toggle switches with a
  cyan→purple gradient when on, soft drop shadows, gradient section
  headers, and a status pill in the title bar. Cards use
  `ImGuiChildFlags_AutoResizeY` so sliders and long descriptions are
  never clipped.
- **Embedded font.** Inter Medium (411 KB, SIL OFL) is now bundled
  inside the DLL as a `constexpr unsigned char[]` blob
  (`native/internal/src/hooks/InterFont.h`). The same face the
  launcher uses, identical pixel-for-pixel on every Windows install.
  Falls back to the ImGui built-in only on a load failure.

### Removed

- **Bypass Placement Validation toggle** and the
  `InventoryItemLibrary::ItemDataValid` MinHook. Was a research aid
  for the wall-placement bug above; the proper fix (category-matched
  template clone) makes it obsolete.
- **Dump Held Item Data button** and the matching
  `Trainer::DumpHeldItemData()` + helpers. Same reason — diagnostic
  scaffolding that's no longer needed in shipped builds.

### Notes

- DLL grew from ~1.5 MB to ~2.0 MB — the difference is the embedded
  Inter TTF.
- Existing 1.5.0 installs auto-update through the in-app updater.
- Multiplayer cheats apply on the listening host only; client-side
  toggles are silently ignored because the server-side replication
  pass would clobber them anyway.

---

## [1.5.0] - 2026-04-22

### Highlights

- **Native in-process injector** — the Electron app now injects
  `IcarusInternal.dll` directly via a **koffi** FFI binding
  (`OpenProcess` → `VirtualAllocEx` → `WriteProcessMemory` →
  `CreateRemoteThread` → `LoadLibraryW`). The `inject.ps1` PowerShell
  fallback and its `Add-Type` JIT cost are gone. Typical end-to-end
  inject latency dropped from ~1 s to **50–150 ms**, and the flow no
  longer spawns a child process (fewer AV false-positives).
- **No Weight fixed at the source** — ZeusMod now detours
  `IcarusFunctionLibrary::AddModifierState` via MinHook. When the
  incoming row is `Overburdened` and No Weight is ON, the call is
  **refused** — the modifier is never applied, so the character no
  longer feels heavy even when the UI reads 0 kg. The previous
  patches (MaxWalkSpeed clamp, ExpireOverburdenedModifier) that
  caused the PhysX tick access-violation have been rolled back.
- **Redesigned desktop UI** — sidebar-driven category nav
  (Survival · Inventory · Character · World · Progression · Give
  Items), a live status strip with an animated connection dot, hero
  attach button with state transitions, and a cyan/purple gradient
  theme. The update modal has been rebuilt with a glass backdrop and
  release-note viewer. All IPC round-trips still go through
  `window.zeusmod.*` in `preload.js`.
- **Phase B scanner (x64dbg-tier)** — `IcarusInternal.dll` gains
  `memmap`, `modules`, `strings`, `refs`, `search` commands on the
  debug pipe. Paired with `scripts/inspect.py`, ZeusMod now ships a
  full memory explorer (module list, VirtualQuery walker, ASCII +
  UTF-16 string scan, pointer back-reference scan, typed value
  search).

### Added — `scripts/inspect.py` (rewrite)

- Graceful `rich` + `capstone` fallback — script still runs on a
  vanilla Python 3.9+ install.
- **Typed readers** — `readf32`, `readf64`, `readi32`, `readi64`,
  `readstr`, `readunicode`, `readguid`, `readptrarr`.
- **Labels** (persistent via `~/.zeusmod_labels.json`) — annotated on
  the hex-dump grid.
- **Bookmarks** (persistent) with optional description.
- **Snapshots** — `snapshot`, `snapshots`, `diff <name>` capture a
  region and show byte-level deltas.
- **Struct viewer** — `struct <UClass> <base>` decodes UE
  `UPROPERTY` offsets with `f32`/pointer hints.
- **Disassembly** — `disasm <addr> <n>` via capstone, when installed.
- **REPL** — `readline`-backed history (`~/.zeusmod_history`), tab
  completion, `help` system with per-command detail and examples.
- **Batch / watch / JSON** — `-c`, `--watch`, `--json`, `--timing`
  CLI flags for scripting and diffed monitoring.

### Added — Repository hygiene

- Release/version badge moved to **1.5.0** on the README.
- `docs/INSPECT.md` — full Python-client command reference.
- GitHub issue templates (bug report, feature request) and PR
  template under `.github/`.

### Removed

- `app/scripts/inject.ps1` — replaced by the koffi FFI injector.
- PowerShell spawn from the `game:inject` IPC handler.

### Migration note

- Existing 1.4.11 installs will auto-update to 1.5.0 through
  `electron-updater` exactly as 1.4.11 demonstrated end-to-end.

## [1.4.11] - 2026-04-17

### Highlights

- **End-to-end test of the new in-app updater** — 1.4.11 is the first
  release that existing 1.4.1 installs are expected to pick up and
  install **fully automatically** (download → silent install →
  auto-relaunch) via `electron-updater` and the silent one-click NSIS
  package shipped in 1.4.1.
- **`electron-builder` bumped 25 → 26.8.1**, which pulls in fixed
  versions of `tar`, `cacache`, `glob`, `@electron/rebuild`, and
  friends, eliminating **11 of 12 `npm audit` advisories** in the
  build tree (all of them dev-only transitive deps).
- **Release notes are now generated from `CHANGELOG.md`** instead of
  being hardcoded in the workflow. The CI reads the `## [X.Y.Z]`
  section that matches the pushed tag and uses it as the GitHub
  release body, so future releases always advertise the actual
  changes that shipped.
- Release body additionally describes the three asset families
  (installer, updater metadata, standalone zip) so users don't try to
  run `latest.yml` or the `.blockmap`.

### Known

- The single remaining `npm audit` advisory is against **Electron 33**
  itself (multiple moderate / high CVEs fixed in Electron 38.8.6 /
  39.8.5). Bumping Electron is a semver-major update and is deferred
  to a dedicated release rather than bundled here, so that the
  updater flow can be validated against a minimally-changed build.

## [1.4.1] - 2026-04-17

### Changed — Updater (critical fix)

- Replaced the custom GitHub-releases updater with `electron-updater`.
  The previous implementation downloaded the installer and spawned it
  as a detached child process, but the install wizard often never
  became visible (UAC or wizard interactions), leaving the app stuck
  on "Restarting..." without actually updating.
- NSIS installer switched to **silent one-click** mode:
  - `oneClick: true` (no wizard pages)
  - `perMachine: false` (installs to `%LOCALAPPDATA%\Programs\ZeusMod`,
    no UAC prompt)
  - `runAfterFinish: true` (app auto-launches after install)
- CI now publishes `latest.yml` + installer blockmap alongside the
  installer so `electron-updater` can discover the latest release and
  support differential downloads.
- `update:install` IPC now calls `autoUpdater.downloadUpdate()` +
  `autoUpdater.quitAndInstall(isSilent=true, isForceRunAfter=true)` —
  the install is silent and the app re-launches automatically.

### Migration note

- Users already running v1.3.x or v1.4.0 must perform **one last
  manual download** of `ZeusMod-Setup-1.4.1.exe` from the Releases
  page (the old in-app updater cannot reliably hand off to the new
  installer). After installing 1.4.1 manually, every subsequent
  update is fully automatic.

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
