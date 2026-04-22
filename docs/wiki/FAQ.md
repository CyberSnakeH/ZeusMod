# FAQ

Short answers to the questions that come up most often. If your
question isn't here, check [Troubleshooting](Troubleshooting.md)
first, then file an issue.

---

## About the project

### What is ZeusMod?

A single-player research trainer for Icarus, built as an internal
DLL plus an Electron desktop launcher plus a Python debug client.
See [Home](Home.md) or [Architecture](Architecture.md).

### Who made this?

[CyberSnake](https://github.com/CyberSnakeH). Sole author.

### Can I use it online / in PvP / on official servers?

No. ZeusMod is **single-player / private session only**. Don't use
it on official multiplayer servers. You're responsible for how you
use it — read the disclaimer in [LICENSE](../../LICENSE) and in the
[README](../../README.md).

### Is it detectable?

ZeusMod makes no effort to hide. The DLL is injected with a
textbook `CreateRemoteThread(LoadLibraryW)`, installs visible
MinHook detours, and writes to a named pipe with a predictable
name. If Icarus ever shipped an anti-cheat, every one of those
signals would trip. Today, Icarus does not — but don't count on
that to stay true, and again: single-player only.

---

## Install & update

### I'm on 1.4.x. Do I have to reinstall?

Once. Download `ZeusMod-Setup-1.5.0.exe` manually from the Releases
page and run it. After that, every subsequent release is picked up
by the in-app updater automatically. The 1.4.x → 1.5.0 jump is the
last manual step; the migration note on the 1.4.1 changelog explains
why.

### Does ZeusMod require admin / UAC?

No. The NSIS installer is `oneClick: true, perMachine: false`, which
installs per-user into `%LOCALAPPDATA%\Programs\ZeusMod`. The
injector also doesn't require admin — Icarus runs at medium
integrity, so a medium-integrity ZeusMod can open the process.

### Does ZeusMod modify Icarus's game files?

No. `Icarus-Win64-Shipping.exe` and the Icarus install directory are
never written to. The DLL lives entirely in memory of the running
process. Uninstalling ZeusMod removes nothing from your Icarus
install.

---

## Features

### Why is No Weight different in 1.5?

Earlier builds tried to neuter weight by clamping
`MaxWalkSpeed = 600` and forcing
`ExpireOverburdenedModifier`. Both caused a PhysX-tick
access-violation (`FPhysScene_PhysX::TickPhysScene` reading a stale
pointer). 1.5+ hooks `IcarusFunctionLibrary::AddModifierState` at
the source and refuses the Overburdened row — the modifier is never
applied, no clamp to roll back, no PhysX issue. See
[Feature Reference → No Weight](Feature-Reference.md#no-weight--weight)
and [Hook Catalog](Hook-Catalog.md#addmodifierstate--no-weight-15).

### Can I change the Speed Hack multiplier?

Yes — use the ± steppers on the **Speed Hack** card, or
`speed_mult <float>` on the pipe. Range is 0.5 to 10.0 in 0.5
increments from the UI; the pipe accepts any float.

### Can I give myself any item?

Yes — the **Give Items** panel lets you spawn any row from
`D_ItemTemplate` into the player's bag. From the pipe:
`give <RowName>,<count>`. See
[Feature Reference → Give Items](Feature-Reference.md#give-items--give).

### Why doesn't the Progression panel show its own cheats?

By design, to keep things consolidated. On first visit the panel
mirrors the **Character** category (Mega XP, Max Talent / Tech /
Solo). Toggling a card in either view updates both. See
`mirrorCategory()` in `app/src/renderer/js/app.js`.

---

## Debug client

### Why is the Python script called `inspect.py`? Doesn't that conflict with stdlib?

It does, and the file has a small guard at the top to strip its
own directory from `sys.path` so Python's stdlib `inspect` still
wins when internal modules `import inspect`. Don't rename the file
without updating that guard — or you'll recreate the circular
import from the 1.5.0 CHANGELOG.

### Do I need to install anything to run `inspect.py`?

A vanilla Python 3.9+ install is enough. `rich` and `pyreadline3`
make the output prettier and add tab completion, and `capstone`
unlocks the `disasm` command — all three are optional.

```powershell
pip install -r scripts/requirements.txt
```

### Why do snapshots not persist across sessions?

By design — snapshots are meant for "poke something, see what
changed", not long-term annotations. Labels + bookmarks persist
(`~/.zeusmod_labels.json`), snapshots don't.

---

## Internals

### Why reflection instead of AOB signatures?

Every signature we tried against Icarus broke within two patches.
UE's reflection graph is stable enough across patches that
name-based lookup survives most of them. See
[Reflection Internals](Reflection-Internals.md).

### What engine version does Icarus use?

Unreal Engine 4.27. ZeusMod bakes in a handful of UE-4.27-specific
layout assumptions (see [Memory Layout](Memory-Layout.md)) that
would need re-validation on UE5.

### Why Electron and not native Win32 for the launcher?

Shipping speed and maintenance cost. Every feature of the launcher
that matters (process detection, pipe client, auto-update, native
inject via koffi) is a thin wrapper around a syscall. Electron is
the boring path — the trainer itself is where the interesting
engineering lives.

---

## License / redistribution

### Can I fork this and reuse the code?

Yes — the project is MIT-licensed. Keep the license file; don't
claim affiliation with RocketWerkz; use your own name on forks.

### Can I package and distribute a modified version?

Yes. MIT allows it. You're responsible for what happens after,
including any bug reports — upstream only fixes things on the
CyberSnake build.

---

## See also

- [Home](Home.md) — wiki landing page.
- [Troubleshooting](Troubleshooting.md) — symptoms and fixes.
- [Glossary](Glossary.md) — the words this wiki uses.
