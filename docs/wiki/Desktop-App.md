# Desktop App

`ZeusMod.exe` is the Electron desktop companion. It detects the
Icarus install, injects `IcarusInternal.dll`, forwards cheat toggles
to the pipe, and delivers auto-updates. This page is the internal
tour — file-by-file.

---

## Stack

| Layer              | Choice                           | Why                                              |
|--------------------|----------------------------------|--------------------------------------------------|
| Runtime            | Electron 33                      | Stable Chromium + Node combo, NSIS support       |
| Packager           | `electron-builder` 26            | Silent NSIS + `latest.yml` for `electron-updater`|
| Auto-updater       | `electron-updater` 6             | GitHub Releases provider, differential downloads |
| FFI                | `koffi` 2                        | Pure-JS native syscalls, no node-gyp            |

No React, no bundler, no TypeScript. The renderer is a single
`index.html`, a single `app.js`, and a single `main.css`. Same thing
on the main side: `main.js`, `preload.js`, `injector.js`. Deliberate
— keeps the attack surface small and the cold start fast.

---

## Main process

### `src/main/main.js`

Responsibilities:

- Create the `BrowserWindow`, wire up the frameless titlebar.
- Register `ipcMain` handlers:
  - `game:status` — returns `{installed, installDir, running}`.
  - `game:inject` — calls `injectDll` (see below).
  - `cheat:toggle` — writes `<cmd>:<value>\n` to the cheats pipe.
  - `update:check` / `update:install` / `update:open-page` — proxy
    to `electron-updater`.
- Register the `autoUpdater` provider + event listeners. Every
  status transition (`checking`, `available`, `downloading`,
  `downloaded`, `error`) broadcasts to the renderer via
  `mainWindow.webContents.send('update:state', state)`.

### `src/main/injector.js`

Pure koffi FFI binding to `kernel32.dll`. The public API:

```js
const { injectDll, InjectError } = require('./injector');
const res = injectDll(pid, dllPath);     // { moduleBase: BigInt }
```

Internally:

```text
OpenProcess(PROCESS_ALL_ACCESS, false, pid)
VirtualAllocEx(hProc, NULL, len, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)
WriteProcessMemory(hProc, alloc, utf16(dllPath+"\0"))
CreateRemoteThread(hProc, NULL, 0,
                   GetProcAddress(GetModuleHandleW("kernel32"), "LoadLibraryW"),
                   alloc, 0, NULL)
WaitForSingleObject(hThread, INFINITE)
GetExitCodeThread  → moduleBase (HMODULE)
VirtualFreeEx(hProc, alloc, 0, MEM_RELEASE)
CloseHandle(hThread)
CloseHandle(hProc)
```

`InjectError` wraps every failure with a code (`WIN32`, `BAD_PID`,
`LOAD_FAILED`, `TIMEOUT`, `TRUNCATED`) so the renderer can display a
useful error instead of a raw errno. A `try/finally` block
guarantees `VirtualFreeEx` + `CloseHandle` run even on exceptions.

**Why not PowerShell.** The pre-1.5 path spawned PowerShell +
`Add-Type` which JIT-compiled a C# remote-thread-injection helper.
That added ~1 second of latency, spawned a visible child process,
and tripped AV heuristics on some machines. koffi is pure JS, ships
as a prebuilt binary, and the end-to-end inject is 50–150 ms.

### `src/main/preload.js`

`contextBridge` that exposes `window.zeusmod.*` to the renderer.
Kept intentionally small — every method is a thin `invoke()` over
one `ipcMain` handler. The renderer never touches `fs`, `net`, or
`child_process` directly.

---

## Renderer

### `src/renderer/index.html`

Structure:

```
┌ titlebar ───────────────────────────────────────────────────┐
│  ZeusMod  vX.Y.Z | Icarus trainer        [ _ ] [ □ ] [ × ]  │
├ sidebar ──────────┬──────────── content ─────────────────────┤
│ [ Icarus tile   ] │  status strip (dot + ATTACH TO ICARUS)   │
│ ▶ Survival        │  ───────────────────────────────────────  │
│   Inventory       │  <category panel>.card-grid              │
│   Character       │    [ cheat card ] [ cheat card ] ...     │
│   World           │                                          │
│   Progression     │                                          │
│   Give Items      │                                          │
├───────────────────┤                                          │
│ Status | Injected │                                          │
│ Updates | v1.5.0  │                                          │
│ [ Check Updates ] │                                          │
│ Made by CyberSnake│                                          │
└───────────────────┴──────────────────────────────────────────┘
```

