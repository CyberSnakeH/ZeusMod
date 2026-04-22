# Feature Reference

For every cheat ZeusMod exposes, this page documents:

- **What it does** (player-visible effect)
- **How it's implemented** (hook vs tick clamp, which UFunction/UPROPERTY it touches)
- **Side effects** (animation, physics, multiplayer implications, any known issues)
- **Toggle name** (the wire command used by both the UI and `inspect.py`)

Cheats are grouped the same way the desktop UI groups them.

---

## Survival

### God Mode — `godmode`

**Effect.** Health stays at max. All damage sources (fall, combat, biome,
hunger/thirst extremes) are neutralised.

**Implementation.** Detour on `SurvivalCharacter::SetHealth`. The hook
short-circuits the incoming health value and keeps the UPROPERTY at the
current `MaxHealth`. A per-tick top-up also re-clamps `Health` against
`MaxHealth` in case another callsite writes directly to the UPROPERTY.

**Side effects.** None in single-player. Multiplayer parity is not
guaranteed — the health packet seen by the server would be inconsistent.

---

### Infinite Stamina — `stamina`

**Effect.** Stamina bar stays at max while sprinting, swinging, climbing.

**Implementation.** Per-tick clamp on
`SurvivalCharacterState::Stamina`. Offset resolved at runtime via
`UObjectLookup::FindPropertyOffset("SurvivalCharacterState", "Stamina")`.

**Side effects.** None observed.

---

### Infinite Armor — `armor`

**Effect.** Every armor slot is topped up to the learned max durability
each tick.

**Implementation.** Walks the armor slot array in the player inventory
and sets `CurrentDurability = MaxDurability` for every slot whose
durability is less than full. Property offsets (`CurrentDurability`,
`MaxDurability`) resolved via reflection.

**Side effects.** Visual durability bars in the UI pulse back to full
on each tick — cosmetic only.

---

### Infinite Oxygen — `oxygen`

**Effect.** Oxygen bar frozen at max. Lets you explore caves, dive, or
stay at altitude indefinitely.

**Implementation.** Per-tick clamp on
`SurvivalCharacterState::Oxygen`.

---

### Infinite Food / Water — `food`, `water`

**Effect.** Hunger and thirst bars pinned at max.

**Implementation.** Per-tick clamp on
`SurvivalCharacterState::Food` and
`SurvivalCharacterState::Water`.

---

### Stable Temperature — `temp`, `temp_val`

**Effect.** Character body temperature is held at a configurable target
(default 20 °C). Hot biomes, cold storms, nights and altitude no longer
push you outside the safe band.

**Implementation.** Per-tick clamp on
`SurvivalCharacterState::ModifiedInternalTemperature` (an `int32` — the
same value the HUD thermometer reads). `temp_val` updates the target;
`temp` toggles the clamp on/off.

**Side effects.** Cold-only effects tied to internal-temperature
thresholds (shivering, frostbite, freezing animations) will not trigger.

---

## Inventory

### Free Craft — `craft`

**Effect.** Lets you queue and complete any recipe — including **0/N**
recipes where you have zero of the required inputs.

**Implementation.** Four cooperating detours:

| Hook target                                               | Purpose                                                                    |
|-----------------------------------------------------------|----------------------------------------------------------------------------|
| `CanQueueItem`                                            | Returns true unconditionally when the recipe's own-inventory belongs to the player. |
| `HasSufficientResource` / `GetResourceRecipeValidity`     | Returns the "have-enough" branch so the UI allows the queue.              |
| `CanSatisfyRecipeQueryInput`                              | Same, for deeper validators used by station crafting.                     |
| Post-queue patch on `DeployableTickSubsystem +0x60`       | When the game's `ValidateQueueItem` would have rejected the queue, we insert a valid `FWeakObjectPtr{ObjectIndex, SerialNumber}` into the active-processor `TArray` so `Process` can still advance. |

A player-owned-inventory filter makes sure only the local player's
`Inventory` reports the unlimited fake count. Deployable inventories
(extractors, processors, benches) keep their real contents so their
outputs are delivered correctly.

**Side effects.** None observed. Crafting XP, unlocks and recipes queue
like they would in a legit session.

---

### Infinite Items — `items`

**Effect.** Consumables (arrows, bullets, torches, bandages, food) stop
decrementing. Tools and armor are clamped to their learned max
durability every tick.

