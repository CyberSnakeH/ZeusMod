# Contributing

If you want to extend ZeusMod — a new cheat, a new debug primitive,
UI work, docs — this page covers what to read, how to structure the
change, and what reviewers will look for.

> **Short version.** Build from source, add your change, run the
> manual test plan, open a PR from a feature branch against `main`,
> and wait for review. There is no squash merge — history is kept
> linear via merge commits so tags stay valid.

---

## Before you start

- Read [Architecture](Architecture.md) to know which component owns
  the change.
- Skim [Feature Reference](Feature-Reference.md) and [Hook
  Catalog](Hook-Catalog.md) for examples of how existing cheats are
  structured.
- Make sure you can build locally — see [Build From Source](Build-From-Source.md).

---

## Coding style

### C++ (`native/`)

- MSVC v143, `/std:c++17`, `/W4`, warnings treated as errors on new
  files.
- `clang-format` rules live in `.clang-format` at the repo root.
  Format before committing: the easiest path is to enable
  "Format on save" pointing at that file in VS.
- Prefer **free functions in anonymous namespaces** over static
  methods on helper classes.
- Keep SEH (`__try` / `__except`) contained in helper functions that
  don't construct C++ objects — MSVC C2712 does not forgive it
  otherwise.
- `UObjectLookup` and `TrainerResolve` own reflection resolution.
  Don't bypass them with hand-written AOB scans.
- One cheat → one file (`Trainer<CheatName>.cpp`) if it needs more
  than a dozen lines of logic. Short cheats can live in the main
  `Trainer.cpp`.

### JavaScript (`app/`)

- ES2022, plain `.js`. No TypeScript, no JSX.
- Preload bridge only — the renderer never touches `fs`, `net`, or
  `child_process`.
- Prettier defaults: 4-space indent, single quotes, trailing comma
  `es5`.

### Python (`scripts/`)

- Target: `3.9+`. Don't reach for syntax that needs 3.12.
- Type hints on function signatures are encouraged.
- Keep optional deps (`rich`, `capstone`, `pyreadline3`) actually
  optional — wrap their imports in `try/except ImportError`.
- Every new command must be added to the `COMMANDS` catalog with a
  `group`, a `desc`, a `usage`, and an `example`. This drives both
  the `DBG_PRIMITIVES` gate and the `help` system.

### Markdown (`docs/wiki/`, `CHANGELOG.md`, `README.md`)

- Wrap at 76–80 columns where reasonable.
- One `#` h1 per page, sub-sections under `##` and below.
- Cross-reference pages with relative links:
  `[Feature Reference](Feature-Reference.md)`.

---

## Commit convention

- Imperative mood, ≤72 chars on the first line.
- Group related changes into a single commit — no "wip" commits in
  the final PR.
- Example subjects:
  - `Release 1.5.0 — Native injector, AddModifierState hook, …`
  - `Fix CI: stop electron-builder from auto-publishing`
  - `Add Infinite Oxygen cheat`
- **No** co-author trailers and **no** AI attribution.

---

## PR conventions

- Branch name: `feature/<slug>` or `fix/<slug>`. Release branches
  are `release/vX.Y.Z`.
- PRs target `main`. Keep them scoped — one feature per PR.
- Use the PR template — in particular, fill the *How it was tested*
  checklist honestly. Reviewers will ask if it's empty.
- Merges are always **"Create a merge commit"**. Squash would break
  release tags.

---

## Local manual test plan

No automated tests. The expected smoke test for any change is:

1. Fresh build (`Release | x64`) succeeds.
2. Fresh `npm ci && npm start` launches ZeusMod.
3. Attach to a running Icarus succeeds (50–150 ms).
4. The cheat you touched toggles on/off without side-effects.
5. `inspect.py -c "character"` still returns an `OK ...` line.
6. No regression in the other cheats — run through the full UI
   once before opening the PR.

If you change the pipe protocol or reflection resolution, add a
line to the PR description stating so. Reviewers will ask for extra
testing on those paths.

---

## Docs are load-bearing

Changing a cheat, a hook, or the pipe surface without updating
[Feature Reference](Feature-Reference.md), [Hook
Catalog](Hook-Catalog.md), [Pipe Protocol](Pipe-Protocol.md), or
[Memory Layout](Memory-Layout.md) will get the PR sent back.

For new releases, **`CHANGELOG.md` comes first**: the release CI
pulls release notes out of it, so the changelog entry is
user-visible.

---

## Reporting without writing code

- Bugs → [bug report](https://github.com/CyberSnakeH/ZeusMod/issues/new?template=bug_report.yml).
- Feature ideas → [feature request](https://github.com/CyberSnakeH/ZeusMod/issues/new?template=feature_request.yml).
- Architecture / strategy discussion → a plain issue. No Discussions
  tab yet.

---

## Scope we won't take

- **Multiplayer bypasses.** ZeusMod is single-player / private
  research only. We don't accept code that hides from anti-cheat or
  desynchronises the server's view of the world.
- **Exploits targeting other players.** Same reason.
- **Closed-source binary blobs.** Every binary dependency lives in
  `native/third_party/` fetched from upstream; nothing opaque gets
  vendored.
