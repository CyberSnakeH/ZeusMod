# ZeusMod Developer Kit

Everything you need to iterate on ZeusMod without restarting Icarus every
time: the Python inspector, the DLL's debug pipe, and the validated UE4
offsets discovered so far.

Updated 2026-04-21.

---

## 1. Architecture overview

```
┌───────────────────────┐
│ Icarus-Win64-Shipping │
│  + IcarusInternal.dll │◄──── pipe \\.\pipe\ZeusModPipe ◄──── scripts/inspect.py
│                       │              (message-mode, single-instance)      (Python)
│  - Trainer (cheats)   │
│  - Hooks (MinHook)    │◄──── ImGui/DX11 overlay
│  - UObjectLookup      │         toggles: GodMode, FreeCraft,
│  - Pipe server thread │         InfiniteItems, StackBooster, …
└───────────────────────┘
       ▲
       │ injected by native/injector/IcarusInjector.exe
       │ (also controlled by app/ Electron launcher)
```

- **DLL:** `native/internal/` — everything that runs inside Icarus.
  Compiles to `IcarusInternal.dll`. Post-build copies to
  `native/injector/bin/Release/` so the injector always ships the latest
  build.
- **Injector:** `native/injector/` — classic LoadLibrary injector.
- **Launcher:** `app/` — Electron UI that talks to the DLL over the same
  pipe and wraps the injection.
- **Inspector:** `scripts/inspect.py` — live memory explorer; the primary
  autonomous-iteration tool described in this doc.

---

## 2. The debug pipe

The DLL opens `\\.\pipe\ZeusModPipe` as `PIPE_TYPE_MESSAGE |
PIPE_READMODE_MESSAGE`, single-instance, 64 KB buffers. A client connects,
sends one request, reads one response, closes.

### Wire format

```
<command>:<value>            — cheat toggle / value (existing commands)
dbg:<subcmd>[:<args...>]     — debug / introspection (new, read-only)
```

Responses always start with `OK ` or `ERR `.

### Reserved command prefixes

| Prefix | Owner              | Example                           |
|--------|--------------------|-----------------------------------|
| `dbg:` | inspection/reflection | `dbg:props:Inventory`          |
| *(bare)* | cheats / tuning    | `godmode:1`, `speed_mult:4.0`    |

The single-instance pipe means clients must race-retry on
`ERROR_FILE_NOT_FOUND (2)` and `ERROR_PIPE_BUSY (231)`. The Python client
does this transparently.

---

## 3. DLL debug primitives (`dbg:<cmd>`)

Implemented in `native/internal/src/cheats/Trainer.cpp`, split between
`HandleDbgCommand` (uses `std::string` freely) and `HandleDbgRaw`
(SEH-only: must not use C++ objects because `__try` can't coexist with
C++ unwind — error C2712).

### Name / class lookup

| Command | Args | Description |
|---------|------|-------------|
| `classof`    | `<addr>`              | UClass name of the UObject at `addr` |
| `nameof`     | `<addr>`              | FName of the UObject at `addr` (reads FName at `addr+0x18`) |
| `findcls`    | `<name>`              | Find a UClass by exact name → `OK 0x…` |
| `findstruct` | `<name>`              | Find a UClass **or** UScriptStruct by name |
| `findobj`    | `<className>`         | First non-CDO live instance of a class |
| `listobj`    | `<className>:<maxN>`  | List up to N live instances |

### Raw memory

| Command | Args | Description |
|---------|------|-------------|
| `read8`  | `<addr>`              | 1-byte read (SEH-guarded) |
| `read32` | `<addr>`              | 4-byte read as u32 (`OK 0xHH (dec)`) |
| `read64` | `<addr>`              | 8-byte read as u64 / pointer |
| `dump`   | `<addr>:<size>`       | Hex dump, size ≤ 0xC000 |
| `scan`   | `<addr>:<range>`      | Find `{ptr, num, max}` patterns in `range` bytes |

### UE reflection (ChildProperties / UStruct walker)

| Command    | Args                      | Description |
|------------|---------------------------|-------------|
| `propoff`  | `<class>:<prop>`          | Offset of a named property (walks Super chain, 16 hops max) |
| `props`    | `<className>`             | List all **own** properties: `+0xNNN  Name  Type` |
| `propsall` | `<className>`             | Same but walks Super chain |

