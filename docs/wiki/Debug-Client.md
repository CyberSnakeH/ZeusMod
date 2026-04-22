# Debug Client (`scripts/inspect.py`)

`inspect.py` is ZeusMod's x64dbg-style memory explorer and reflection
inspector for the live Icarus process. This wiki page is the extended
guide — tutorials, workflows, and command philosophy. For a terse
alphabetical reference, see [`docs/INSPECT.md`](../INSPECT.md).

---

## What it does

- Walks Unreal reflection (`UClass`, `UScriptStruct`, `UFunction`,
  `UPROPERTY`) on the live process.
- Reads and writes memory through typed primitives (`u32`, `u64`,
  `f32`, `f64`, ASCII / UTF-16 strings, `FGuid`, pointer arrays).
- Enumerates loaded modules, walks `VirtualQueryEx`, scans ranges for
  strings or typed values, finds pointer back-references.
- Decodes raw memory through reflected `UPROPERTY` offsets
  (`struct <UClass> <base>`).
- Drives every cheat the desktop UI exposes, from a terminal.
- Persists labels and bookmarks across sessions.
- Captures per-region snapshots and byte-diffs them.

---

## Three ways to use it

### 1. One-shot

```powershell
python scripts/inspect.py -c "character"
```

Runs a single command (or a `;`-separated batch), prints the result,
exits. Great for scripts, CI smoke tests, and piped commands.

### 2. REPL (the one most people live in)

```powershell
python scripts/inspect.py
```

You get:

- `readline`-backed history stored at `~/.zeusmod_history`.
- Tab completion against the full command catalog.
- `help` for a grouped overview; `help <cmd>` for per-command detail.
- `;` separates commands inside a single line.
- `= name` at the end of any command saves the extracted hex into `$name`.

### 3. Watch mode

```powershell
python scripts/inspect.py --watch "playerinv" --interval 1
```

Re-runs the command every `interval` seconds and colour-highlights
the lines that changed since the last run (via `difflib.ndiff`).
Perfect for spotting inventory mutations, HP ticks, or "what writes
this field".

---

## Variables and expressions

```text
character = char          # saves into $char
readf32 $char + 0x758     # arithmetic supported
read64 $char + 0x758 = comp
propoff Inventory CurrentWeight
```

Every command that returns a primary hex value in an `OK ...` line
sets `$prev`. The `= <name>` suffix additionally saves under a custom
name.

The VarStore resolver understands:

| Token             | Meaning                                               |
|-------------------|-------------------------------------------------------|
| `0x...`           | Literal hex                                           |
| `<bare hex>`      | Literal hex (e.g. `1A124C91590`)                      |
| `$prev`           | Last parsed hex value                                 |
| `$<name>`         | Saved variable                                        |
| `+`, `-`          | Hex arithmetic                                        |

---

## Typical workflows

### Finding the player inventory from scratch

```text
character = char           # $char = player pawn
playerinv                  # list UInventory candidates hanging off the pawn
# pick the one whose class you recognise, e.g. obj=0x28F...
# save it as $bp
```

### Probing a UPROPERTY by name

```text
propoff Inventory CurrentWeight      # → 0xE8
readf32 $bp + 0xE8                   # current weight in kg
```

### Dumping a struct through reflection

```text
struct Inventory $bp
# Displays every UPROPERTY with its offset, value (f32/u64 hints),
# and pointer / string tags. This is the fastest way to eyeball a
# struct without cross-referencing SDK dumps.
```

### Tracking a value while a cheat is active

```text
# Terminal 1
python scripts/inspect.py -c "weight 1"

# Terminal 2
python scripts/inspect.py --watch "readf32 0x1A1B84C43E8" --interval 0.5
# Watch the encumbrance value while you run around in-game.
```

### Memory hunting, x64dbg-style

