# Hook Catalog

This page lists every MinHook detour ZeusMod installs inside
`IcarusInternal.dll`, along with its target resolution path, calling
convention, and the reason it exists. Use it as a map when reading
`Trainer.cpp`, `TrainerFreeCraft.cpp`, and `TrainerGiveItem.cpp`.

---

## How we install a detour

Every hook installation goes through three stages:

1. **Resolve the target address.** For UFunctions, this is
   `UObjectLookup::FindNativeFunction(className, funcName)` — it walks
   the UE reflection graph, finds the `UFunction`, reads its `Func`
   pointer (the Kismet thunk) and then walks the thunk to the real
   C++ `exec` body.
2. **Register the detour with MinHook.**
   ```cpp
   MH_STATUS s = MH_CreateHook(target, &DetourFn, &g_origFn);
   if (s != MH_OK) return;
   MH_EnableHook(target);
   ```
3. **Log the hook** so the in-game console shows the target address
   and the detour symbol, for post-mortem analysis.

A dedicated `InstallXHook()` function lives next to each cheat's
`Trainer::X` bool. It is idempotent — safe to call again if the cheat
is toggled off and back on.

---

## Hook reference

Legend:
- **Cheat** — the UI toggle / wire command that owns the hook.
- **Target** — the function we replace.
- **Resolution** — how we find it at runtime.
- **Installed at** — when the detour goes in.

---

### `SetHealth` → God Mode

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `godmode`                                                           |
| Target signature  | `void SurvivalCharacter::SetHealth(float NewHealth)`                |
| Resolution        | `FindNativeFunction("SurvivalCharacter", "SetHealth")`              |
| Installed at      | First time `godmode:1` is received                                  |
| Detour behaviour  | Ignores the incoming value, keeps the character at `MaxHealth`.     |

---

### `AddModifierState` → No Weight (1.5+)

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `weight`                                                            |
| Target signature  | `bool IcarusFunctionLibrary::AddModifierState(UObject* Parent, FModifierStateRowHandle InModifier, UObject* Causer, UObject* Instigator, float Effectiveness)` |
| Resolution        | `FindNativeFunction("IcarusFunctionLibrary", "AddModifierState")`   |
| Installed at      | First time `weight:1` is received                                   |
| Detour behaviour  | Reads `InModifier +0x08` (RowName FName ComparisonIndex). If the resolved string is `"Overburdened"` and `Trainer::NoWeight == true`, returns `false` without calling the original. Otherwise forwards to `g_origAddModifierState`. |
| Notes             | Replaces the earlier MaxWalkSpeed clamp + ExpireOverburdenedModifier pair, which caused a PhysX-tick AV. Hooking at the source is both correct and stable. |

---

### `CanQueueItem` → Free Craft

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `craft`                                                             |
| Target signature  | `bool UCraftingSystem::CanQueueItem(UInventory* Inventory, FRecipeQuery Query)` |
| Resolution        | `FindNativeFunction("CraftingSystem", "CanQueueItem")`              |
| Installed at      | First time `craft:1` is received                                    |
| Detour behaviour  | If `Inventory` is the local player's own backpack, returns `true` without evaluating the recipe. Deployable inventories (benches, processors) fall through to the original. |

---

### `HasSufficientResource` / `GetResourceRecipeValidity` → Free Craft

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `craft`                                                             |
| Target signature  | `bool UCraftingSystem::HasSufficientResource(...)` / `UCraftingSystem::GetResourceRecipeValidity(...)` |
| Resolution        | By name, same class as above.                                       |
| Installed at      | First time `craft:1` is received                                    |
| Detour behaviour  | Returns the "yes" branch when the inventory is player-owned.        |

---

### `CanSatisfyRecipeQueryInput` → Free Craft

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `craft`                                                             |
| Target signature  | `bool UCraftingSystem::CanSatisfyRecipeQueryInput(...)`             |
| Resolution        | By name.                                                            |
| Installed at      | First time `craft:1` is received                                    |
| Detour behaviour  | Same as above — short-circuits the last remaining validator.         |

---

### `AddProcessingRecipe` → Free Craft (station side)

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `craft`                                                             |
| Target            | `UCraftingSystem::AddProcessingRecipe(...)`                         |
| Resolution        | By name.                                                            |
| Installed at      | First time `craft:1` is received                                    |
| Detour behaviour  | After the game's own `ValidateQueueItem` runs, the detour walks the unreflected `DeployableTickSubsystem +0x60` active-processor `TArray` and injects a valid `FWeakObjectPtr{ObjectIndex, SerialNumber}` referring to the queued recipe, so the `Process` tick can pick it up. Also pre-populates `ProcessingItem +0x1C0` with the queued slot so `Process` advances progress from frame 1. |

---

### `ConsumeItem` → Infinite Items

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | `items`                                                             |
| Target signature  | `void UInventory::ConsumeItem(int32 Slot, int32 Amount)`            |
| Resolution        | `FindNativeFunction("Inventory", "ConsumeItem")`                    |
| Installed at      | First time `items:1` is received                                    |
| Detour behaviour  | For the local player's inventory, returns immediately without decrementing. Deployables fall through. |

---

### `GetItemCount` / `FindItemCountByType` → (not hooked in 1.5)

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | (was part of early Infinite Items before we had the player-owned-inventory filter) |
| Status            | **No longer hooked.** Kept in `UObjectLookup` resolution as reference symbols but not detoured. |

---

### Swapchain `Present` → ImGui overlay

| Field             | Value                                                               |
|-------------------|---------------------------------------------------------------------|
| Cheat             | (infrastructure — always installed)                                 |
| Target            | `IDXGISwapChain::Present` slot in the D3D11 swap-chain vtable       |
| Resolution        | D3D11 device enumeration + vtable read (no UE reflection involved)  |
| Installed at      | Trainer boot                                                        |
| Detour behaviour  | Runs ImGui's `NewFrame` / `EndFrame` / `RenderDrawData` around the real `Present` to draw the in-game menu. Captures input via a WindowProc hook. |

---

## Uninstall order (on <kbd>F10</kbd>)

1. Toggle every cheat off (releases any tick-scoped allocations).
2. `MH_DisableHook(MH_ALL_HOOKS)` — restore the original bytes.
3. `MH_Uninitialize()` — release the MinHook trampoline pool.
4. Tear down the ImGui overlay (Present / WndProc).
5. Close the named-pipe servers.
6. `FreeLibrary` returns — the game process keeps running exactly as
   it would have without the trainer.

---

## Adding a new hook

1. Add a resolution symbol in `TrainerResolve.cpp` next to the
   existing `FindNativeFunction` calls, so the target is located at
   boot even if the cheat is off by default.
2. Add a detour function in the matching cheat file (`Trainer.cpp`,
   `TrainerFreeCraft.cpp`, or `TrainerGiveItem.cpp`). Keep the detour
   minimal — if you need to construct C++ objects, put the SEH probe
   in a separate helper (C2712 rule).
3. Register an `InstallXHook()` function alongside the existing ones,
   and call it from the cheat-toggle path when the bool flips on.
4. Update [Hook Catalog](Hook-Catalog.md) and [Feature
   Reference](Feature-Reference.md).

---

## See also

- [Reflection Internals](Reflection-Internals.md) — how `FindNativeFunction` walks thunks.
- [Memory Layout](Memory-Layout.md) — struct offsets referenced above (`FModifierStateRowHandle`, `DeployableTickSubsystem +0x60`, etc.).
- [Feature Reference](Feature-Reference.md) — player-visible effect for each hook.
