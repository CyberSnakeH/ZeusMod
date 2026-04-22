# Release Process

How a new ZeusMod release gets cut, tested, and shipped to existing
users over-the-air.

---

## Cadence

There is no fixed cadence. Releases ship when:

- A new feature is done and manually verified in-game.
- A blocking bug is fixed.
- An Icarus patch broke something reflection didn't catch.

Expect roughly one release every two to four weeks in active
periods, and long quiet stretches between them.

---

## Versioning

[Semantic versioning](https://semver.org/), with the same scheme
the Electron ecosystem uses:

| Change                                   | Bump   |
|------------------------------------------|--------|
| Fix a bug, no new features               | patch  |
| Add a cheat, a UI card, a debug command  | minor  |
| Breaking change to the pipe protocol, a migration requiring manual intervention, or a removal | major |

The version tag is `vX.Y.Z` (with the `v` prefix). Tag format is
load-bearing — the CI workflow in `.github/workflows/release.yml`
filters on it.

---

## Release checklist

Before cutting a release:

- [ ] Native + Electron builds are green locally in Release | x64.
- [ ] `app/bin/IcarusInternal.dll` matches
  `native/internal/bin/Release/IcarusInternal.dll` (same md5).
- [ ] New features were exercised live against a running Icarus —
  toggles, UI, `inspect.py` smoke tests.
- [ ] Update modal picks up the release body cleanly (no YAML
  escaping issues).
- [ ] `CHANGELOG.md` has a `## [X.Y.Z] - YYYY-MM-DD` section at the
  top with Highlights / Added / Changed / Fixed subheadings.
- [ ] `app/package.json → version` bumped to `X.Y.Z`.
- [ ] `README.md` badge bumped.
- [ ] No AI co-author trailer or third-party attribution leaked into
  commits. Author must be `CyberSnakeH` only.

---

## Cutting the release

1. Commit everything to a short-lived branch:

   ```bash
   git checkout -b release/vX.Y.Z
   git add -A
   git commit -m "Release X.Y.Z — one-line summary"
   ```

2. Open a PR against `main`. Use the PR template; include the test
   plan from the checklist above.

3. Once the PR looks good, push the tag **to the commit that will be
   merged**:

   ```bash
   git tag -a vX.Y.Z -m "ZeusMod X.Y.Z — one-line summary"
   git push origin vX.Y.Z
   ```

4. Merge the PR with **"Create a merge commit"** (not squash). Squash
   would rewrite the commit hash and invalidate the tag.

5. CI picks up the tag push and cuts the GitHub Release (see below).

---

## What CI does on a tag push

From `.github/workflows/release.yml`:

1. **Checkout** — Windows runner, shallow clone.
2. **Native deps** — `scripts/fetch-native-deps.ps1` fetches
   ImGui + MinHook into `native/third_party/`.
3. **Build native** — `msbuild ZeusMod.sln /t:Rebuild
   /p:Configuration=Release /p:Platform=x64`.
4. **Stage binaries** — copy
   `native/internal/bin/Release/IcarusInternal.dll` and
   `native/injector/bin/Release/IcarusInjector.exe` into
   `app/bin/`.
5. **Build installer** — `cd app && npm ci && npm run dist`.
   Produces:
   - `ZeusMod-Setup-X.Y.Z.exe`
   - `latest.yml`
   - `ZeusMod-Setup-X.Y.Z.exe.blockmap`
6. **Compose release notes** — extract the `## [X.Y.Z]` block from
   `CHANGELOG.md`, prepend to `.github/release-footer.md`.
7. **Publish** — `gh release create vX.Y.Z --notes-file notes.md
   <installer> <latest.yml> <blockmap>`.

If any step fails, the workflow exits non-zero and no release is
published. The tag stays in place — fix the problem and either push
another commit (the tag is re-associated with a push to the release
branch, and you retag from the new commit) or delete + retag:

```bash
git tag -d vX.Y.Z
git push --delete origin vX.Y.Z
git tag -a vX.Y.Z -m "…"
git push origin vX.Y.Z
```

Deleting a tag is a destructive op — only do it before the release
is live to end users.

---

## After the release goes live

1. Confirm the GitHub Release page shows the installer + `latest.yml`
   + blockmap.
2. Open ZeusMod on a test machine still running the previous
   version. Within 30 s the Updates chip should flip to the new
   version and the modal should offer **Download & Install**.
3. Click through. Silent NSIS runs, the app relaunches on the new
   version, and the footer chip flips to *"You are on the latest
   version"*.
4. Close the release PR and delete the `release/vX.Y.Z` branch.

---

## Hotfix flow

Same shape as a regular release, with two differences:

- The release branch is named `hotfix/vX.Y.(Z+1)`.
- The `CHANGELOG.md` entry lives under **Fixed** only. No **Added**
  or **Changed** on a hotfix.

If the hotfix is urgent (saves users from data loss / a crashing
game), skip writing new features onto the branch — ship the fix
alone.

---

## See also

- [Contributing](Contributing.md) — commit conventions, PR reviews.
- [Build From Source](Build-From-Source.md) — the exact build the CI runs.
- `CHANGELOG.md` at the repo root — canonical version history.
