# Contributing to ZeusMod

Thanks for taking the time to contribute. This document describes the layout of
the repository, the build flow, and the conventions to follow when opening a
pull request.

## Repository layout

```
ZeusMod/
├── .github/workflows/      CI (tag push → GitHub Release)
├── app/                    Electron desktop launcher + auto-updater
├── docs/                   Architecture, build instructions, screenshots
├── native/
│   ├── shared/             Static lib shared by injector + internal DLL
│   ├── injector/           IcarusInjector.exe (external injector)
│   ├── internal/           IcarusInternal.dll (injected trainer)
│   └── third_party/        ImGui + MinHook (fetched by CI, gitignored)
├── CHANGELOG.md
├── LICENSE
├── README.md
└── ZeusMod.sln
```

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the component boundaries
and runtime flow, and [`docs/BUILD.md`](docs/BUILD.md) for the full build
instructions.

## Coding conventions

- **C++** — C++20, 4-space indent, `PascalCase` types, `camelCase` locals,
  `m_` prefix for non-static members, left-aligned pointers (`T* name`).
  `.clang-format` at the repo root captures the default style.
- **JavaScript (Electron)** — 4-space indent, single quotes, trailing semicolons.
- **Shell / YAML** — 2-space indent.
- **Commit messages** — imperative mood, past-tense-free
  (`Fix crash in Render::Hook`, not `Fixed` / `Fixing`).
- Keep diffs focused. Unrelated formatting churn belongs in its own commit.

## Build & test loop

| Task                       | Command                                                       |
|----------------------------|---------------------------------------------------------------|
| Native build (local dev)   | Open `ZeusMod.sln` in VS 2022 → Build `Release\|x64`         |
| Native build (CLI)         | `msbuild native/internal/IcarusInternal.vcxproj /p:Configuration=Release /p:Platform=x64` |
| Electron dev run           | `cd app && npm install && npm start`                          |
| Electron installer build   | `cd app && npm run dist`                                      |

The CI workflow at `.github/workflows/release.yml` is the source of truth for
how releases are produced — if you change project paths or dependencies, update
that file in the same PR.

## Releasing

1. Bump `app/package.json` `version` and add an entry to `CHANGELOG.md`.
2. Commit and push.
3. Tag `vX.Y.Z` and push the tag. CI builds the installer, standalone ZIP, and
   publishes the GitHub Release.