`props`/`propsall` accept UClass **or** UScriptStruct names (they share
`UStruct` layout). Example:

```
dbg:props:Inventory
OK props Inventory (cls=0x150A9B62380)
  +0x0C8  OnInventoryItemChanged                    MulticastInlineDelegateProperty
  +0x0E8  CurrentWeight                             FloatProperty
  +0x0F0  Slots                                     StructProperty
  +0x1F8  Items                                     (inside Slots struct — not shown here, see propswalk)
  …
```

### Session helpers

| Command    | Args | Description |
|------------|------|-------------|
| `character` | —   | Player pawn ptr + `Off::Player_InventoryComp` |
| `playerinv` | —   | Walk the InventoryComponent, list child UInventory* candidates |

---

## 4. Python inspector (`scripts/inspect.py`)

Three modes:

```bash
# REPL
python scripts/inspect.py

# One-shot
python scripts/inspect.py "props Inventory"
python scripts/inspect.py "read64 \$char+0x758"

# Batch (semicolon-separated, variables persist)
python scripts/inspect.py -c "character = c; read64 \$c+0x758 = comp; dump \$comp 0x100"
```

### Expression evaluation

Every hex argument goes through `VarStore.resolve()` which understands:

- bare hex (`132515DAAC0`) — auto-prefixed to `0x132515DAAC0`
- explicit hex (`0xDEADBEEF`)
- decimal (`4096`)
- variables (`$char`, `$comp`, `$prev`)
- arithmetic (`$char + 0x758`, `$bag - 0x10`)

Save a command's primary hex result with `= name`:

```
character = char
read64 $char+0x758 = comp
scan $comp+0xE8 0x30
```

### Primitives (pass-through to DLL)

Same name as the DLL command. The inspector handles hex-encoding,
variable substitution, and response parsing.

```python
DBG_PRIMITIVES = {
    "findcls", "findstruct", "findobj", "listobj", "classof", "nameof",
    "read8", "read32", "read64", "dump", "scan", "character", "playerinv",
    "props", "propsall", "propoff",
}
```

Cheat toggles are available too:

```python
CHEAT_COMMANDS = {
    "godmode", "stamina", "armor", "oxygen", "food", "water",
    "craft", "items", "stacks", "weight", "speed", "speed_mult",
    "time", "time_val", "temp", "temp_val", "megaexp",
    "talent", "tech", "solo", "give",
}
```

### Composites (Python-side orchestration)

Located in `scripts/inspect.py`, registered in `COMPOSITES`.

