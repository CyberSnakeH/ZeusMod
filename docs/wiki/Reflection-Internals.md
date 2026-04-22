# Reflection Internals

`UObjectLookup` is the single source of truth for *"find an Unreal
symbol at runtime"* in ZeusMod. This page documents what it knows,
how it walks the engine's reflection graph, and what layout
assumptions it bakes in.

If you're adding a new hook or a new UPROPERTY clamp, this is the
file to read.

---

## The UE reflection graph in 90 seconds

Unreal Engine 4 keeps a live, typed graph of every class, struct,
enum, function and property currently loaded. The graph is rooted at
two global tables:

- **`GObjects`** — flat array of `FUObjectItem`. Every UObject ever
  created (classes, CDOs, live instances, script structs) has an
  entry here.
- **`GNames`** — pooled array of `FName` entries. Every string used
  by the reflection system (class names, function names, property
  names) lives here.

On top of those, each `UClass` links to its children through
`UStruct::Children` — a linked list of `UField*`. Walking that list
gives you every property and function defined on the class. The
`Super` pointer gives you the parent class.

Put together, this is enough to turn a **string** ("SurvivalCharacter"
+ "SetHealth") into a **C++ address**, without any AOB scanning.

---

## `UObjectLookup` API surface

```cpp
namespace UObjectLookup {

    // ── Class / struct lookup by string
    UClass*         FindClassByName(const char* name);
    UScriptStruct*  FindScriptStructByName(const char* name);

    // ── FName resolution
    std::string     ResolveFNameByIndex(int32_t comparisonIndex);
    int32_t         ResolveNameIndex(const char* s);   // reverse lookup

    // ── Property / function walking
    size_t          FindPropertyOffset(const char* className,
                                       const char* propertyName);
    UFunction*      FindUFunction      (const char* className,
                                       const char* funcName);
    uint8_t*        FindNativeFunction (const char* className,
                                       const char* funcName);

    // ── GObjects walker
    UObject*        GetObjectByIndex(int32_t idx);
    template<typename Fn>
    void            ForEachObject(Fn&& visitor);

    // ── Misc
    bool            IsA(UObject* obj, const char* className);
    UObject*        FindFirstInstance(const char* className);

}
```

Everything in `Trainer.cpp` and friends goes through this API.
No hardcoded AOB patterns, no SDK dumps, no build-time offsets.

---

## Class lookup: `FindClassByName`

```cpp
UClass* FindClassByName(const char* name);
```

Implementation sketch:

1. Walk `GObjects`.
2. For each entry, check the `ClassPrivate` pointer's UClass name via
   `UObject::Name` (an `FName` at `+0x18`).
3. Accept the first object whose class-name matches **and** whose
   type-chain includes `UClass`, `UBlueprintGeneratedClass`, **or**
   `UWidgetBlueprintGeneratedClass`. The last one was added in 1.x so
   that UMG widget classes (e.g. `UMG_EncumbranceBar_C`) can be
   resolved the same way — essential for reading the encumbrance
   widget's cached UPROPERTY offsets.

FName resolution takes care of the hashed index → string conversion
using `GNames`.

### Caching

Lookups hit a local `std::unordered_map<std::string, UClass*>` that
is never invalidated — class objects in UE are effectively permanent
for a play session.

---

## Property offset: `FindPropertyOffset`

```cpp
size_t FindPropertyOffset(const char* className, const char* propertyName);
```

1. Resolve `className` to its `UClass*`.
2. Walk `UStruct::Children` (`+0x40`) as an `FField*` linked list.
3. For each field, compare `FField::Name` to `propertyName`.
4. On match, return `FProperty::Offset_Internal` (`+0x44` in UE 4.27).
5. If no match on this class, walk to `UStruct::SuperStruct`
   (`+0x30`) and keep searching.

Returns `0` if not found. Every caller we have is expected to assert
against that sentinel (no cheat ships with a `0`-offset UPROPERTY —
that would be a clamp against the object header).

---

## UFunction lookup: `FindUFunction` / `FindNativeFunction`

`FindUFunction` walks `UStruct::Children` just like the property
walker, but filters for `UField::Class == UFunction::StaticClass()`.

`FindNativeFunction` additionally **walks the thunk**:

```cpp
UFunction* fn = FindUFunction(cls, name);
uint8_t*   thunk = (uint8_t*)fn->Func;     // Kismet thunk
uint8_t*   impl  = WalkThunkToImpl(thunk); // C++ exec body
return impl;
```

`WalkThunkToImpl` decodes a small number of x64 prologue/RIP-relative
instructions to find the `exec<Function>` native body that the thunk
jumps to. That's the address MinHook needs — detouring the thunk
itself would only intercept Blueprint calls, not native ones.

---

## GObjects enumeration

```cpp
template<typename Fn>
void ForEachObject(Fn&& visitor);
```

Walks every entry in `GObjects`. Each slot is an `FUObjectItem`:

```cpp
struct FUObjectItem {
    UObject* Object;      // +0x00
    int32_t  Flags;       // +0x08
    int32_t  ClusterIdx;  // +0x0C
    int32_t  SerialNum;   // +0x14  (our layout assumption)
};
```

`SerialNum` is read at `+0x14`. This is one of a handful of UE 4.27
layout assumptions ZeusMod bakes in (see
[Memory Layout](Memory-Layout.md)). It's used when we need to insert
a valid `FWeakObjectPtr{ObjectIndex, SerialNumber}` into an
unreflected TArray — the Free Craft subsystem patch, for example.

---

## FName resolution

```cpp
std::string ResolveFNameByIndex(int32_t comparisonIndex);
```

Walks the `GNames` chunked array for the entry with that comparison
index, reads the ANSI/UCS2 payload, and returns a `std::string`.
Caches by index.

The inverse — name → index — is used when Free Craft injects a
handle into the processor `TArray`.

---

## Live behaviour

`UObjectLookup` is populated lazily. On DLL attach we do **one**
pre-resolve pass for the symbols that every cheat relies on
(`ResolveAllOffsets()` — see `TrainerResolve.cpp`). That pass also
writes a concise log of what it found and what it didn't, so if an
Icarus patch renames a property you get an immediate diagnostic
instead of a silent no-op.

---

## Testing reflection lookups by hand

Every call the DLL makes is reachable from `inspect.py`:

```text
findcls Inventory                    # UClass address
props   Inventory                    # own properties of the class
propsall Inventory                   # + Super walk
propoff Inventory CurrentWeight      # single property offset
funcoff IcarusFunctionLibrary:AddModifierState
listobj Inventory:10                 # first 10 live instances
```

If ZeusMod can't find something, `inspect.py` can — or vice versa.
The asymmetry makes for very fast debugging on a new game build.

---

## See also

- [Hook Catalog](Hook-Catalog.md) — every function we resolve at runtime.
- [Memory Layout](Memory-Layout.md) — all the non-reflected offsets.
- [Debug Client](Debug-Client.md) — reflection walk from Python.
