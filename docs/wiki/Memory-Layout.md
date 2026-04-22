# Memory Layout

This is a reference of the memory layouts ZeusMod relies on. Anything
here that isn't resolved through UE reflection is a **bake-in** — it
would need re-validation if Icarus moved to UE5 or changed a specific
Blueprint structure.

Everything is x64, little-endian.

---

## UE 4.27 object model (engine-level, non-negotiable)

### `UObject` header

| Offset | Field                    | Notes                                     |
|--------|--------------------------|-------------------------------------------|
| +0x00  | `vtable`                 | Standard C++ vtable pointer               |
| +0x08  | `ObjectFlags` (EObjectFlags) | bitset                                |
| +0x0C  | `InternalIndex`          | Index into `GObjects`                     |
| +0x10  | `ClassPrivate` (`UClass*`) | Points at the class reflection object   |
| +0x18  | `NamePrivate` (`FName`)  | ComparisonIndex + Number                  |
| +0x20  | `OuterPrivate` (`UObject*`) | Outer object chain                     |

### `FUObjectItem`

| Offset | Field                     |
|--------|---------------------------|
| +0x00  | `Object` (`UObject*`)     |
| +0x08  | `Flags` (int32)           |
| +0x0C  | `ClusterRootIndex` (int32)|
| +0x14  | `SerialNumber` (int32)    |

`SerialNumber` at `+0x14` is a ZeusMod-baked assumption. It is used
when we synthesise a `FWeakObjectPtr {ObjectIndex, SerialNumber}` to
inject into the `DeployableTickSubsystem` active-processor TArray.

### `UStruct`

| Offset | Field                    | Notes                                     |
|--------|--------------------------|-------------------------------------------|
| +0x30  | `SuperStruct`            | Parent class/struct                       |
| +0x40  | `Children` (`UField*`)   | Linked list head of properties/functions  |

### `FField` (4.25+ — post-FField-refactor)

| Offset | Field                       |
|--------|-----------------------------|
| +0x00  | `vtable`                    |
| +0x08  | `Class` (`FFieldClass*`)    |
| +0x18  | `Name` (`FName`)            |
| +0x20  | `Next` (`FField*`)          |

### `FProperty` (extends `FField`)

| Offset | Field                 |
|--------|-----------------------|
| +0x44  | `Offset_Internal`     |

This is the field `FindPropertyOffset` reads after matching the name.

### `UFunction` (extends `UStruct`)

The field we care about for hooking:

| Offset | Field                    | Notes                                     |
|--------|--------------------------|-------------------------------------------|
| +0xB0  | `Func` (thunk ptr)       | Points at the Kismet-generated thunk. We walk it to the C++ `exec` body via `WalkThunkToImpl`. |

(Exact offsets here are UE 4.27-specific and are read through UE's
own reflection headers at build time via `UE4.h`.)

---

## `FName`

```cpp
struct FName {
    int32 ComparisonIndex;  // +0x00  — key into GNames
    int32 Number;           // +0x04  — de-dup suffix (unused for most names)
};
```

`Number` is ignored by `ResolveFNameByIndex` — collisions are
vanishingly rare in Icarus-relevant FNames.

---

## Game-specific structs we peek into

### `FModifierStateRowHandle` (24 bytes)

Used as an argument to `IcarusFunctionLibrary::AddModifierState`.
Our No Weight detour reads the middle 8 bytes to check the row name.

| Offset | Field                       | Notes                                     |
|--------|-----------------------------|-------------------------------------------|
| +0x00  | `DataTable` (`UDataTable*`) | D_ModifierStates                          |
| +0x08  | `RowName` (`FName`)         | The row the modifier refers to            |
| +0x10  | `DataTableName` (`FName`)   | Debug name of the data table              |

The detour reads `+0x08`'s ComparisonIndex and resolves it to a
string; the No Weight kill-switch compares that string to
`"Overburdened"`.

### `UInventory`

| Offset   | UPROPERTY                       | Type           | Notes                              |
|----------|---------------------------------|----------------|------------------------------------|
| +0x00C8  | `OnInventoryItemChanged`        | delegate       | UE multicast delegate header       |
| +0x00E8  | `CurrentWeight`                 | float32        | Cached display weight              |
| +0x00F0  | `Slots` (`TArray<FItemData>`)   | `{ptr, num, max}` | The item grid                   |
| +0x0250  | `OverflowSpawnTransform`        | `FTransform`   | Drop spot for overflow             |
| +0x0338  | `SpoilTickRate`                 | float32        | Spoil cadence                      |

These offsets are resolved at runtime via reflection, but we list
them here because they come up often in `inspect.py` sessions.

### `BP_IcarusPlayerCharacterSurvival_C`

| Offset   | UPROPERTY             | Type           | Notes                              |
|----------|-----------------------|----------------|------------------------------------|
| +0x1010  | `CurrentWeight`       | float32        | Character's encumbrance reading    |

### `UMG_EncumbranceBar_C`

| Offset   | UPROPERTY             | Type           | Notes                              |
|----------|-----------------------|----------------|------------------------------------|
| +0x0304  | `PlayerWeight`        | float32        | Widget-cached encumbrance          |
| +0x0868  | `CurrentEncumbrance`  | float32        | Widget-cached value for the bar fill |
| +0x086D  | `NeedsUpdate`         | bool (1 byte)  | Widget redraw gate                 |

These resolve correctly only because `FindClassByName` was extended
in 1.x to accept `UWidgetBlueprintGeneratedClass` in addition to
`UBlueprintGeneratedClass`.

### `InventoryComponent` — offset to `Inventory` list

| Offset   | Field                                          |
|----------|------------------------------------------------|
| +0x0758  | `InventoryComponent*` on the player pawn       |

`Off::Player_InventoryComp` in the DLL. Reported by `character` in
`inspect.py`:

```
OK character=0x28F2A3AC010 Off::Player_InventoryComp=0x758
```

### `DeployableTickSubsystem +0x60`

The active-processor `TArray<FWeakObjectPtr>` that Free Craft injects
into. It is **unreflected** — not exposed as a UPROPERTY. This
offset is a hard bake-in; a schema change on the subsystem would
require a re-audit.

### `ProcessingItem +0x1C0`

Free Craft's pre-populator writes the queued slot here so the
internal `Process` tick advances progress from frame 1.

---

## Anything else?

If you discover a new offset while working in `inspect.py`:

1. Use `propoff <class> <prop>` if it's a UPROPERTY — that's reflected,
   you don't need to write it down.
2. Use `label` + `bookmark` to annotate the address locally.
3. If it ends up in a hook, add it to this page **and** to the matching
   `Off::` namespace inside `TrainerResolve.cpp`.

Every reflected offset lives inside `UObjectLookup`. Every
non-reflected offset lives here. There is no third pool.

---

## See also

- [Reflection Internals](Reflection-Internals.md) — how the reflected offsets get resolved.
- [Hook Catalog](Hook-Catalog.md) — who reads which offset.
- [Debug Client](Debug-Client.md) — tools to inspect these layouts live.