| Composite      | Purpose |
|----------------|---------|
| `follow <addr> [+offN...]` | Chase a pointer chain, report final class |
| `obj <addr>`               | Class, name, and 5-level outer chain |
| `finduobj <className>`     | `findobj` + `obj` one-liner |
| `invtest <addr>`           | Full UInventory candidate analysis: class, outer chain, all nested TArrays in 0x30..0x300, first slot dump |
| `findplayerinv`            | Walk character → InvComp → list UInventory candidates, flag the one whose outer reaches the character as `[OWNS PLAYER]` |
| `propswalk <addr>`         | **Raw** ChildProperties walker (works even when the current DLL doesn't have `props`). Walks `UStruct.ChildProperties` (+0x50 → FField chain, +0x20 Next, +0x28 NamePrivate, +0x4C Offset_Internal) |

### Adding a new composite

```python
def composite_foo(args: list[str], vars_: VarStore) -> str:
    if not args:
        return "ERR usage: foo <addr>"
    addr = vars_.resolve(args[0])
    # Call primitives via primitive_send(), read results via extract_hex()
    r = primitive_send("read64", [f"{addr + 0x10:X}"], vars_)
    cls = extract_hex(r)
    vars_.prev = addr    # so the result can be chained
    return f"OK foo 0x{addr:X} cls=0x{cls:X}"

COMPOSITES["foo"] = composite_foo
```

Composites should stay **composable**: they call `primitive_send()` just
like a user would, so every operation is still logged in the pipe and
can be reproduced manually.

---

## 5. Validated UE 4.27 offsets (Icarus build)

All offsets are resolved dynamically via reflection in the DLL, but the
values below are stable for this game build and have been observed in
live memory (slots 0..29 of the real Backpack, 30/56 items in use).

### Global layout constants

```
UObject:
  +0x00   vtable
  +0x08   ObjectFlags | ObjectIndex
  +0x10   ClassPrivate (UClass*)
  +0x18   NamePrivate (FName)   ← what ReadFNameAt uses
  +0x20   OuterPrivate (UObject*)

UStruct (base of UClass and UScriptStruct):
  +0x40   Super (UStruct*)
  +0x48   Children (UField*)           — functions chain
  +0x50   ChildProperties (FField*)    — properties chain

FField (base of FProperty):
  +0x00   ClassPrivate (FFieldClass*)  — first field = FName at +0x00
  +0x20   Next (FField*)
  +0x28   NamePrivate (FName)

FProperty:
  +0x4C   Offset_Internal (int32)      ← the byte offset we want

UFunction (at vtable[68] = ProcessEvent in Icarus):
  +0xB0   FunctionFlags
  +0xB4   NumParms
  +0xB6   ParmsSize
  +0xB8   ReturnValueOff
  +0xD8   Native Func (C++ thunk)
```

### Character and inventory chain (reflection-backed)

```
BP_IcarusPlayerCharacterSurvival_C
  +0x758   InventoryComponent*                  (Off::Player_InventoryComp)

UInventoryComponent
  +0x0E8   Inventories : TArray<Entry>          (Off::InvComp_Inventories)
             Entry = 0x20 bytes = {
               +0x00  vtable            (constant for this build)
               +0x08  FName name        ← "Quickbar" / "Backpack" / "Equipment" / "Suit" / "Upgrade"
               +0x10  UInventory* bag   ← the actual container
               +0x18  weakObjectPtr
             }
  +0x138   ManuallyAddedItems

UInventory  (class "Inventory")
  +0x0E8   CurrentWeight : float
  +0x0F0   Slots : FInventorySlotsFastArray     (0x160 bytes)
  +0x1F8   ↳ Items : TArray<FInventorySlot>     (Off::FastArray_Slots)
  +0x250   OverflowSpawnTransform
  +0x288   InitialItems
  +0x2A8   CurrentlyEquippedModifiers
  +0x308   ReplicatedModifierStackMultipliers
  +0x318   InventoryInfoRowHandle
  +0x3A0   ParentInventory

FInventorySlot  (stride 0x240)
  +0x010   ItemData : FItemData                 (Off::Slot_ItemData)
  +0x200   Query
  +0x218   Locked
  +0x21C   LastItem
  +0x234   Slotable
  +0x238   Index (int32)                        ← set to slot index when appending

FItemData  (total 0x1F0)
  +0x018   ItemStaticData : FItemTemplateRowHandle     (Off::Item_StaticData)
             +0x00  DataTablePtr (FWeakObjectPtr, 8 bytes)
             +0x08  RowName (FName)
             +0x10  DataTableName (FName)
  +0x030   ItemDynamicData : TArray<FItemDynamicData>  (Off::Item_DynamicData)
             FItemDynamicData = 8 bytes = {
               +0x00 uint8 type        ← e.g. 7 = ItemableStack
               +0x04 int32 value       ← stack count when type == ItemableStack
             }
  +0x040   ItemCustomStats
  +0x050   CustomProperties
  +0x0A0   CachedStats
  +0x1B0   bIsItemInstance
  +0x1B8   DatabaseGUID                (Off::Item_DatabaseGUID)
  +0x1C8   ItemOwnerLookupId
  +0x1D0   RuntimeTags
```

These live in `native/internal/src/cheats/Trainer.h` inside
`namespace Off` so every module shares them.

### How stacks actually work

A stack of N items is **one** `FInventorySlot` containing **one**
`FItemData` whose `ItemDynamicData` TArray has an entry with
`type == 7 (ItemableStack)` and `value == N`.

So the Stack Booster walks every slot and sets `value = 9999` on each
`ItemableStack` dynamic property — it does not duplicate meta items.

### How item lookup works

All items live in a global `IcarusDataTable` singleton resolved on init:

- `g_dItemTemplate` → pointer to `D_ItemTemplate` (3054 rows)
- `BuildItemLibrary_Internal()` iterates rows via `IntToStruct` and
  caches their `FName` row names for the UI

When placing an item we:

1. Call `MakeItemTemplate(rowName)` via `CallUFunction` to build an
   `FItemTemplateRowHandle` with a correct `DataTablePtr`.
2. Call `CreateItem` with that handle to build an `FItemData` template.
3. Copy the resulting 0x1F0 bytes into `Slots[foundSlot].ItemData`
   (direct memory write — the server-authoritative
   `ManuallyForcePlaceItem` rejects client-side placements).
4. Increment `Slots.num` if we appended past the current tail.
5. Optionally invoke `MarkSlotIndexDirty(foundSlot)` to nudge
   replication/UI.

---

## 6. Typical iteration workflows

### A. "I don't know where X is in memory"

```
python scripts/inspect.py
zm> props ClassName                    # list reflected properties
zm> propoff ClassName FieldName        # one-off offset
zm> findstruct MyStruct = st
zm> propswalk $st                      # fallback when DLL lacks struct fallback
```

### B. "Find the live instance I care about"

```
zm> finduobj SomeClass                 # first live non-CDO
zm> listobj SomeClass 20               # list 20 instances
zm> invtest 0x…                        # heavy analysis on a candidate
```

### C. "Follow a pointer chain"

```
zm> character = c
zm> read64 $c+0x758 = comp
zm> read64 $comp+0xE8 = invArr         # TArray data ptr
zm> dump $invArr 0xC0                  # all 6 inventory entries
zm> findplayerinv                      # composite: does all of the above
```

### D. "Prove a layout hypothesis"

```
zm> nameof 0x153DF930020+0x18          # resolve RowName of slot 0
OK Stone
zm> nameof 0x153DF930020+0x240+0x18    # slot 1
OK Sulfur
zm> nameof 0x153DF930020+29*0x240+0x18 # slot 29
OK Refined_Metal
```

The `nameof X` trick reads an FName at `X+0x18` (it's really designed
for UObjects but works for any FName-shaped memory: pass
`FNameAddr - 0x18`).

---

## 7. Adding a new DLL `dbg:` command

1. Open `native/internal/src/cheats/Trainer.cpp`.
2. Decide: does your command need `std::string` / `std::vector`?
   - **Yes** → add it in `HandleDbgCommand`, before the
     `return HandleDbgRaw(...)` line.
   - **No** (raw memory only) → add it in `HandleDbgRaw` (SEH-only, no
     C++ objects allowed in the same function).
3. Keep the response format `OK ...` or `ERR ...`. The Python client
   parses the first hex token as `$prev`.
4. If it takes typed UE data, use `UObjectLookup::*` — the FName pool
   and GObjects resolver is already set up.
5. Update the error fallthrough at the bottom of `HandleDbgRaw`:

   ```cpp
   return _snprintf_s(out, outCap, _TRUNCATE,
       "ERR unknown cmd '%s'. Available: … <your-cmd>", cmd);
   ```

6. Add the name to `DBG_PRIMITIVES` in `scripts/inspect.py` so the
   client routes the command through the `dbg:` prefix automatically.

### SEH + C++ objects — the C2712 trap

Windows structured exceptions (`__try/__except`) can't coexist with C++
unwind in the same function. If you need both, split the function:

```cpp
static bool SafeRawRead(void* p, int* out) {
    __try { *out = *reinterpret_cast<int*>(p); return true; }
    __except (1) { return false; }
}

int OuterFunction() {
    std::string s;         // unwindable — __try would fail here
    int v = 0;
    if (!SafeRawRead(somePtr, &v)) return 0;
    s = std::to_string(v);
    // …
}
```

This pattern is used throughout `ResolvePlayerInventoryByName` and
`HandleDbgCommand` / `HandleDbgRaw`.

---

## 8. Adding a new cheat toggle

1. Field in `Trainer.h`: `bool MyCheat = false;`
2. Pipe dispatch in `PipeServerThread` (`Trainer.cpp`):
   ```cpp
   else if (strcmp(cmd, "mycheat") == 0) self->MyCheat = (v != 0);
   ```
3. Tick logic in `Trainer::Tick` (guard everything with `__try` / bounds
   checks — this runs on a worker thread every frame).
4. Electron UI toggle in `app/src/renderer/index.html` + handler.
5. ImGui toggle in `native/internal/src/hooks/Render.cpp` (inside the
   cheat panel draw).
6. Legacy overlay toggle (optional) in `native/internal/src/ui/Overlay.cpp`.

Use `ResolvePlayerBackpack()` / `ResolvePlayerQuickbar()` rather than
`Player_InventoryComp` directly — the former returns a real UInventory
whose `FastArray_Slots` offset is valid.

---

## 9. Build + deploy

```bash
# From repo root, with VS Build Tools installed:
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" \
    native/internal/IcarusInternal.vcxproj \
    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m /v:m
```

Output:

- `native/internal/bin/Release/IcarusInternal.dll` — canonical
- `native/injector/bin/Release/IcarusInternal.dll` — copied by
  post-build step; locked while Icarus is running (expect a post-build
  copy failure, that's OK — the DLL itself still compiled)

If the copy step fails because Icarus is running, close the game and
either re-run MSBuild or `cp` the DLL manually:

```bash
cp native/internal/bin/Release/IcarusInternal.dll \
   native/injector/bin/Release/IcarusInternal.dll
```

---

## 10. Known landmines

- **FWeakObjectPtr** — 8 bytes, `{int32 ObjectIndex, int32
  ObjectSerialNumber}`. Both values change per session; never hardcode.
- **FName** — 8 bytes, `{int32 ComparisonIndex, int32 Number}`. The
  comparison index is a pool offset; resolve via `ReadFNameAt` or the
  `nameof` trick.
- **UInventoryComponent is not a UInventory.** It holds a TArray of
  named UInventory children (Quickbar, Backpack, …). Direct-writing
  slot data into the component's memory works coincidentally because
  `Off::FastArray_Slots = 0x1F8` happens to fall in a reasonable spot,
  but it doesn't replicate and silently fails validation. Always
  resolve the right child first.
- **`ManuallyForcePlaceItem` is server-authoritative.** Client-side
  calls return `false` with no other signal. Use direct memory write
  into the Slots TArray instead, then `MarkSlotIndexDirty` to signal
  replication.
- **Single-instance pipe.** `scripts/inspect.py` retries on err 2 / 231
  for 400 ms — don't remove the retry loop, composites fire hundreds
  of requests in a row and will hit the connect/recreate window.
- **Post-build copy failure ≠ build failure.** MSBuild reports
  `error MSB3073` when Icarus holds the injected DLL. The `.dll` in
  `native/internal/bin/Release/` is still fresh.
- **FProperty size/dim offsets** beyond `Offset_Internal` have not been
  validated for this build. Don't trust `props` output for those
  fields — it's elided in the current code.
- **Slots.num vs Slots.max.** `num` = currently-used; `max` =
  pre-allocated capacity. A backpack with `num=30 max=56` has 26 free
  slots — iterate to `max` when looking for insertion space, bump
  `num` when you append past the current tail.

---

## 11. Quick reference card

```
# Character & inventory
python scripts/inspect.py character
python scripts/inspect.py findplayerinv

# Structure introspection
python scripts/inspect.py "props Inventory"
python scripts/inspect.py "propoff Inventory Slots"
python scripts/inspect.py "findstruct ItemData"
python scripts/inspect.py "propswalk 0x…"

# Memory probing
python scripts/inspect.py "read64 \$char+0x758"
python scripts/inspect.py "dump \$comp 0x100"
python scripts/inspect.py "scan \$bag 0x400"

# Combined discovery session
python scripts/inspect.py -c \
  "character = c; \
   read64 \$c+0x758 = comp; \
   read64 \$comp+0xE8 = invArr; \
   read64 \$invArr+0x10 = bag; \
   classof \$bag; \
   scan \$bag+0xF0 0x160"

# Toggle a cheat directly over pipe (smoke test after a rebuild)
python scripts/inspect.py "stacks 1"
python scripts/inspect.py "items 1"
python scripts/inspect.py "give wood,10"
```