```text
modules                                  # where is Icarus-Win64-Shipping.exe?
# OK modules count=174
#   0x00007FF640330000  size=0x06A62000  Icarus-Win64-Shipping.exe
#   ...

strings 0x7FF640330000 0x4000 8          # strings in the PE header region
# OK strings @0x7FF640330000 range=0x4000 minLen=8
#   0x7FF64033004D  ascii(40)  "!This program cannot be run in DOS mode."

refs $char $char 0x1000                  # what points to the player pawn?
# OK refs target=0x28F2A3AC010 start=0x28F2A3AC010 range=0x1000
#   0x28F2A3AC060  ->  0x28F2A3AC010  (+0x0)
#   0x28F2A3AC128  ->  0x28F2A3AC010  (+0x0)

search $bp 0x1000 u64 0xD78A             # find a specific weight value
```

### Labels and bookmarks (persistent)

```text
label    0x1A124C91590 Character
label    0x1A1B84C4300 Backpack
labels                                 # list every label
unlabel  0x1A124C91590

bookmark bp 0x1A1B84C4300 "Primary player bag"
bookmarks                              # list every bookmark
unbookmark bp
```

Labels are **overlaid on `dump` output automatically**:

```text
+0x000  20 4F E6 44 F6 7F 00 00  ...   ; Backpack
```

They live in `~/.zeusmod_labels.json`.

### Snapshots + diff (in-process)

```text
snapshot bp1 0x1A1B84C4300 0x100
# … play a little in-game …
snapshot bp2 0x1A1B84C4300 0x100
diff bp1
```

Snapshots are in-process only — they persist across commands inside
a single `inspect.py` invocation but not between separate launches.

### Disassembly (needs `pip install capstone`)

```text
disasm 0x7FF640330000 8
```

---

## JSON and scripting

```powershell
python scripts/inspect.py --json -c "character; modules" | jq '.[] | .raw'
```

Every response is wrapped as `{"cmd": "...", "args": [...], "raw": "..."}`.
Use it with `jq` / PowerShell `ConvertFrom-Json` / any pipeline tool.

`--timing` prepends `(nn.nn ms)` to each response for quick profiling.

---

## Debug-client feature table

| Category   | Commands                                                              |
|------------|-----------------------------------------------------------------------|
| Lookup     | `classof`, `nameof`, `findcls`, `findstruct`, `findobj`, `findname`, `listobj`, `getbyindex`, `outer`, `isa`, `fname`, `fnameof` |
| Reflection | `props`, `propsall`, `propoff`, `listfuncs`, `funcoff`, `propget`, `propset`, `callfn` |
| Memory     | `read8`, `read32`, `read64`, `dump`, `scan`, `pattern`, `vtable`, `module` |
| Write      | `write8`, `write32`, `write64`, `wbytes`                              |
| Player     | `character`, `playerinv`, `inv`, `listitems`, `dumpslot`              |
| Scanner    | `memmap`, `modules`, `strings`, `refs`, `search`                      |
| Cheats     | `godmode`, `stamina`, `armor`, `oxygen`, `food`, `water`, `craft`, `items`, `weight`, `speed`, `speed_mult`, `time`, `time_val`, `temp`, `temp_val`, `megaexp`, `talent`, `tech`, `solo`, `give` |
| Typed      | `readf32`, `readf64`, `readi32`, `readi64`, `readstr`, `readunicode`, `readguid`, `readptrarr` |
| Annotate   | `label`, `labels`, `unlabel`, `bookmark`, `bookmarks`, `unbookmark`   |
| Snapshot   | `snapshot`, `snapshots`, `diff`                                       |
| Composite  | `struct`, `disasm` (optional)                                         |

---

## Design principle: no hidden state

Every result you see on screen is derivable from the wire-level
response the DLL sent back. Composite commands (like `struct` or
`dump` with labels) orchestrate several primitives and apply
formatting client-side — but the primitives themselves are pure.
That makes it easy to build new composites without changing the DLL,
and to fall back to raw primitives when something looks off.

If a composite surprises you, set `--timing` and re-run — you'll see
the sequence of primitives it fired, with per-call latency, and
the layer of abstraction collapses back to a series of wire calls.

---

## See also

- [`docs/INSPECT.md`](../INSPECT.md) — flat alphabetical command reference.
- [Pipe Protocol](Pipe-Protocol.md) — wire-format spec.
- [Reflection Internals](Reflection-Internals.md) — how the Lookup and Reflection groups resolve things.
- [Memory Layout](Memory-Layout.md) — known offsets to pair with `readf32 / propoff / struct`.
