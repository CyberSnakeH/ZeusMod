#pragma once
// Internal-only helpers shared across the Trainer* translation units.
// Not part of the public trainer API — consumers outside of
// src/cheats/Trainer*.cpp should use Trainer.h instead.
//
// Shape of the split:
//   Trainer.cpp              Lifecycle + Tick dispatcher + core patches + IPC pipe
//   TrainerResolve.cpp       Trainer::ResolveAllOffsets (UProperty table)
//   TrainerDiagnostics.cpp   UE reflection/introspection helpers
//   TrainerFreeCraft.cpp     Free-craft hook subsystem (the big one)
//
// Anything declared here lives in exactly one of those .cpp files; this
// header just makes it visible across them without leaking into Trainer.h.

#include <cstdint>
#include <cstddef>
#include <cmath>
#include "UE4.h"
#include "UObjectLookup.h"
#include "Logger.h"

// ============================================================================
// AOB constants for UE 4.27 Icarus-Win64-Shipping
// ============================================================================
// Craft-exclusive validation hook offsets. NEVER add hooks for functions
// called by the processing tick (CanProcess, CanStartProcessing,
// DoProcessInternal) or inventory side-effects (ConsumeItem entry, food
// consumption) — those are hot paths called every frame for every
// deployable/actor in the world and a hook mistake crashes the game.
// CanSatisfyRecipeQueryInput is deliberately NOT hooked: it is a
// Blueprint-callable UFunction invoked via UFunction::Invoke which uses
// the (UObject*, FFrame&, void*) script calling convention, not the
// __fastcall (this, input, mult, inv, amt*) convention our detour expects.

inline constexpr const char* kSetHealthWriteAob =
    "79 04 33 ?? EB 09 41 8B ?? 41 3B ?? 0F 4C ?? 89 ?? D8 01 00 00";
inline constexpr const char* kScaledInputAob =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B E9 49 8B F8 8B DA 48 8B F1 E8 ?? ?? ?? ?? 84 C0 75 04 33 C0 EB 3E";
inline constexpr const char* kScaledResourceAob =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B E9 49 8B D8 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 84 C0 75 04 33 C0 EB 3E";
inline constexpr const char* kConsumeItemAob =
    "48 3B F9 75 F2 44 29 66 04 E9";
inline constexpr const char* kFindItemCountAob =
    "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 "
    "48 81 EC 90 00 00 00 41 0F B6 D8 4C 8B E2 4C 8B E9 33 FF 44 8B F7 "
    "33 D2 41 8B 4C 24 08 E8 ?? ?? ?? ?? 44 0F B6 C8 41 39 7C 24 0C "
    "0F 94 C0 41 84 C1 74 07 33 C0 E9";
inline constexpr const char* kGetTotalWeightAob =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 "
    "48 8D 6C 24 A9 48 81 EC 90 00 00 00 33 DB C7 45 EB 01 00 00 00 "
    "4C 8D B1 E8 00 00 00";
inline constexpr const char* kCanSatisfyRecipeInputAob =
    "4C 8B DC 49 89 5B 08 49 89 6B 10 4D 89 4B 20 56 57 41 54 41 56 41 57 48 83 EC 60 "
    "49 8B F9 41 8B F0 48 8B EA 4C 8B E1 33 C9 4C 8B B4 24 B0 00 00 00 41 89 0E 0F AF 72 18";
inline constexpr const char* kCanQueueItemAob =
    "48 8B C4 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 68 98 48 81 EC 28 01 00 00 0F 29 70 A8 0F 29 78 98 4D 8B E0 48 8B FA 4C 8B F1 "
    "48 8B 99 A0 00 00 00 48 8B CA E8 ?? ?? ?? ?? 4C 8B C3 48 8B D0 48 8D 4F 08 E8 ?? ?? ?? ??";
inline constexpr const char* kGetMaxCraftableStackAob =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 E1 48 81 EC C8 00 00 00 49 8B F0 48 8B CA E8 ?? ?? ?? ?? 4C 8B F0 48 89 45 AF 48 85 C0 75 08 48 8B 0E E9";
inline constexpr const char* kCanSatisfyRecipeQueryInputAob =
    "4C 8B DC 49 89 5B 08 49 89 6B 10 4D 89 4B 20 56 57 41 54 41 56 41 57 48 83 EC 60 "
    "49 8B F9 41 8B F0 48 8B EA 4C 8B E1 33 C9 4C 8B B4 24 B0 00 00 00";
