# Troubleshooting

The symptoms you're most likely to hit, and the one-line diagnostic
for each. If you hit something that isn't here, file a
[bug report](https://github.com/CyberSnakeH/ZeusMod/issues/new/choose)
with the logs + the version visible in the ZeusMod titlebar.

---

## Install / launch

### "Icarus is not installed" in the sidebar

- The installer folder-lookup walks every Steam library folder
  listed in `…\Steam\steamapps\libraryfolders.vdf`. If your Icarus
  install is on a drive that isn't registered there, ZeusMod can't
  find it.
- **Fix** — launch Icarus through Steam at least once, or add the
  library folder in Steam → *Settings → Storage*.

### ZeusMod window won't open / crashes immediately

- Electron crash logs land at `%APPDATA%\ZeusMod\logs\`. Send the
  most recent `*.log` when filing a bug.
- Most recurring cause: graphics driver blocking hardware
  acceleration. Launch with
  `ZeusMod.exe --disable-gpu` once to confirm.

---

## Attach / inject

### Clicking Attach does nothing / status stays yellow

1. Check that `Icarus-Win64-Shipping.exe` is actually in your task
   list **and** has finished loading a prospect. Attaching against
   the main menu sometimes fails because not every class we resolve
   is loaded yet.
2. If it still fails, open the DevTools (<kbd>Ctrl+Shift+I</kbd>)
   and read the error on the Attach button's status strip.

| Error token         | Meaning                                                        | Fix                                                             |
|---------------------|----------------------------------------------------------------|-----------------------------------------------------------------|
| `BAD_PID`           | tasklist parser didn't find the game                           | Relaunch Icarus; make sure you're past the loading screen       |
| `WIN32 5`           | `ERROR_ACCESS_DENIED` on `OpenProcess`                         | Run ZeusMod as admin **or** disable any AV that hardens Icarus  |
| `LOAD_FAILED`       | `LoadLibraryW` returned 0 inside the target                    | The DLL path is wrong, or the DLL's dependencies didn't resolve |
| `TIMEOUT`           | `CreateRemoteThread` never signalled                            | Icarus's main thread stalled. Wait 30 s and retry.              |

### `LOAD_FAILED` but the DLL path is correct

- The DLL was probably built with a VCRuntime the target doesn't
  ship. Ensure you built against the VS 2022 v143 toolset (matches
  the engine runtime used by retail Icarus). See
  [Build From Source](Build-From-Source.md).

### Attach succeeds, cheats don't take effect

- Open `inspect.py` and run `character`. You should get
  `OK character=0x... Off::Player_InventoryComp=0x758`.
  - **If you get `ERR ...`** — the class resolution failed, most
    likely because a prospect isn't fully loaded. Wait for the HUD
    to appear, try again.
  - **If you get an address of `0x0`** — the player pawn hasn't
    been spawned yet. Load fully into the world and retry.

---

## In-game

### Game crashes during a PhysX tick with `FPhysScene_PhysX::TickPhysScene`

This was the symptom that drove the 1.5.0 No Weight rewrite. If
you're seeing it:

- Make sure you're on **1.5.0 or newer**. Earlier builds clamped
  `MaxWalkSpeed = 600` and called `ExpireOverburdenedModifier`, both
  of which left the PhysX tick reading a stale pointer.
- 1.5+ hooks `AddModifierState` instead — no more PhysX issue.

### "No Weight" is on but the character still feels heavy

- Open the UI and confirm the toggle is actually engaged (card
  should be accent-coloured, not grey).
- If it is, the DLL version doesn't match the client — close Icarus
  and relaunch the launcher. `app/bin/IcarusInternal.dll` should be
  1.5.0 or newer. See [Release Process](Release-Process.md) to find
  out how to read the DLL version.

### Free Craft lets me queue but the item never completes

- Some recipes run on `DeployableTickSubsystem`, which we inject
  into via `+0x60`. If a content patch changes that subsystem
  layout, the injection is a no-op.
- Send the recipe name + the station type you were at in the bug
  report — we can validate the offset against a dev build.

---

## Debug client

### `inspect.py` → `ImportError: cannot import name 'dataclass' from dataclasses`

- The script is named `inspect.py`, which shadows Python's stdlib
  `inspect`. There is a guard at the very top of the file that
  strips its own directory from `sys.path`. Don't rename the file
  without updating that guard.

### Every command returns `ERR unknown primitive '<name>'`

- The pipe is open but the running DLL doesn't recognise the
  primitive. That usually means the DLL in memory is older than the
  client expects:
  1. Close Icarus.
  2. Verify `app/bin/IcarusInternal.dll` is the latest build
     (`md5sum native/internal/bin/Release/IcarusInternal.dll
     app/bin/IcarusInternal.dll` should match).
  3. Relaunch Icarus → Attach → retry.
- The other possibility: the command isn't in the client's
  `DBG_PRIMITIVES` catalog. That's the bug we fixed in 1.5.0 — make
  sure your `scripts/inspect.py` is also up to date.

### `UnicodeEncodeError: 'charmap' codec can't encode character '\u25b8'`

- The Windows code page default (cp1252) can't render the arrow
  glyph. The script reconfigures `sys.stdout` to UTF-8 at startup
  with `errors="replace"` — if you still see this, you're on an
  ancient Python. Use 3.9+.

### `snapshot bp1 ...` then `diff bp1` → `ERR no such snapshot`

- Snapshots are **in-process only**. They persist across commands
  inside a single `inspect.py` invocation (REPL session or a single
  `-c "..."` batch) but not between separate launches. Capture and
  compare inside the same session.

---

## Auto-updates

### Update modal opens but download stalls at 0 %

- The GitHub Releases endpoint is sometimes slow for the first few
  seconds. Give it 10–15 s before assuming it's broken.
- If it's been stuck for minutes, open `%APPDATA%\ZeusMod\logs\*.log`
  — `electron-updater` writes verbose download progress there,
  including any HTTP error.

### Installed 1.x, opened 1.x, and the Updates chip says `—`

- `electron-updater` hasn't run yet. Click **Check for Updates** in
  the sidebar footer to force a check. The first auto-check
  otherwise happens 30 s after launch.

---

## File a bug

Still stuck? Open a
[bug report](https://github.com/CyberSnakeH/ZeusMod/issues/new/choose)
with:

- ZeusMod version (titlebar).
- Icarus build ID (Steam → Icarus → Properties → Updates).
- What you clicked / what command you ran.
- What happened vs what you expected.
- The most recent `%APPDATA%\ZeusMod\logs\*.log` if relevant.
