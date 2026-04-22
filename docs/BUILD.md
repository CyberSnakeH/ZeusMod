# ZeusMod — Build instructions

## Prerequisites

- Windows 10 or 11 (x64)
- Visual Studio 2022 with the **Desktop development with C++** workload
- Windows 10 SDK (any recent revision)
- Node.js 20+ and npm (for the Electron app)

The v145 platform toolset is declared in the `.vcxproj` files; CI overrides to
v143 (VS 2022) on the runner. Either works.

## Native (C++)

### In Visual Studio

Open [`ZeusMod.sln`](../ZeusMod.sln) at the repo root. The solution contains
three projects:

- `Shared` (static lib) — `native/shared/`
- `IcarusInjector` (Win32 application) — `native/injector/`
- `IcarusInternal` (dynamic library) — `native/internal/`

Pick `Release | x64` and build.

Outputs land inside each project directory:

- `native/injector/bin/Release/IcarusInjector.exe`
- `native/internal/bin/Release/IcarusInternal.dll`

`IcarusInternal` also post-build-copies its `.dll` into
`native/injector/bin/Release/` so the external injector can load it from the
same folder.

### From the command line

```powershell
# 1. Fetch ImGui + MinHook into native/third_party/ (required before building internal).
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\fetch-native-deps.ps1

# 2. Build.
msbuild native/shared/Shared.vcxproj           /p:Configuration=Release /p:Platform=x64
msbuild native/injector/IcarusInjector.vcxproj /p:Configuration=Release /p:Platform=x64
msbuild native/internal/IcarusInternal.vcxproj /p:Configuration=Release /p:Platform=x64
```

## External dependencies (`native/third_party/`)

ImGui and MinHook are **not** vendored — the directory is gitignored. Both CI
and local dev populate it the same way, via
[`scripts/fetch-native-deps.ps1`](../scripts/fetch-native-deps.ps1):

```
native/third_party/
├── imgui/                  ← ImGui v1.91.8 (core + DX11/Win32 backends only)
└── minhook/
    ├── include/MinHook.h
    └── src/
        ├── {buffer, hook, trampoline}.{c,h}
        └── hde/            ← HDE64 disassembler (headers + compiled .c)
```

Run the script once per machine (or whenever you want to bump versions — see
the `-ImGuiTag` / `-MinHookBranch` parameters). The CI workflow at
`.github/workflows/release.yml` runs the equivalent logic inline so tag builds
stay deterministic even if the script changes.

## Electron app

```powershell
cd app
npm install
npm start              # run in dev mode
npm run dist           # produce ZeusMod-Setup-x.y.z.exe in app/dist/
```

The packaged installer expects the native binaries to already exist in
`app/bin/`:

```powershell
mkdir app\bin -Force
copy native\injector\bin\Release\IcarusInjector.exe  app\bin\
copy native\internal\bin\Release\IcarusInternal.dll  app\bin\
```

electron-builder bundles `app/src/**`, `app/scripts/**` and `app/bin/**` into
the NSIS installer and ships `app/scripts/inject.ps1` + both native binaries
alongside the Electron runtime.

## Release

1. Bump `app/package.json` `version`.
2. Add a new entry at the top of `CHANGELOG.md`:
   ```md
   ## [1.4.12] - YYYY-MM-DD
   - …
   ```
3. Commit: `Release 1.4.12 — <one-line summary>`.
4. Tag + push: `git tag v1.4.12 && git push origin v1.4.12`.

CI then:
- Builds the native binaries.
- Builds the Electron installer.
- Extracts the matching `CHANGELOG.md` section into the GitHub Release body.
- Publishes the installer, its blockmap, `latest.yml`, and a standalone
  `ZeusMod-Standalone-vX.Y.Z.zip`.
