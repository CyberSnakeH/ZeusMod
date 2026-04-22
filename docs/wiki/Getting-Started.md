# Getting Started

This page walks you through installing ZeusMod from the release, attaching
to *Icarus*, and toggling your first cheat. It also covers the headless
path for anyone who wants to run the trainer from the command line.

> **Prerequisites** — a 64-bit Windows install (10 or 11), a Steam copy of
> *Icarus* (any recent patch), and 150 MB of free disk space. No .NET, no
> Visual C++ runtime to chase — everything the installer needs is bundled.

---

## 1. Install the launcher

1. Download the latest `ZeusMod-Setup-X.Y.Z.exe` from the
   [**Releases**](https://github.com/CyberSnakeH/ZeusMod/releases) page.
2. Run it. The installer is a **silent one-click NSIS** package:
   - No wizard pages.
   - No UAC prompt (it installs into
     `%LOCALAPPDATA%\Programs\ZeusMod`).
   - Creates a desktop shortcut and a Start-Menu entry.
   - Auto-launches ZeusMod when it finishes.
3. From now on, every new release will be pulled and installed
   automatically in the background via `electron-updater`.

### What the installer actually drops

```text
%LOCALAPPDATA%\Programs\ZeusMod\
├── ZeusMod.exe
├── resources\
│   ├── app.asar                    ← Electron app bundle
│   └── bin\
│       ├── IcarusInternal.dll      ← The trainer payload
│       └── IcarusInjector.exe      ← CLI injector (optional headless path)
└── (electron runtime + icu data)
```

The `bin/IcarusInternal.dll` is the **only** file that has to end up
mapped inside the game process. Everything else is host-side tooling.

---

## 2. Launch Icarus, then ZeusMod

1. Start *Icarus* from Steam and load into a prospect (any biome, any
   mode — prospect has to be live, not sitting at the main menu).
2. Open ZeusMod. The sidebar game-tile turns green with the text
   *"Game detected — ready to attach"*. If it stays red with *"Launch
   Icarus first"*, re-check that `Icarus-Win64-Shipping.exe` is in
   your task list.
3. Click **ATTACH TO ICARUS**. The launcher calls
   `injector.js → injectDll(pid, dllPath)` — a koffi FFI binding that
   does `OpenProcess` → `VirtualAllocEx` → `WriteProcessMemory` →
   `CreateRemoteThread(LoadLibraryW)` against the running game.
   End-to-end latency: **50–150 ms**.
4. The status dot goes solid cyan and says *"Attached — cheats are
   live"*. The button label flips to **CONNECTED**.

The DLL runs `DllMain(DLL_PROCESS_ATTACH)` → spins up a dedicated
trainer thread → resolves offsets via UE reflection → installs MinHook
detours → opens the named pipe `\\.\pipe\ZeusModPipe` → the pipe
server is now ready for the UI.

---

## 3. Toggle your first cheat

The desktop UI has six category panels; the default view is **Survival**.
Flip the toggle on any card, for example **Infinite Stamina**. Behind
the scenes:

```text
checkbox "change" event
  → window.zeusmod.toggleCheat("stamina", "1")          [preload.js]
  → ipcMain "cheat:toggle" handler                       [main.js]
  → pipe write: "stamina:1"                              [main.js]
  → DLL pipe-server HandleLine("stamina:1")              [Trainer.cpp]
  → Trainer::Get().Stamina = true                        [Trainer.cpp]
  → on the next tick: the stamina hooks start firing
```

Un-toggling re-sets the bool. There's no reload, no reinject.

---

## 4. Unload without closing the game

Two clean exits:

| Method | When |
|--------|------|
| Press <kbd>F10</kbd> **inside Icarus** | You want to stay in-prospect but drop the trainer. MinHook detours unhook, the overlay tears down, the pipe server shuts, and `FreeLibrary` runs. |
| Close ZeusMod | The desktop UI disconnects but the DLL keeps running. Re-open ZeusMod and click Attach again to re-connect the pipe — no re-inject. |

---

## 5. Headless path (power users)

Nothing about ZeusMod requires the Electron launcher. If you want to
script inject + cheats from a terminal:

```powershell
# Inject
"%LOCALAPPDATA%\Programs\ZeusMod\resources\bin\IcarusInjector.exe" ^
  "Icarus-Win64-Shipping.exe" ^
  "%LOCALAPPDATA%\Programs\ZeusMod\resources\bin\IcarusInternal.dll"

# Drive from Python
python scripts\inspect.py -c "godmode 1; stamina 1; speed_mult 4.0"

# Or open the REPL
python scripts\inspect.py
```

All the commands the UI exposes are reachable from `inspect.py`. See
[Debug Client](Debug-Client.md).

---

## 6. Updating

ZeusMod ships with `electron-updater` wired to the GitHub Releases
feed. When a new tag is pushed:

1. CI builds `ZeusMod-Setup-X.Y.Z.exe` + `latest.yml` and attaches
   them to the release.
2. Your running client polls the feed, notices the bump, shows the
   *"Update available"* modal with the changelog entry, and downloads
   the installer.
3. Click **Download &amp; Install** — the installer runs silently and
   re-launches ZeusMod on the new version.

Force-check at any time with **Check for Updates** in the sidebar
footer.

---

## Next steps

- [**Feature Reference**](Feature-Reference.md) — what each cheat does and how it's implemented.
- [**Debug Client**](Debug-Client.md) — drive the trainer from Python.
- [**Architecture**](Architecture.md) — all four components in one diagram.
- [**Troubleshooting**](Troubleshooting.md) — if Attach fails or a cheat doesn't stick.