Six nav items, each swapping a `.cat-panel` in the main area. Cheat
cards are plain `<article>` blocks with a single `<input
type="checkbox" data-cheat-toggle="<cmd>">` — the renderer wires
them to the pipe via one event handler.

### `src/renderer/js/app.js`

- `wireTitlebar()` — min / max / close.
- `wireSidebarNav()` — swap active panel; mirror the Character
  category into Progression at first visit.
- `wireCheatToggles()` — every `[data-cheat-toggle]` checkbox fires
  `window.zeusmod.toggleCheat`.
- `wireSpeedStepper()`, `wireTimeStepper()` — value steppers.
- `refreshGameStatus()` — polls `game:status` every 3 s when not
  injected.
- `renderUpdate()` / `showUpdateModal()` — render auto-updater state
  into the footer chip + modal.

No framework. The renderer fits in a few hundred lines.

### `src/renderer/styles/main.css`

- Palette: `--bg-0: #0a0d14`, accent `--acc-cyan: #00d2ff`,
  `--acc-purple: #8b5bff`.
- Inter / Segoe for body, JetBrains Mono for code.
- Radial gradient background, card grid, pulse animation on the
  status dot.
- Credit line under the sidebar footer uses a cyan → purple gradient
  `-webkit-background-clip: text`.

---

## Packaging

`npm run dist` produces three artefacts under `app/dist/`:

| Artefact                          | Who reads it                                         |
|-----------------------------------|------------------------------------------------------|
| `ZeusMod-Setup-X.Y.Z.exe`         | Users. Silent NSIS installer.                        |
| `latest.yml`                      | `electron-updater`. Release manifest.                |
| `ZeusMod-Setup-X.Y.Z.exe.blockmap`| `electron-updater`. Differential download manifest.  |

The install target is `%LOCALAPPDATA%\Programs\ZeusMod` (per-user,
no UAC). `oneClick: true` makes the install silent. `runAfterFinish:
true` relaunches ZeusMod after the installer closes.

---

## Auto-update flow

On launch:

1. `autoUpdater.checkForUpdatesAndNotify()` fires at boot and every
   30 minutes.
2. If a newer version is published on the GitHub Releases feed, the
   main process broadcasts `update:state = available` with the
   release tag + notes parsed out of the release body.
3. The renderer opens the update modal. User clicks **Download &
   Install** → `autoUpdater.downloadUpdate()` begins.
4. Progress events stream via `update:state = downloading` (with a
   `progress` percentage field).
5. On `update-downloaded`, the main process calls
   `autoUpdater.quitAndInstall(isSilent=true, isForceRunAfter=true)`
   and the installer runs silently and re-launches ZeusMod.

The **Check for Updates** button in the sidebar footer forces step 1
on demand.

---

## IPC surface (complete)

| `window.zeusmod.*`           | Corresponding `ipcMain` handler | Returns                                              |
|------------------------------|---------------------------------|------------------------------------------------------|
| `getVersion()`               | *direct, not IPC*               | `app.getVersion()`                                   |
| `minimize() / maximize() / close()` | window ops               | `void`                                               |
| `getGameStatus()`            | `game:status`                   | `{installed, installDir, running}`                   |
| `injectGame()`               | `game:inject`                   | `{success, output?, error?, pid?, moduleBase?}`      |
| `toggleCheat(name, value)`   | `cheat:toggle`                  | `{success, error?}`                                  |
| `getUpdateState()`           | `update:state-sync`             | current `{status, ...}` snapshot                     |
| `onUpdateState(cb)`          | subscribes to broadcasts        | unsubscribe fn                                       |
| `checkForUpdates()`          | `update:check`                  | `void`                                               |
| `installUpdate()`            | `update:install`                | `{success, error?}`                                  |
| `openReleasePage()`          | `update:open-page`              | `void`                                               |

---

## See also

- [Architecture](Architecture.md) — where the Electron app fits.
- [Getting Started](Getting-Started.md) — user-facing install flow.
- [Release Process](Release-Process.md) — how releases get cut and picked up by the updater.
