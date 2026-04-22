<h1 align="center"><code>inspect.py</code> — ZeusMod debug client</h1>

<p align="center">
  <em>x64dbg-style memory explorer + reflection inspector for the live Icarus
  process, talking to <code>IcarusInternal.dll</code> over a named pipe.</em>
</p>

<p align="center">
  <a href="#what-it-does">What it does</a> ·
  <a href="#requirements">Requirements</a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#command-catalog">Command catalog</a> ·
  <a href="#composites--workflows">Composites &amp; workflows</a> ·
  <a href="#tips">Tips</a>
</p>

---

## What it does

`scripts/inspect.py` is a command-line client for the debug pipe exposed by
`IcarusInternal.dll` (`\\.\pipe\ZeusModDbg`). It is what we use day-to-day to:

- Walk Unreal reflection (`UClass`, `UScriptStruct`, `UFunction`,
  `UPROPERTY`) on the **live** game process — no SDK dump, no rebuild.
- Read and write memory with typed primitives (`u32`, `u64`, `f32`, `f64`,
  ASCII / UTF-16 strings, `FGuid`, pointer arrays).
- Enumerate loaded modules, walk virtual memory (`VirtualQueryEx`), scan
  ranges for strings or typed values, and find pointer back-references —
  in the spirit of x64dbg.
- Decode raw memory through reflected `UPROPERTY` offsets via the
  `struct <UClass> <base>` composite.
- Toggle every cheat the in-game menu exposes (forwarded to the same DLL).

Everything is line-oriented, so it composes with batch scripts (`-c`),
watch mode (`--watch`), and JSON output (`--json`).

---

## Requirements

| Component                | Status                                                    |
|--------------------------|-----------------------------------------------------------|
| Python ≥ 3.9             | Required                                                  |
| `rich` ≥ 13              | Optional — pretty tables, syntax highlighting             |
| `pyreadline3` ≥ 3.4      | Optional — tab completion + history on Windows            |
| `capstone` (Python pkg)  | Optional — `disasm` command lights up                     |
| Icarus running + ZeusMod attached | Required — the named pipe only opens after inject |

```bash
# Optional polish (none required to run):
pip install -r scripts/requirements.txt
```

Without the optional packages the script falls back to plain ANSI output
and still does everything except disassembly.

---

## Quick start

```bash
# Single command (one-shot)
python scripts/inspect.py -c "character"

# Multiple commands, semicolons separate
python scripts/inspect.py -c "character = char; props Inventory; struct Inventory \$char"

# REPL — interactive shell with history + tab completion
python scripts/inspect.py

# Watch mode — re-run every 1 s, color-diff the output
python scripts/inspect.py --watch "playerinv" --interval 1

# JSON output — pipe into jq / other tooling
python scripts/inspect.py --json -c "character"

# Time every command
python scripts/inspect.py --timing -c "modules"
```

In the REPL, `help` lists all groups, `help <cmd>` shows usage + an example,
and `;` separates commands inside a single line.

---

## Command catalog

### Variables

The client carries a `VarStore` between commands:

| Token            | Meaning                                                 |
|------------------|---------------------------------------------------------|
| `0x...`          | Literal hex                                             |
| `<bare hex>`     | Literal hex (e.g. `1A124C91590`)                        |
| `$prev`          | Last hex value parsed out of the previous response      |
| `$<name>`        | Saved variable (set with `cmd args = name` suffix)      |
| `$char + 0x758`  | Arithmetic with `+` and `-`                             |

Save the result of any command with the trailing `= name` suffix:

```text
character = char         # $char now holds the player pawn pointer
read64 $char + 0x758 = comp
dump $comp 0x40
```

### `Lookup` — discover by name

