# Build From Source

This page walks you through building every ZeusMod component on a fresh
Windows dev machine: the native trainer (`IcarusInternal.dll`), the
native injector (`IcarusInjector.exe`), and the Electron desktop app
(`ZeusMod.exe`).

---

## Prerequisites

| Tool                  | Tested version | Why                                                 |
|-----------------------|----------------|-----------------------------------------------------|
| Windows 10 / 11 x64   | 22H2+          | Target platform                                     |
| Visual Studio 2022    | 17.9+          | MSVC v143, Windows 10 SDK, CMake optional           |
| Node.js               | 20.x LTS       | Electron, electron-builder, koffi                   |
| Python                | 3.9+           | `inspect.py`, pre-build icon generation             |
| Git                   | 2.40+          | Everything                                          |
| (Optional) 7-Zip      | 22+            | For inspecting `.nupkg` installers during debugging |

Install the Visual Studio workload:

```
Desktop development with C++
  ✔ MSVC v143 — VS 2022 C++ x64/x86 build tools
  ✔ Windows 10 SDK (10.0.19041.0 or newer)
  ✔ C++ CMake tools for Windows
  ✔ Windows Universal CRT SDK
```

---

## 1. Clone and fetch third-party deps

```powershell
git clone https://github.com/CyberSnakeH/ZeusMod.git
cd ZeusMod

# Fetch ImGui + MinHook into native/third_party/
powershell -ExecutionPolicy Bypass -File scripts/fetch-native-deps.ps1
```

The fetch script pins specific commits of **Dear ImGui** and
**MinHook** and drops them under `native/third_party/`. That directory
is `.gitignored` on purpose — CI runs the same script and the
contents are reproducible.

---

## 2. Build the native solution

```powershell
# Command line (no VS GUI)
msbuild ZeusMod.sln /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Or open `ZeusMod.sln` in Visual Studio, set the active config to
**Release | x64**, and build the solution.

Outputs:

| Project            | Output                                                 |
|--------------------|--------------------------------------------------------|
| `Shared`           | `native/shared/bin/Release/Shared.lib`                 |
| `IcarusInjector`   | `native/injector/bin/Release/IcarusInjector.exe`       |
| `IcarusInternal`   | `native/internal/bin/Release/IcarusInternal.dll`       |

After a successful build, copy the DLL + injector into the Electron
app's `bin/` folder so the packaged launcher can ship them:

```powershell
Copy-Item native/internal/bin/Release/IcarusInternal.dll app/bin/
Copy-Item native/injector/bin/Release/IcarusInjector.exe app/bin/
```

---

## 3. Build the Electron app

```powershell
cd app
npm install
npm start                 # run in dev mode against app/src/
```

This hot-loads the renderer off `app/src/renderer/`. Making the
DevTools visible: open ZeusMod, <kbd>Ctrl+Shift+I</kbd>.

To **package** a release installer locally (e.g. to test
`electron-updater`):

```powershell
npm run dist              # builds dist/ZeusMod-Setup-X.Y.Z.exe
```

`electron-builder` reads `app/package.json → build{}` for the NSIS
config (oneClick, perMachine=false, runAfterFinish, installer icon).
The bundled DLL + injector come from `app/bin/` via the
`extraResources` entry.

---

## 4. Run the Python debug client

```powershell
# Optional polish (tab completion + rich tables)
pip install -r scripts/requirements.txt

# One-shot or REPL
python scripts/inspect.py
python scripts/inspect.py -c "character"
python scripts/inspect.py --watch "playerinv"
```

See [Debug Client](Debug-Client.md) for the full command catalog.

---

## CI — what GitHub Actions does on a tag push

File: `.github/workflows/release.yml`.

1. Checks out the repo on a Windows runner.
2. Runs `scripts/fetch-native-deps.ps1` — fetches ImGui + MinHook.
3. Builds `ZeusMod.sln` in `Release | x64` via MSBuild.
4. Copies `IcarusInternal.dll` + `IcarusInjector.exe` into `app/bin/`.
5. `npm ci && npm run dist` — `electron-builder` produces
   `ZeusMod-Setup-X.Y.Z.exe` + `latest.yml` + blockmap.
6. Reads the `## [X.Y.Z]` block out of `CHANGELOG.md`.
7. Reads `.github/release-footer.md` — static footer (download
   instructions, disclaimers).
8. Creates a GitHub Release with the composed body and uploads the
   installer / updater metadata / standalone zip.

The first step that fails fast blocks the release. Tag format:
`vX.Y.Z` (semver) — see [Release Process](Release-Process.md).

---

## Clean build (troubleshooting)

If a build gets into a weird state:

```powershell
# Native
Remove-Item -Recurse -Force native/internal/bin, native/internal/obj
Remove-Item -Recurse -Force native/injector/bin, native/injector/obj
Remove-Item -Recurse -Force native/shared/bin, native/shared/obj

# Electron
Remove-Item -Recurse -Force app/node_modules, app/dist
npm cache clean --force
```

Then start again from step 2.

---

## Common build errors

| Symptom                                                                              | Cause / fix                                                                                                                                           |
|--------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| `error C2712: cannot use __try in functions that require object unwinding`           | A `__try` block is mixed with C++ objects that need unwinding. Move the raw-memory probe into a separate helper function that builds no C++ objects. |
| `error MSB8036: The Windows SDK version 10.0.19041.0 was not found`                  | Install the matching SDK through the VS Installer, or point the project at the SDK you have via *Project Properties → General → Windows SDK Version*. |
| `LINK : fatal error LNK1104: cannot open file 'IcarusInternal.dll'`                  | Icarus still has the DLL mapped. Close the game (or press <kbd>F10</kbd> in it) and rebuild.                                                          |
| `electron-builder exit code 1 with "ERR_ELECTRON_BUILDER_CANNOT_EXECUTE_CODE_SIGN"`  | Code signing disabled in `package.json.build`. If you re-enable it, you must also set `CSC_LINK` / `CSC_KEY_PASSWORD` in the env.                     |
| `koffi` fails to bind kernel32                                                       | Ensure `npm install` completed without errors. koffi ships prebuilt binaries for Windows x64; the `node-gyp` fallback is not needed.                  |
| inspect.py throws `ImportError: cannot import name 'dataclass' from dataclasses`      | The script is named `inspect.py`, shadowing the stdlib. There is a guard at the top of the file — don't rename the file without updating the guard.  |

---

## See also

- [Architecture](Architecture.md) — what each project produces and how they cooperate.
- [Release Process](Release-Process.md) — tagging and shipping a new build.
- [Contributing](Contributing.md) — coding style, commit conventions, PR expectations.