**Implementation.** Detour on `ConsumeItem` (fast-path, returns
without decrementing for the local player's inventory) plus a tick
clamp that writes `CurrentDurability = MaxDurability` on every
non-consumable slot.

---

### No Weight — `weight`

**Effect.** Character never feels heavy. Full sprint, full jump, full
movement animations at any load.

**Implementation — 1.5+.** Detour on
`IcarusFunctionLibrary::AddModifierState`. Signature in the game:

```cpp
bool AddModifierState(
    UObject* ParentObject,
    FModifierStateRowHandle InModifier,  // 24 bytes: DataTablePtr + RowName + DTName
    UObject* Causer,
    UObject* Instigator,
    float Effectiveness);
```

When the incoming `InModifier.RowName` resolves to `"Overburdened"`
**and** `Trainer::NoWeight == true`, the detour returns `false`
**without** calling the real function. The modifier is never applied,
so the character's `MaxWalkSpeed` is never clamped down and the
encumbered animation set is never activated.

**Why this, not `SetCurrentWeight = 0`.** The `Inventory.CurrentWeight`
UPROPERTY is a display value; the server-authoritative "are you over
the cap?" check goes through `AddModifierState`. Earlier versions
tried byte-patching `MaxWalkSpeed` and calling
`ExpireOverburdenedModifier` — both caused an access-violation in the
PhysX tick (`FPhysScene_PhysX::TickPhysScene` reading a stale
pointer). Hooking at the source is both correct and stable.

**Side effects.** The UI still shows your real current weight and its
numeric cap. The cap just never fires.

---

## Character

### Speed Hack — `speed`, `speed_mult`

**Effect.** Multiplies the player's movement speed by a configurable
factor (walk, run, crouch, swim, fly). Default `x2.0`.

**Implementation.** Per-tick clamp on
`SurvivalCharacterMovementComponent::MaxWalkSpeed` etc. — the tick
writes `base_value * Trainer::SpeedMult` on every movement-component
speed UPROPERTY.

**Side effects.** At very high multipliers (`x6+`) the physics tick
can snag you on cliff faces and doorframes. Drop it back to `x4` or
lower for exploration; the multiplier accepts 0.5 step increments
between 0.5 and 10.0.

---

### Mega XP — `megaexp`

**Effect.** ×100 experience gain. The character visibly levels up
through the game's own level-up gates — no instant-max teleport that
breaks unlock animations.

**Implementation.** Per-tick `TotalExperience += 50000`. Resolved via
`UObjectLookup::FindPropertyOffset("PlayerCharacterState",
"TotalExperience")`.

---

### Max Talent / Tech / Solo Points — `talent`, `tech`, `solo`

**Effect.** Fills the corresponding progression-point pools to their
cap. Every talent / tech / solo-play perk becomes available.

**Implementation.** Reflected UPROPERTY writes on the matching
`SurvivalProfile` / `IcarusGameInstance` state containers.

---

## World

### Time Lock — `time`, `time_val`

**Effect.** Freezes the in-game clock at a preset hour (00:00, 06:00,
12:00, 18:00 presets in the UI; any float between 0 and 24 from the
pipe).

**Implementation.** Per-tick write on the WorldSettings time-of-day
UPROPERTY. `time_val` updates the target; `time` toggles the clamp.

**Side effects.** Animal and weather simulations that key off the
time-of-day cycle will run in a single "phase" for as long as the
clamp is on.

---

## Give Items — `give`

**Effect.** Spawns any item from `D_ItemTemplate` directly into the
player's backpack.

**Implementation.** Three-stage call chain invoked via
`ProcessEvent`:

```text
give Wood,10
  → MakeItemTemplate(RowName="Wood")        // builds an FItemTemplate
  → CreateItem(template, count=10)          // builds an FItemData
  → OnServer_AddItem(itemData)              // routes through the controller
```

All three are UFunctions, resolved by reflection and called with a
stack-allocated `Params` struct matching their `ParmsSize`.

**Side effects.** The item appears at the next convenient inventory
slot. If the backpack is full it overflows to the ground, exactly as
it would in a legit session.

---

## Command reference (flat table)

| Command       | UI card                 | Type                |
|---------------|-------------------------|---------------------|
| `godmode`     | God Mode                | toggle              |
| `stamina`     | Infinite Stamina        | toggle              |
| `armor`       | Infinite Armor          | toggle              |
| `oxygen`      | Infinite Oxygen         | toggle              |
| `food`        | Infinite Food           | toggle              |
| `water`       | Infinite Water          | toggle              |
| `temp`        | Stable Temperature      | toggle              |
| `temp_val`    | Stable Temperature      | value (°C)          |
| `craft`       | Free Craft              | toggle              |
| `items`       | Infinite Items          | toggle              |
| `weight`      | No Weight               | toggle              |
| `speed`       | Speed Hack              | toggle              |
| `speed_mult`  | Speed Hack              | value (float)       |
| `time`        | Lock Time               | toggle              |
| `time_val`    | Lock Time               | value (0–24)        |
| `megaexp`     | Mega XP                 | toggle              |
| `talent`      | Max Talent Points       | toggle              |
| `tech`        | Max Tech Points         | toggle              |
| `solo`        | Max Solo Points         | toggle              |
| `give`        | Give Items              | `give <Row>,<qty>`  |

For every command, `<value>` is a plain ASCII token (`0` / `1` for
booleans, a float for multipliers, a signed int for temperature, an
integer for hour/count). The command + value are separated by a
colon on the wire — see [Pipe Protocol](Pipe-Protocol.md).

---

## See also

- [Hook Catalog](Hook-Catalog.md) — every MinHook detour with its target resolution.
- [Reflection Internals](Reflection-Internals.md) — how these UPROPERTY / UFunction lookups work.
- [Debug Client](Debug-Client.md) — drive each cheat from Python.