| Command       | Usage                          | What it does                                            |
|---------------|--------------------------------|---------------------------------------------------------|
| `classof`     | `classof <addr>`               | UClass name of the UObject at `<addr>`                  |
| `nameof`      | `nameof <addr>`                | FName at `<addr>+0x18` (the UObject's own name)         |
| `findcls`     | `findcls <name>`               | UClass address by exact class name                      |
| `findstruct`  | `findstruct <name>`            | UClass OR UScriptStruct address by name                 |
| `findobj`     | `findobj <className>`          | First live non-CDO instance of `<className>`            |
| `findname`    | `findname <class>:<substr>`    | First instance whose name matches a substring           |
| `listobj`     | `listobj <class>:<N>`          | List up to N live instances of `<className>`            |
| `getbyindex`  | `getbyindex <idxHex>`          | `GObjects[idx]` (for `FWeakObjectPtr` decoding)         |
| `outer`       | `outer <addr>`                 | UObject outer chain (up to 8 levels)                    |
| `isa`         | `isa <addr>:<className>`       | Walk the Super chain for a class match                  |
| `fname`       | `fname <idxHex>`               | Resolve an FName ComparisonIndex to a string            |
| `fnameof`     | `fnameof <addr>`               | Read the FName whose 8 bytes sit at `<addr>`            |

### `Reflection` — typed UPROPERTY / UFunction access

| Command     | Usage                                       | What it does                                           |
|-------------|---------------------------------------------|--------------------------------------------------------|
| `props`     | `props <name>`                              | Own UPROPERTYs of a class/struct                       |
| `propsall`  | `propsall <name>`                           | `props` + walk Super classes                           |
| `propoff`   | `propoff <class> <prop>`                    | Single property offset (Super walk)                    |
| `listfuncs` | `listfuncs <name>`                          | UFunction children of a class                          |
| `funcoff`   | `funcoff <class>:<func>`                    | UFunction address by class:func                        |
| `propget`   | `propget <obj>:<cls>:<field>:[bytes]`       | Typed read through the FField chain                    |
| `propset`   | `propset <obj>:<cls>:<field>:<hex>`         | Typed write through the FField chain                   |
| `callfn`    | `callfn <obj>:<cls>:<fn>[:<hex>]`           | Invoke a UFunction via `ProcessEvent`                  |

### `Memory` — raw I/O

| Command   | Usage                          | What it does                                              |
|-----------|--------------------------------|-----------------------------------------------------------|
| `read8`   | `read8 <addr>`                 | 1-byte read                                               |
| `read32`  | `read32 <addr>`                | 4-byte read (u32 / float)                                 |
| `read64`  | `read64 <addr>`                | 8-byte read (ptr / u64)                                   |
| `dump`    | `dump <addr> <size>`           | Hex dump, size ≤ `0xC000`. **Label-overlay aware.**       |
| `scan`    | `scan <addr> <range>`          | Detect TArray `{ptr,num,max}` triplets in a range         |
| `pattern` | `pattern <addr>:<range>:<hex>` | Find byte sequences in a range                            |
| `vtable`  | `vtable <addr>:<idx>`          | Read `vtable[idx]` of a UObject                           |
| `module`  | `module [name]`                | Module base + size (empty = main exe)                     |

#### Typed readers (composites)

| Command       | Usage                          | Returns                                |
|---------------|--------------------------------|----------------------------------------|
| `readf32`     | `readf32 <addr>`               | `float`                                |
| `readf64`     | `readf64 <addr>`               | `double` + raw u64                     |
| `readi32`     | `readi32 <addr>`               | signed 32-bit int                      |
| `readi64`     | `readi64 <addr>`               | signed 64-bit int                      |
| `readstr`     | `readstr <addr> <maxLen>`      | NUL-terminated ASCII string            |
| `readunicode` | `readunicode <addr> <chars>`   | NUL-terminated UTF-16LE string         |
| `readguid`    | `readguid <addr>`              | `FGuid` formatted `XXXXXXXX-XXXX-…`    |
| `readptrarr`  | `readptrarr <addr> <n>`        | `n` consecutive 8-byte pointers        |

### `Write`

| Command  | Usage                  | What it does       |
|----------|------------------------|--------------------|
| `write8` | `write8 <addr>:<hex>`  | Write 1 byte       |
| `write32`| `write32 <addr>:<hex>` | Write 4 bytes      |
| `write64`| `write64 <addr>:<hex>` | Write 8 bytes      |
| `wbytes` | `wbytes <addr>:<hex>`  | Write raw hex blob |

### `Player` — high-level helpers

| Command     | Usage                  | What it does                                            |
|-------------|------------------------|---------------------------------------------------------|
| `character` | `character`            | Player pawn pointer + InvComp offset                    |
| `playerinv` | `playerinv`            | Walk InvComp, list UInventory candidates                |
| `inv`       | `inv`                  | Player inventories by name + slot counts                |
| `listitems` | `listitems <bagAddr>`  | Slot-by-slot row name + stack count                     |
| `dumpslot`  | `dumpslot <bag>:<idx>` | Detailed `FItemData` dump for one slot                  |

### `Scanner` (x64dbg-tier) — needs DLL ≥ 1.5.0

| Command   | Usage                                            | What it does                                          |
|-----------|--------------------------------------------------|-------------------------------------------------------|
| `memmap`  | `memmap [<start>] [<range>]`                     | `VirtualQueryEx` walker. Empty = full address space   |
| `modules` | `modules`                                        | `EnumProcessModules` list (base + size + path)        |
| `strings` | `strings <addr> <range> [<minLen>]`              | ASCII + UTF-16 scanner over a memory range            |
| `refs`    | `refs <target> <scanStart> <range>`              | Find 8-byte-aligned pointers to `<target>`            |
| `search`  | `search <addr> <range> <type> <value>`           | Typed search (`u32` `u64` `f32` `f64` `ascii` `utf16`)|

### `Cheats` — same set the desktop UI exposes

| Command       | Effect                                       |
|---------------|----------------------------------------------|
| `godmode`     | God Mode                                     |
| `stamina`     | Infinite Stamina                             |
| `armor`       | Infinite Armor                               |
| `oxygen`      | Infinite Oxygen                              |
| `food`        | Infinite Food                                |
| `water`       | Infinite Water                               |
| `craft`       | Free Craft (handles 0/N recipes)             |
| `items`       | Infinite Items + max durability lock         |
| `weight`      | No Weight (AddModifierState hook)            |
| `speed`       | Speed Hack on/off                            |
| `speed_mult`  | Speed multiplier (float)                     |
| `time`        | Lock Time of Day                             |
| `time_val`    | Locked time value (0–24)                     |
| `temp`        | Stable Temperature                           |
| `temp_val`    | Stable temperature target (°C)               |
| `megaexp`     | ×100 XP                                      |
| `talent`      | Max talent points                            |
| `tech`        | Max tech points                              |
| `solo`        | Max solo points                              |
| `give`        | `give <RowName>,<count>`                     |

---

## Composites &amp; workflows

In addition to primitives, `inspect.py` ships **client-side composites**
that orchestrate several primitives into one call.

### Labels &amp; bookmarks (persisted to `~/.zeusmod_labels.json`)

```text
label    0x1A124C91590 Character
labels                              # list all labels
unlabel  0x1A124C91590

bookmark bp 0x1A1B84C4300 Primary player bag
bookmarks                           # list all bookmarks
unbookmark bp
```

Labels are **overlaid on `dump` output** automatically:

```text
+0x000  20 4F E6 44 F6 7F 00 00  ...   ; Backpack
```

### Snapshots (in-process, per-batch / per-REPL)

```text
snapshot bp1 0x1A1B84C4300 0x100
# … later in the same session …
snapshot bp2 0x1A1B84C4300 0x100
diff bp1                            # byte-level delta vs snapshot
snapshots                           # list all snapshots
```

### Struct viewer

```text
struct Inventory $bp
# →
#   +0x00C8  OnInventoryItemChanged   0x000001A1CBB26A00
#   +0x00E8  CurrentWeight            0x000000000000D78A
#   +0x00F0  Slots                    0x0000000000000000
#   +0x0338  SpoilTickRate            0x3F9D19173FC00000  f32≈1.5
```

### Disassembly (requires `pip install capstone`)

```text
disasm 0x7FF640330000 8
```

### Common workflows

```text
# 1. Find the Inventory class + dump its fields
findcls Inventory = invcls
propsall Inventory

# 2. Locate the live player inventory and inspect it
character = char
playerinv
listitems $bp

# 3. Probe the weight property end-to-end
propoff Inventory CurrentWeight       # → 0xE8
readf32 $bp + 0xE8                    # current weight, in kg

# 4. x64dbg-style memory hunting
modules                               # find Icarus base
strings 0x7FF640330000 0x4000 8       # PE header strings
refs $char $char 0x1000               # who points to the player pawn?
search $bp 0x1000 u64 0xD78A          # find a specific weight value
```

---

## Tips

- **Stdlib shadowing** — the script is named `inspect.py`, but Python's
  stdlib also ships `inspect`. The first lines of the script strip its
  own directory from `sys.path` so the stdlib version still wins. Don't
  rename the file unless you also delete that guard.
- **No connection?** — the pipe only opens once `IcarusInternal.dll` is
  injected. Launch Icarus, click **Attach** in ZeusMod, then re-run.
- **Phase B commands return `ERR unknown primitive`?** — the running
  DLL is older than 1.5.0. Close Icarus, relaunch, re-attach.
- **JSON mode** — every response is wrapped as
  `{"cmd": "...", "args": [...], "raw": "..."}`. Use it with `jq`.
- **Watch mode** — pair with `playerinv` or `listitems $bp` to see live
  inventory mutations highlighted with `ndiff`.
- **History &amp; completion** — Up/Down browse `~/.zeusmod_history`,
  Tab completes against the full command catalog.

---

For the wire-format spec and the matching DLL handlers, see
[`docs/DEV_KIT.md`](DEV_KIT.md) and `native/internal/src/cheats/Trainer.cpp`
(`HandleDbgCommand`, `HandleDbgRaw`).