inline constexpr const char* kGetItemCountAob =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 99 F8 01 00 00 33 FF "
    "48 63 81 00 02 00 00 48 8D 34 C0 48 C1 E6 06 48 03 F3 48 3B DE 74 40 90";
// UInventory::ConsumeItem entry prologue. 25 bytes — very distinctive.
//   mov [rsp+0x18], rbx / mov [rsp+0x20], rbp / push rsi rdi r12 r14 r15 /
//   sub rsp, 0x420
inline constexpr const char* kConsumeItemEntryAob =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC ?? 04 00 00";

// ============================================================================
// Inline helpers
// ============================================================================

inline float NormalizeLockedHour(float hour) {
    float normalized = std::fmod(hour, 24.0f);
    if (normalized < 0.0f) normalized += 24.0f;
    return normalized;
}

inline bool MatchPrefix(uintptr_t addr, const uint8_t* bytes, size_t count) {
    auto* p = reinterpret_cast<const uint8_t*>(addr);
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != bytes[i]) return false;
    }
    return true;
}

inline uintptr_t ResolveNativeOrAob(const char* className, const char* fnName,
                                    uintptr_t moduleBase, size_t moduleSize,
                                    const char* aob, const char* logName) {
    if (UObjectLookup::IsInitialized()) {
        uintptr_t addr = UObjectLookup::FindNativeFunction(className, fnName);
        if (addr) {
            LOG_RESOLVE("%s: name lookup -> 0x%p", logName, reinterpret_cast<void*>(addr));
            return addr;
        }
    }

    if (aob && *aob) {
        uintptr_t hit = UE4::PatternScan(moduleBase, moduleSize, aob);
        if (hit) {
            LOG_RESOLVE("%s: AOB -> 0x%p", logName, reinterpret_cast<void*>(hit));
            return hit;
        }
    }

    LOG_RESOLVE("%s: runtime resolution failed", logName);
    return 0;
}

inline uintptr_t ResolveNativeOnly(const char* className, const char* fnName, const char* logName) {
    if (UObjectLookup::IsInitialized()) {
        uintptr_t addr = UObjectLookup::FindNativeFunction(className, fnName);
        if (addr) {
            LOG_RESOLVE("%s: name lookup -> 0x%p", logName, reinterpret_cast<void*>(addr));
            return addr;
        }
    }

    LOG_RESOLVE("%s: name lookup failed", logName);
    return 0;
}

// ============================================================================
// Cross-module forward declarations
// ============================================================================

// Defined in TrainerFreeCraft.cpp. Named `Trainer_*` because the underlying
// implementations live inside an anonymous namespace (shared file-static
// state); these are the external-linkage trampolines Trainer.cpp calls.
void Trainer_ResetFreeCraftTelemetry();
void Trainer_PollArpcTrackedProcessor();
void Trainer_InstallCraftValidationHooks(uintptr_t base, size_t sz);
void Trainer_ResolveAndValidateTickSubsystem();
void Trainer_ClampTalentModelAvailablePoints(void* controller, int32_t value);
void Trainer_StartArpcFastWatcher();

// Defined in TrainerGiveItem.cpp — reflection-driven item injection.
void Trainer_GiveItem_Init();
void Trainer_GiveItem_LogSamples();
bool Trainer_GiveItem(const char* rawName, int count);
bool Trainer_AddItemToProcessor(void* procComp, const char* rawName, int count);
#include <vector>
#include <string>
const std::vector<std::string>& Trainer_GiveItem_GetAllNames();

// Defined in TrainerDiagnostics.cpp.
void  DumpClassProperties(const char* className);
void  DiscoverTickSubsystem();
void  HexDump(const char* label, void* obj, size_t bytes);
void  DumpMetaResourceArray(const char* label, uintptr_t arrayAddr);
void  EnumerateLiveInstancesWithProbe(const char* className, uintptr_t probeOff, int maxResults);
void* FindFirstLiveInstance(const char* className);
void* SafeReadPtrAt(const void* base, uintptr_t off);
void* FindCharacterComponentByClass(void* character, const char* className);
void* FindFirstInstanceWithNonNullProbe(const char* className, uintptr_t probeOff);
