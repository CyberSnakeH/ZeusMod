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

namespace UObjectLookup {

// One-time init: AOB-scans for GUObjectArray and FNamePool. Returns true on
// success. Logs progress to stdout. Idempotent (safe to call again).
bool Initialize();

bool IsInitialized();

// === Raw object array access ===

int32_t GetObjectCount();
uintptr_t GetObjectByIndex(int32_t index);

// === Name reading ===

// Resolve an FName at the given memory location to a string. Empty string
// if read fails. Strips the _N number suffix if present (use GetNameWithNumber
// if you need it).
std::string ReadFNameAt(uintptr_t fnameAddr);

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

} // namespace UObjectLookup
