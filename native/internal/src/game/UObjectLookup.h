#pragma once
// ============================================================================
// UObjectLookup — name-based lookup of UE4 UFunctions / UClasses / UObjects.
//
// Ported from UE5CEDumper (Aura/Serie/Genau modules), trimmed down to the
// minimum needed for Icarus (UE 4.27 with standard FNamePool layout).
//
// Why this exists: PDB/RVA-based offsets break on every game patch. AOBs are
// more resilient but still drift. UFunction names ARE STABLE across patches
// as long as the function itself is not renamed in source, which is rare.
// This module finds functions by their UE reflection name and returns the
// native C++ entry point — fully patch-proof.
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace UObjectLookup {

// One-time init: AOB-scans for GUObjectArray and FNamePool. Returns true on
// success. Logs progress to stdout. Idempotent (safe to call again).
bool Initialize();

bool IsInitialized();

// === Raw object array access ===

int32_t GetObjectCount();
uintptr_t GetObjectByIndex(int32_t index);

// Read the SerialNumber field of the FUObjectItem at the given index.
// Used to construct a valid FWeakObjectPtr value { ObjectIndex, SerialNumber }
// that will pass FWeakObjectPtr::Get()'s serial check when the game later
// dereferences it. Returns 0 on any read failure or out-of-range index.
int32_t GetObjectSerialNumberByIndex(int32_t index);

// === Name reading ===

// Resolve an FName at the given memory location to a string. Empty string
// if read fails. Strips the _N number suffix if present (use GetNameWithNumber
// if you need it).
std::string ReadFNameAt(uintptr_t fnameAddr);

// Resolve a bare FName ComparisonIndex (int32, from the name pool) to a
// string. Empty if the index is out of range. Used by the pipe command
// `fname <index>` to decode raw FName values without going through memory.
std::string ResolveFNameByIndex(int32_t comparisonIndex);

// Convenience: read the Name field of a UObject (offset +0x18).
std::string GetObjectName(uintptr_t uobjectAddr);

// Read the class name of a UObject (the name of its UClass, e.g. "Function",
// "ProcessingComponent", etc.).
std::string GetObjectClassName(uintptr_t uobjectAddr);

// === Function lookup ===

// Walk a UClass's Children chain (UStruct.Children at +0x48) looking for a
// UFunction whose name matches. Returns the UObject address of the UFunction,
// or 0 if not found.
uintptr_t FindFunctionInClass(uintptr_t uclassAddr, const char* funcName);

// Linear scan of GUObjectArray for a UClass with the given short name
// (case-insensitive). Returns 0 if not found. Slow — caller should cache.
uintptr_t FindClassByName(const char* className);

// Linear scan of GUObjectArray for all *instances* (not the UClass itself)
// whose class is `className` or derives from it when `includeSubclasses=true`.
// Skips class default objects (CDOs). Slow — caller should cache and refresh
// periodically rather than every tick.
//
// Use case: find every USurvivalCharacterState in the world so a host trainer
// can apply survival cheats authoritatively to all players on the server,
// not just the local one. Replication naturally propagates the writes to
// remote clients.
std::vector<uintptr_t> FindAllInstancesOfClass(const char* className,
                                               bool includeSubclasses = true);

// Same as FindClassByName but also matches UScriptStruct instances. Use when
// you need to walk the ChildProperties of a C++ USTRUCT (FItemData etc.).
uintptr_t FindStructByName(const char* structName);

// Read UFunction::Func (the native entry point) at offset +0xD8 (UE 4.27).
// Returns 0 if the read fails or the value isn't in the module's code range.
// NOTE: this is the BP-callable THUNK, not the C++ impl. Use ResolveThunkToImpl
// to walk past the thunk and get the actual C++ implementation address.
uintptr_t GetUFunctionNativeAddr(uintptr_t ufunctionAddr);

// Disassemble a UFunction thunk to find the CALL to its C++ implementation.
// Returns the impl address, or 0 if no valid CALL target found in the
// thunk's first 256 bytes. The impl is what you want for byte patching
// (`mov eax, 0 / 9999 / 1 ; ret`) — patching the thunk corrupts FFrame.
uintptr_t ResolveThunkToImpl(uintptr_t thunkAddr);

// === UPROPERTY offset resolver =============================================

// Find the byte offset of a UPROPERTY within a class, walking the FField
// (ChildProperties) chain. If `propName` is not on this class directly, walks
// the Super chain to find it on a parent class. Returns -1 if not found.
//
// Example:
//   int healthOff = UObjectLookup::FindPropertyOffset("ActorState", "Health");
//   // healthOff == 0x1D8 (or whatever it is in this build)
int32_t FindPropertyOffset(const char* className, const char* propName);

// Same as above, but with a pre-resolved class pointer. Walks the FField
// chain on that class only (no Super walking).
int32_t FindPropertyOffsetInClass(uintptr_t uclassAddr, const char* propName);

// Like FindPropertyOffset but tries FindStructByName first (UScriptStruct),
// then falls back to FindClassByName (UClass). Use this for offsets INSIDE
// struct types like FItemData, FInventorySlot, FItemDynamicData — where
// FindPropertyOffset fails because it only walks UClass registries.
int32_t FindStructPropertyOffset(const char* structName, const char* propName);

// Resolve a named member of a UEnum to its integer value. Example:
//   int dur = ResolveEnumValue("EDynamicItemProperties", "Durability"); // == 6
// Returns -1 if the enum or member isn't found. Safer than hardcoding
// the integer across game updates.
int32_t ResolveEnumValue(const char* enumName, const char* memberName);

// One-shot: find class by name, find function in it by name, return its
// native entry point. Logs the resolved chain. Returns 0 on any failure.
//
// Example:
//   uintptr_t fn = UObjectLookup::FindNativeFunction(
//       "ProcessingComponent", "CanQueueItem");
//   // fn is now the same value as your old PDB-based m_canQueueItemAddr,
//   // but resolved by name and stable across game updates.
uintptr_t FindNativeFunction(const char* className, const char* funcName);

// Enumerate all UClass / BPGC instances whose name contains the given
// substring (case-insensitive). Logs each match with class name + outer.
// Slow (full GObjects walk) — use only for debugging / one-shot discovery.
void DumpClassesContaining(const char* substring, int maxResults = 50);

// Enumerate all UFunction instances on a UClass and log their names.
void DumpFunctionsOf(uintptr_t uclassAddr, int maxResults = 200);

// Walk a UFunction's ChildProperties chain and print each parameter
// (name, type, offset, size, Parm/Out/Return/Ref flags). Required to build
// a ProcessEvent param buffer for calling the function.
void DumpUFunctionSignature(uintptr_t ufunctionAddr, const char* label = nullptr);
void DumpUFunctionSignatureByName(const char* className, const char* funcName);

// Enumerate UDataTable UObject instances whose name contains `substring`.
// Logs the data table address, its RowStruct class name, and an estimate
// of the number of rows. Used to locate the item template DataTable.
void DumpDataTableInstances(const char* substring, int maxResults = 50);

// ============================================================================
// UFunction invocation via UObject::ProcessEvent
// ----------------------------------------------------------------------------
// UObject::ProcessEvent(UFunction*, void* Params) is a virtual member at a
// known vtable slot. We resolve it once by reading an instance's vtable and
// cache the pointer. The params buffer format is: for each FProperty on the
// UFunction's ChildProperties chain, write the arg value at its Offset_Internal
// (walked via FindPropertyOffsetInClass on the UFunction itself). Return values
// are read from the same buffer at UFunction::ReturnValueOffset.
//
// For static blueprint function libraries (ItemTemplateLibrary, Inventory-
// ItemLibrary) the `self` arg can be ANY valid UObject — the library uses
// it only to find WorldContext. The CDO of the library class also works.
// ============================================================================

// Discover the ProcessEvent vtable slot by disassembling any cached UObject.
// Must be called once after GObjects is ready. Returns the resolved pointer,
// or nullptr on failure. Idempotent; subsequent calls return the cached value.
using FnProcessEvent = void(__fastcall*)(void* self, void* ufunction, void* params);
FnProcessEvent ResolveProcessEvent(void* knownInstance);

// Call a UFunction via ProcessEvent. Returns true on success. `paramsBuffer`
// must be at least `UFunction::ParmsSize` bytes and have each arg written at
// its property offset. For static library calls, pass any UObject as `self`.
bool CallUFunction(void* self, uintptr_t ufunction, void* paramsBuffer);

} // namespace UObjectLookup
