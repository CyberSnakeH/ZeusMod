#include "Trainer.h"
#include "UE4.h"
#include "UObjectLookup.h"
#include "libs/minhook/include/MinHook.h"
#include <cmath>
#include <intrin.h>

// All memory access goes through ReadAt/WriteAt with __try for safety

namespace {
// Craft-exclusive validation hook offsets. NEVER add hooks for functions
// called by the processing tick (CanProcess, CanStartProcessing,
// DoProcessInternal) or inventory side-effects (ConsumeItem entry, food
// consumption) - those are hot paths called every frame for every
// deployable/actor in the world and a hook mistake crashes the game.
// CanSatisfyRecipeQueryInput is deliberately NOT hooked: it is a
// Blueprint-callable UFunction invoked via UFunction::Invoke which uses
// the (UObject*, FFrame&, void*) script calling convention, not the
// __fastcall (this, input, mult, inv, amt*) convention our detour
// expects. Hooking it crashes with ILLEGAL_INSTRUCTION because the
// garbage `currentAmount` argument (read from [rsp+0x28] which is not
// set by the script frame layout) gets dereferenced and written to.
constexpr const char* kSetHealthWriteAob =
    "79 04 33 ?? EB 09 41 8B ?? 41 3B ?? 0F 4C ?? 89 ?? D8 01 00 00";
constexpr const char* kScaledInputAob =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B E9 49 8B F8 8B DA 48 8B F1 E8 ?? ?? ?? ?? 84 C0 75 04 33 C0 EB 3E";
constexpr const char* kScaledResourceAob =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 "
    "49 8B E9 49 8B D8 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 84 C0 75 04 33 C0 EB 3E";
constexpr const char* kConsumeItemAob =
    "48 3B F9 75 F2 44 29 66 04 E9";
constexpr const char* kFindItemCountAob =
    "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 "
    "48 81 EC 90 00 00 00 41 0F B6 D8 4C 8B E2 4C 8B E9 33 FF 44 8B F7 "
    "33 D2 41 8B 4C 24 08 E8 ?? ?? ?? ?? 44 0F B6 C8 41 39 7C 24 0C "
    "0F 94 C0 41 84 C1 74 07 33 C0 E9";
constexpr const char* kGetTotalWeightAob =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 "
    "48 8D 6C 24 A9 48 81 EC 90 00 00 00 33 DB C7 45 EB 01 00 00 00 "
    "4C 8D B1 E8 00 00 00";
constexpr const char* kCanSatisfyRecipeInputAob =
    "4C 8B DC 49 89 5B 08 49 89 6B 10 4D 89 4B 20 56 57 41 54 41 56 41 57 48 83 EC 60 "
    "49 8B F9 41 8B F0 48 8B EA 4C 8B E1 33 C9 4C 8B B4 24 B0 00 00 00 41 89 0E 0F AF 72 18";
constexpr const char* kCanQueueItemAob =
    "48 8B C4 4C 89 40 18 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 68 98 48 81 EC 28 01 00 00 0F 29 70 A8 0F 29 78 98 4D 8B E0 48 8B FA 4C 8B F1 "
    "48 8B 99 A0 00 00 00 48 8B CA E8 ?? ?? ?? ?? 4C 8B C3 48 8B D0 48 8D 4F 08 E8 ?? ?? ?? ??";
constexpr const char* kGetMaxCraftableStackAob =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 E1 48 81 EC C8 00 00 00 49 8B F0 48 8B CA E8 ?? ?? ?? ?? 4C 8B F0 48 89 45 AF 48 85 C0 75 08 48 8B 0E E9";
constexpr const char* kCanSatisfyRecipeQueryInputAob =
    "4C 8B DC 49 89 5B 08 49 89 6B 10 4D 89 4B 20 56 57 41 54 41 56 41 57 48 83 EC 60 "
    "49 8B F9 41 8B F0 48 8B EA 4C 8B E1 33 C9 4C 8B B4 24 B0 00 00 00";
constexpr const char* kGetItemCountAob =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 99 F8 01 00 00 33 FF "
    "48 63 81 00 02 00 00 48 8D 34 C0 48 C1 E6 06 48 03 F3 48 3B DE 74 40 90";

// UInventory::ConsumeItem entry prologue, verified from the user's
// runtime dump (prefix mismatch log showed actual bytes at PDB offset).
// MSVC spills rbx/rbp via mov FIRST, then pushes the other callee-saved
// regs, then allocates the stack frame:
//   mov [rsp+0x18], rbx
//   mov [rsp+0x20], rbp
//   push rsi; push rdi; push r12; push r14; push r15
//   sub rsp, 0x420
// 25 bytes total - very distinctive.
constexpr const char* kConsumeItemEntryAob =
    "48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC ?? 04 00 00";

float NormalizeLockedHour(float hour) {
    float normalized = std::fmod(hour, 24.0f);
    if (normalized < 0.0f) normalized += 24.0f;
    return normalized;
}

bool MatchPrefix(uintptr_t addr, const uint8_t* bytes, size_t count) {
    auto* p = reinterpret_cast<const uint8_t*>(addr);
    for (size_t i = 0; i < count; ++i) {
        if (p[i] != bytes[i]) return false;
    }
    return true;
}

uintptr_t ResolveNativeOrAob(const char* className, const char* fnName,
                             uintptr_t moduleBase, size_t moduleSize,
                             const char* aob, const char* logName) {
    if (UObjectLookup::IsInitialized()) {
        uintptr_t addr = UObjectLookup::FindNativeFunction(className, fnName);
        if (addr) {
            printf("[RESOLVE] %s: name lookup -> 0x%p\n", logName, reinterpret_cast<void*>(addr));
            return addr;
        }
    }

    if (aob && *aob) {
        uintptr_t hit = UE4::PatternScan(moduleBase, moduleSize, aob);
        if (hit) {
            printf("[RESOLVE] %s: AOB -> 0x%p\n", logName, reinterpret_cast<void*>(hit));
            return hit;
        }
    }

    printf("[RESOLVE] %s: runtime resolution failed\n", logName);
    return 0;
}

uintptr_t ResolveNativeOnly(const char* className, const char* fnName, const char* logName) {
    if (UObjectLookup::IsInitialized()) {
        uintptr_t addr = UObjectLookup::FindNativeFunction(className, fnName);
        if (addr) {
            printf("[RESOLVE] %s: name lookup -> 0x%p\n", logName, reinterpret_cast<void*>(addr));
            return addr;
        }
    }

    printf("[RESOLVE] %s: name lookup failed\n", logName);
    return 0;
}

// ----- Craft-exclusive validation hooks ---------------------------------
// Only these functions are hooked. They are called ONLY during craft UI
// interaction / OnServer_AddProcessingRecipe - never from the per-tick
// processing system. Any hook in the processing tick path crashes the
// game via UProcessingComponent::DoProcessInternal.
using FnCanSatisfy           = bool(__fastcall*)(void*, void*, int, void*, int*);
using FnCanQueueItem         = bool(__fastcall*)(void*, void*, void*);
using FnGetMaxCraftableStack = int (__fastcall*)(void*, void*, void*, void*);
using FnConsumeItem          = bool(__fastcall*)(void*, int, int, bool);
using FnHasSufficientResource = bool(__fastcall*)(void*, void*, int, int, void*);
using FnGetResourceRecipeValidity = int(__fastcall*)(void*, void*, int, void*);
using FnHasWaterSourceConnection = bool(__fastcall*)(void*, bool);
using FnServerStartProcessing = void(__fastcall*)(void*);
using FnServerActivateProcessor = void(__fastcall*)(void*);

static FnCanSatisfy           g_origCanSatisfyRecipeInput      = nullptr;
static FnCanQueueItem         g_origCanQueueItem               = nullptr;
static FnGetMaxCraftableStack g_origGetMaxCraftableStack       = nullptr;
static FnConsumeItem          g_origConsumeItem                = nullptr;
static FnHasSufficientResource g_origHasSufficientResource     = nullptr;
static FnGetResourceRecipeValidity g_origGetResourceRecipeValidity = nullptr;
static FnHasWaterSourceConnection g_origHasWaterSourceConnection = nullptr;
static FnServerStartProcessing g_origServerStartProcessing = nullptr;
static FnServerActivateProcessor g_origServerActivateProcessor = nullptr;
static bool     s_craftHooksInstalled = false;
static uintptr_t s_moduleBase         = 0;
static uintptr_t s_hasSufficientResourceAddr   = 0;
static uintptr_t s_getResourceRecipeValidityAddr = 0;
static void* s_pendingProcessorKickSelf = nullptr;
static void* s_lastKickedProcessorSelf = nullptr;
static ULONGLONG s_lastProcessorKickTick = 0;

inline uintptr_t RelCaller(void* ret) {
    return s_moduleBase ? (uintptr_t)ret - s_moduleBase : (uintptr_t)ret;
}

// Scan backward from `addr` for the end-of-previous-function marker
// (RET followed by INT3 padding). Used to locate the start of the
// function that called one of our hooks, so we can dump its prologue.
uintptr_t FindFuncStart(uintptr_t addr, uintptr_t maxBack = 0x4000) {
    __try {
        for (uintptr_t i = 1; i < maxBack; ++i) {
            uint8_t* p = reinterpret_cast<uint8_t*>(addr - i);
            if (p[0] == 0xC3 && p[1] == 0xCC) {
                uint8_t* q = p + 1;
                int pad = 0;
                while (*q == 0xCC && pad < 32) { ++q; ++pad; }
                return reinterpret_cast<uintptr_t>(q);
            }
        }
    } __except (1) {}
    return 0;
}

void DumpCallerContext(void* retAddr, const char* label) {
    __try {
        uintptr_t ret = reinterpret_cast<uintptr_t>(retAddr);
        uint8_t* p = reinterpret_cast<uint8_t*>(ret);

        printf("[DUMP] %s post-call bytes (ret=+0x%llX):",
            label, (unsigned long long)RelCaller(retAddr));
        for (int i = 0; i < 64; ++i) printf(" %02X", p[i]);
        printf("\n");

        // Also dump 16 bytes BEFORE the return address - this is the
        // tail end of the CALL instruction plus whatever came before it.
        uint8_t* q = reinterpret_cast<uint8_t*>(ret - 16);
        printf("[DUMP] %s pre-call bytes (ret-16):", label);
        for (int i = 0; i < 16; ++i) printf(" %02X", q[i]);
        printf("\n");

        uintptr_t funcStart = FindFuncStart(ret);
        if (funcStart) {
            uint8_t* fp = reinterpret_cast<uint8_t*>(funcStart);
            printf("[DUMP] %s enclosing func at +0x%llX prologue:",
                label, (unsigned long long)RelCaller(reinterpret_cast<void*>(funcStart)));
            for (int i = 0; i < 48; ++i) printf(" %02X", fp[i]);
            printf("\n");
        } else {
            printf("[DUMP] %s func start not found\n", label);
        }
    } __except (1) {
        printf("[DUMP] %s exception while reading memory\n", label);
    }
}

static bool s_dumpedCanQueueCtx = false;
static bool s_dumpedCanSatisfyCtx = false;
static bool s_dumpedHasSufficientCtx = false;
static bool s_dumpedGetRecipeValidityCtx = false;
static bool s_loggedItemPath = false;
static bool s_loggedResourcePath = false;
static bool s_loggedProcessorPath = false;

// Verbose per-call logging: first N invocations per hook are printed with
// parameters so we can see the exact sequence during a craft click.
constexpr int kMaxHookLogPerHook = 12;
static int s_logCanSatisfyCount = 0;
static int s_logCanQueueCount = 0;
static int s_logGetMaxStackCount = 0;
static int s_logConsumeItemCount = 0;
static int s_logHasSufficientResourceCount = 0;
static int s_logGetRecipeValidityCount = 0;
static int s_logHasWaterSourceCount = 0;
static int s_logStartProcessingCount = 0;
static int s_logActivateProcessorCount = 0;

enum class FreeCraftPathKind {
    Items,
    Resources,
    Processor,
};

const char* FreeCraftPathKindName(FreeCraftPathKind kind) {
    switch (kind) {
    case FreeCraftPathKind::Items: return "Items";
    case FreeCraftPathKind::Resources: return "Resources";
    default: return "Processor";
    }
}

void LogFreeCraftPath(FreeCraftPathKind kind, const char* fnName, void* retAddr) {
    bool& logged =
        (kind == FreeCraftPathKind::Items) ? s_loggedItemPath :
        (kind == FreeCraftPathKind::Resources) ? s_loggedResourcePath :
        s_loggedProcessorPath;
    if (!logged) {
        logged = true;
        if (retAddr) {
            printf("[PATH] FreeCraft category=%s via %s caller=+0x%llX\n",
                FreeCraftPathKindName(kind), fnName, (unsigned long long)RelCaller(retAddr));
        } else {
            printf("[PATH] FreeCraft category=%s via %s caller=<unknown>\n",
                FreeCraftPathKindName(kind), fnName);
        }
    }
}

void ResetFreeCraftTelemetry() {
    s_dumpedCanQueueCtx = false;
    s_dumpedCanSatisfyCtx = false;
    s_dumpedHasSufficientCtx = false;
    s_dumpedGetRecipeValidityCtx = false;
    s_loggedItemPath = false;
    s_loggedResourcePath = false;
    s_loggedProcessorPath = false;
    s_logCanSatisfyCount = 0;
    s_logCanQueueCount = 0;
    s_logGetMaxStackCount = 0;
    s_logConsumeItemCount = 0;
    s_logHasSufficientResourceCount = 0;
    s_logGetRecipeValidityCount = 0;
    s_logHasWaterSourceCount = 0;
    s_logStartProcessingCount = 0;
    s_logActivateProcessorCount = 0;
    s_pendingProcessorKickSelf = nullptr;
    s_lastKickedProcessorSelf = nullptr;
    s_lastProcessorKickTick = 0;
}

void QueueProcessorKick(void* self) {
    if (self) s_pendingProcessorKickSelf = self;
}

bool __fastcall HookCanSatisfyRecipeInput(void* self, void* input, int multiplier, void* inventories, int* currentAmount) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "CanSatisfyRecipeInput", ret);
        if (s_logCanSatisfyCount++ < kMaxHookLogPerHook)
            printf("[HOOK] CanSatisfyRecipeInput#%d caller=+0x%llX self=%p input=%p mult=%d\n",
                s_logCanSatisfyCount, (unsigned long long)RelCaller(ret), self, input, multiplier);
        if (!s_dumpedCanSatisfyCtx) {
            s_dumpedCanSatisfyCtx = true;
            DumpCallerContext(ret, "CanSatisfyRecipeInput");
        }
        // Don't write *currentAmount - may be a script-frame garbage
        // pointer that corrupts code memory and triggers a delayed
        // ILLEGAL_INSTRUCTION crash. Returning true is enough.
        return true;
    }
    return g_origCanSatisfyRecipeInput(self, input, multiplier, inventories, currentAmount);
}

bool __fastcall HookCanQueueItem(void* self, void* recipeToQueue, void* inventories) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        QueueProcessorKick(self);
        LogFreeCraftPath(FreeCraftPathKind::Items, "CanQueueItem", ret);
        if (s_logCanQueueCount++ < kMaxHookLogPerHook)
            printf("[HOOK] CanQueueItem#%d caller=+0x%llX self=%p recipe=%p inv=%p\n",
                s_logCanQueueCount, (unsigned long long)RelCaller(ret), self, recipeToQueue, inventories);
        if (!s_dumpedCanQueueCtx) {
            s_dumpedCanQueueCtx = true;
            DumpCallerContext(ret, "CanQueueItem");
        }
        return true;
    }
    return g_origCanQueueItem(self, recipeToQueue, inventories);
}

int __fastcall HookGetMaxCraftableStack(void* self, void* recipe, void* inventories, void* craftingPlayer) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "GetMaxCraftableStack", ret);
        if (s_logGetMaxStackCount++ < kMaxHookLogPerHook)
            printf("[HOOK] GetMaxCraftableStack#%d caller=+0x%llX self=%p recipe=%p\n",
                s_logGetMaxStackCount, (unsigned long long)RelCaller(ret), self, recipe);
        return 9999;
    }
    return g_origGetMaxCraftableStack(self, recipe, inventories, craftingPlayer);
}

// UInventory::ConsumeItem(int location, int amount, bool clearItemSave).
// Standard __fastcall, NOT a Blueprint UFunction. Returning true bypasses
// the slot-search loop entirely - critical when FreeCraft is on with zero
// real items, because the loop's "slot not found" early-exit makes the
// inline-SUB NOP useless. Recipe sits in queue forever otherwise.
bool __fastcall HookConsumeItem(void* self, int location, int amount, bool clearItemSave) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "ConsumeItem", ret);
        if (s_logConsumeItemCount++ < kMaxHookLogPerHook)
            printf("[HOOK] ConsumeItem#%d self=%p loc=%d amt=%d clear=%d\n",
                s_logConsumeItemCount, self, location, amount, (int)clearItemSave);
        return true;
    }
    return g_origConsumeItem(self, location, amount, clearItemSave);
}

bool __fastcall HookHasSufficientResource(void* self, void* resourceType, int requiredAmount,
                                          int recipeCost, void* additionalInventories) {
    void* ret = _ReturnAddress();
    bool orig = g_origHasSufficientResource(self, resourceType, requiredAmount,
                                            recipeCost, additionalInventories);
    if (Trainer::Get().FreeCraft) {
        QueueProcessorKick(self);
        LogFreeCraftPath(FreeCraftPathKind::Resources, "HasSufficientResource", ret);
        if (s_logHasSufficientResourceCount++ < kMaxHookLogPerHook) {
            printf("[GATE] HasSufficientResource#%d caller=+0x%llX self=%p resource=%p req=%d cost=%d addInv=%p orig=%d -> 1\n",
                s_logHasSufficientResourceCount, (unsigned long long)RelCaller(ret), self, resourceType, requiredAmount,
                recipeCost, additionalInventories, (int)orig);
        }
        if (!s_dumpedHasSufficientCtx) {
            s_dumpedHasSufficientCtx = true;
            DumpCallerContext(ret, "HasSufficientResource");
        }
        return true;
    }
    return orig;
}

int __fastcall HookGetResourceRecipeValidity(void* self, void* resourceType, int requiredAmount,
                                             void* additionalInventories) {
    void* ret = _ReturnAddress();
    int orig = g_origGetResourceRecipeValidity(self, resourceType, requiredAmount, additionalInventories);
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Resources, "GetResourceRecipeValidity", ret);
        if (s_logGetRecipeValidityCount++ < kMaxHookLogPerHook) {
            printf("[GATE] GetResourceRecipeValidity#%d caller=+0x%llX self=%p resource=%p req=%d addInv=%p orig=%d -> 0\n",
                s_logGetRecipeValidityCount, (unsigned long long)RelCaller(ret), self, resourceType, requiredAmount,
                additionalInventories, orig);
        }
        if (!s_dumpedGetRecipeValidityCtx) {
            s_dumpedGetRecipeValidityCtx = true;
            DumpCallerContext(ret, "GetResourceRecipeValidity");
        }
        // Observed validity-style helper: preserve the call for logging,
        // then force the permissive result only when FreeCraft is enabled.
        return 0;
    }
    return orig;
}

bool __fastcall HookHasWaterSourceConnection(void* self, bool mustBeActiveConnection) {
    void* ret = _ReturnAddress();
    bool orig = g_origHasWaterSourceConnection(self, mustBeActiveConnection);
    if (Trainer::Get().FreeCraft) {
        QueueProcessorKick(self);
        LogFreeCraftPath(FreeCraftPathKind::Processor, "HasWaterSourceConnection", ret);
        if (s_logHasWaterSourceCount++ < kMaxHookLogPerHook) {
            printf("[PROC] HasWaterSourceConnection#%d caller=+0x%llX self=%p mustActive=%d orig=%d -> 1\n",
                s_logHasWaterSourceCount, (unsigned long long)RelCaller(ret), self,
                (int)mustBeActiveConnection, (int)orig);
        }
        return true;
    }
    return orig;
}

void __fastcall HookServerStartProcessing(void* self) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Processor, "OnServer_StartProcessing", ret);
        if (s_logStartProcessingCount++ < kMaxHookLogPerHook) {
            printf("[PROC] OnServer_StartProcessing#%d caller=+0x%llX self=%p\n",
                s_logStartProcessingCount, (unsigned long long)RelCaller(ret), self);
        }
    }
    g_origServerStartProcessing(self);
}

void __fastcall HookServerActivateProcessor(void* self) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Processor, "OnServer_ActivateProcessor", ret);
        if (s_logActivateProcessorCount++ < kMaxHookLogPerHook) {
            printf("[PROC] OnServer_ActivateProcessor#%d caller=+0x%llX self=%p\n",
                s_logActivateProcessorCount, (unsigned long long)RelCaller(ret), self);
        }
    }
    g_origServerActivateProcessor(self);
}

void InstallCraftValidationHooks(uintptr_t base, size_t sz) {
    if (s_craftHooksInstalled) return;
    s_moduleBase = base;

    uintptr_t csriAddr =
        ResolveNativeOrAob("ProcessingComponent", "CanSatisfyRecipeInput",
                           base, sz, kCanSatisfyRecipeInputAob, "CanSatisfyRecipeInput");
    uintptr_t cqiAddr =
        ResolveNativeOrAob("ProcessingComponent", "CanQueueItem",
                           base, sz, kCanQueueItemAob, "CanQueueItem");
    uintptr_t maxStackAddr =
        ResolveNativeOrAob("ProcessingComponent", "GetMaxCraftableStack",
                           base, sz, kGetMaxCraftableStackAob, "GetMaxCraftableStack");
    uintptr_t consumeAddr =
        ResolveNativeOrAob("Inventory", "ConsumeItem",
                           base, sz, kConsumeItemEntryAob, "ConsumeItem");
    uintptr_t getResourceRecipeValidityAddr =
        ResolveNativeOrAob("ProcessingComponent", "GetResourceRecipeValidity",
                           base, sz, nullptr, "GetResourceRecipeValidity");
    uintptr_t hasSufficientResourceAddr =
        ResolveNativeOrAob("ProcessingComponent", "HasSufficientResource",
                           base, sz, nullptr, "HasSufficientResource");
    uintptr_t hasWaterSourceConnectionAddr =
        ResolveNativeOnly("ProcessingComponent", "HasWaterSourceConnection", "HasWaterSourceConnection");
    uintptr_t serverStartProcessingAddr =
        ResolveNativeOnly("ProcessingComponent", "OnServer_StartProcessing", "OnServer_StartProcessing");
    uintptr_t serverActivateProcessorAddr =
        ResolveNativeOnly("ProcessingComponent", "OnServer_ActivateProcessor", "OnServer_ActivateProcessor");

#if 0

    static const uint8_t prefixCSRI[]        = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x08, 0x49 };
    static const uint8_t prefixCQI[]         = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48 };
    static const uint8_t prefixGetMaxStack[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44 };

    // Three-tier resolver:
    //   1. UObjectLookup name-based (patch-proof — works across game updates)
    //   2. AOB pattern scan on the C++ impl prologue (game update tolerant)
    //   3. PDB offset with prefix check (fast, breaks on every game patch)
    auto resolve = [&](const char* className, const char* fnName,
                       uintptr_t pdbOff, const uint8_t* prefix, size_t prefixLen,
                       const char* aob, const char* logName) -> uintptr_t {
        // Tier 1: UObject name lookup (resolves thunk→impl via HDE64 walker)
        if (UObjectLookup::IsInitialized()) {
            uintptr_t addr = UObjectLookup::FindNativeFunction(className, fnName);
            if (addr) {
                printf("[HOOK] %s: name lookup -> 0x%p\n", logName, (void*)addr);
                return addr;
            }
        }
        // Tier 2: AOB pattern scan
        uintptr_t hit = UE4::PatternScan(base, sz, aob);
        if (hit) {
            printf("[HOOK] %s: AOB scan -> 0x%p\n", logName, (void*)hit);
            return hit;
        }
        // Tier 3: PDB offset (last resort)
        uintptr_t cand = base + pdbOff;
        if (MatchPrefix(cand, prefix, prefixLen)) {
            printf("[HOOK] %s: PDB offset -> 0x%p\n", logName, (void*)cand);
            return cand;
        }
        printf("[HOOK] %s: ALL RESOLVERS FAILED — hook skipped\n", logName);
        return 0;
    };

    uintptr_t csriAddr    = resolve("ProcessingComponent", "CanSatisfyRecipeInput",
                                    kCanSatisfyRecipeInputOffset, prefixCSRI, sizeof(prefixCSRI),
                                    kCanSatisfyRecipeInputAob, "CanSatisfyRecipeInput");
    uintptr_t cqiAddr     = resolve("ProcessingComponent", "CanQueueItem",
                                    kCanQueueItemOffset, prefixCQI, sizeof(prefixCQI),
                                    kCanQueueItemAob, "CanQueueItem");
    uintptr_t maxStackAddr= resolve("ProcessingComponent", "GetMaxCraftableStack",
                                    kGetMaxCraftableStackOffset, prefixGetMaxStack, sizeof(prefixGetMaxStack),
                                    kGetMaxCraftableStackAob, "GetMaxCraftableStack");
    uintptr_t consumeAddr = resolve("Inventory", "ConsumeItem",
                                    kConsumeItemFnOffset, nullptr, 0,
                                    kConsumeItemEntryAob, "ConsumeItem");
    // ConsumeItem prologue check is special — fall back if name lookup didn't work
    #endif
    if (consumeAddr) {
        static const uint8_t prefixConsume[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C };
        if (!MatchPrefix(consumeAddr, prefixConsume, sizeof(prefixConsume))) {
            uint8_t* p = reinterpret_cast<uint8_t*>(consumeAddr);
            printf("[HOOK] ConsumeItem: WARN prologue mismatch (%02X %02X %02X %02X %02X %02X %02X %02X)\n",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        printf("[HOOK] MH_Initialize failed: %d\n", (int)init);
        return;
    }

    auto install = [](uintptr_t addr, void* detour, void** orig, const char* name) {
        if (!addr) return;
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(addr), detour, orig);
        if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
            printf("[HOOK] %s create failed: %d\n", name, (int)s);
            return;
        }
        s = MH_EnableHook(reinterpret_cast<void*>(addr));
        if (s != MH_OK && s != MH_ERROR_ENABLED) {
            printf("[HOOK] %s enable failed: %d\n", name, (int)s);
            return;
        }
        printf("[HOOK] %s installed at 0x%p\n", name, reinterpret_cast<void*>(addr));
    };

    install(csriAddr,     reinterpret_cast<void*>(&HookCanSatisfyRecipeInput), reinterpret_cast<void**>(&g_origCanSatisfyRecipeInput), "CanSatisfyRecipeInput");
    install(cqiAddr,      reinterpret_cast<void*>(&HookCanQueueItem),          reinterpret_cast<void**>(&g_origCanQueueItem),          "CanQueueItem");
    install(maxStackAddr, reinterpret_cast<void*>(&HookGetMaxCraftableStack),  reinterpret_cast<void**>(&g_origGetMaxCraftableStack),  "GetMaxCraftableStack");
    install(consumeAddr,  reinterpret_cast<void*>(&HookConsumeItem),           reinterpret_cast<void**>(&g_origConsumeItem),           "ConsumeItem");
    if (csriAddr || cqiAddr || maxStackAddr || consumeAddr) {
        printf("[FREECRAFT] Item path hooks armed\n");
    }
    install(getResourceRecipeValidityAddr, reinterpret_cast<void*>(&HookGetResourceRecipeValidity),
            reinterpret_cast<void**>(&g_origGetResourceRecipeValidity), "GetResourceRecipeValidity");
    install(hasSufficientResourceAddr, reinterpret_cast<void*>(&HookHasSufficientResource),
            reinterpret_cast<void**>(&g_origHasSufficientResource), "HasSufficientResource");
    install(hasWaterSourceConnectionAddr, reinterpret_cast<void*>(&HookHasWaterSourceConnection),
            reinterpret_cast<void**>(&g_origHasWaterSourceConnection), "HasWaterSourceConnection");
    install(serverStartProcessingAddr, reinterpret_cast<void*>(&HookServerStartProcessing),
            reinterpret_cast<void**>(&g_origServerStartProcessing), "OnServer_StartProcessing");
    install(serverActivateProcessorAddr, reinterpret_cast<void*>(&HookServerActivateProcessor),
            reinterpret_cast<void**>(&g_origServerActivateProcessor), "OnServer_ActivateProcessor");
    if (getResourceRecipeValidityAddr || hasSufficientResourceAddr) {
        printf("[FREECRAFT] Resource path hooks armed\n");
    }
    if (hasWaterSourceConnectionAddr || serverStartProcessingAddr || serverActivateProcessorAddr) {
        printf("[FREECRAFT] Processor runtime hooks armed\n");
    }

    s_craftHooksInstalled = true;
}

}

// =============================================================================
// ResolveAllOffsets — replaces every Off:: namespace constant with the value
// resolved at runtime via UObjectLookup. Each property is looked up by its UE
// reflection name, walking the parent class chain if needed. Unresolved
// properties stay at 0 and the corresponding feature path is skipped safely.
//
// This makes ALL field offsets patch-proof across game updates, the same way
// FindNativeFunction makes function addresses patch-proof.
// =============================================================================
void Trainer::ResolveAllOffsets() {
    auto resolve = [](uintptr_t& slot, const char* className, const char* propName) {
        int32_t off = UObjectLookup::FindPropertyOffset(className, propName);
        if (off >= 0) {
            slot = static_cast<uintptr_t>(off);
        } else {
            printf("[INIT] WARN unresolved property %s::%s\n", className, propName);
        }
    };

    printf("[INIT] === Resolving UPROPERTY offsets via name lookup ===\n");

    // --- Engine / UWorld / UGameInstance / UPlayer / AController ---
    // The player-chain scan walks an engine-global pointer whose object
    // exposes GameInstance at UGameEngine::GameInstance (+0xD28), not the
    // reflected UWorld::OwningGameInstance field.
    resolve(Off::World_GameInstance,        "GameEngine",   "GameInstance");
    resolve(Off::World_GameState,           "World",        "GameState");
    resolve(Off::GI_LocalPlayers,           "GameInstance", "LocalPlayers");
    resolve(Off::Player_Controller,         "Player",       "PlayerController");
    resolve(Off::Ctrl_Character,            "Controller",   "Character");
    resolve(Off::Char_MovementComp,         "Character",    "CharacterMovement");

    // --- AActor / AIcarusCharacter / UModifierStateComponent ---
    resolve(Off::Char_InstanceComponents,   "Actor",                "InstanceComponents");
    resolve(Off::Char_BlueprintComponents,  "Actor",                "BlueprintCreatedComponents");
    resolve(Off::Actor_CustomTimeDilation,  "Actor",                "CustomTimeDilation");
    resolve(Off::Char_ActorState,           "IcarusCharacter",      "ActorState");
    resolve(Off::Player_InventoryComp,      "IcarusPlayerCharacter","InventoryComponent");
    resolve(Off::Mod_Lifetime,              "ModifierStateComponent","ModifierLifetime");
    resolve(Off::Mod_Remaining,             "ModifierStateComponent","RemainingTime");

    // --- UActorState (base of all character states) ---
    resolve(Off::State_Health,      "ActorState", "Health");
    resolve(Off::State_MaxHealth,   "ActorState", "MaxHealth");
    resolve(Off::State_Armor,       "ActorState", "Armor");
    resolve(Off::State_MaxArmor,    "ActorState", "MaxArmor");
    resolve(Off::State_AliveState,  "ActorState", "CurrentAliveState");

    // --- UCharacterState (extends UActorState) ---
    resolve(Off::State_Stamina,     "CharacterState", "Stamina");
    resolve(Off::State_MaxStamina,  "CharacterState", "MaxStamina");

    // --- USurvivalCharacterState (extends UCharacterState) ---
    resolve(Off::State_Oxygen,      "SurvivalCharacterState", "OxygenLevel");
    resolve(Off::State_MaxOxygen,   "SurvivalCharacterState", "MaxOxygen");
    resolve(Off::State_Water,       "SurvivalCharacterState", "WaterLevel");
    resolve(Off::State_MaxWater,    "SurvivalCharacterState", "MaxWater");
    resolve(Off::State_Food,        "SurvivalCharacterState", "FoodLevel");
    resolve(Off::State_MaxFood,     "SurvivalCharacterState", "MaxFood");

    // --- UCharacterMovementComponent ---
    resolve(Off::CMC_MaxWalkSpeed,         "CharacterMovementComponent", "MaxWalkSpeed");
    resolve(Off::CMC_MaxWalkSpeedCrouched, "CharacterMovementComponent", "MaxWalkSpeedCrouched");
    resolve(Off::CMC_MaxSwimSpeed,         "CharacterMovementComponent", "MaxSwimSpeed");
    resolve(Off::CMC_MaxFlySpeed,          "CharacterMovementComponent", "MaxFlySpeed");

    // --- AIcarusGameStateSurvival (extends AGameStateBase) ---
    resolve(Off::GS_TimeOfDay,             "IcarusGameStateSurvival", "TimeOfDay");
    resolve(Off::GS_SecondsPerGameDay,     "IcarusGameStateSurvival", "SecondsPerGameDay");
    resolve(Off::GS_ReplicatedLastSessionProspectGameTime,
                                           "IcarusGameStateSurvival", "ReplicatedLastSessionProspectGameTime");

    // --- UInventory ---
    resolve(Off::Inv_CurrentWeight,        "Inventory", "CurrentWeight");
    resolve(Off::Inv_Slots,                "Inventory", "Slots");

    printf("[INIT] === Offset resolution complete ===\n");
}

void Trainer::Initialize() {
    AllocConsole();
    SetConsoleTitleW(L"ZeusMod");
    freopen_s(&m_con, "CONOUT$", "w", stdout);
    printf("=== ZeusMod Internal ===\n\n");
    StartPipeServer();

    // Bring up the UE4 reflection-based name lookup BEFORE FindPlayer.
    // The thunk-to-impl resolver uses HDE64 to walk thunk bytecode, identify
    // the result-buffer register from the prologue, and find the CALL that
    // precedes a write to [resultReg] — that's the C++ impl. This makes the
    // entire resolution chain patch-proof across game updates.
    if (UObjectLookup::Initialize()) {
        printf("[INIT] UObjectLookup ready (name-based resolution active)\n");
        ResolveAllOffsets();
    } else {
        printf("[INIT] UObjectLookup failed — falling back to hardcoded offsets\n");
    }

    FindPlayer();
}

void Trainer::Shutdown() {
    PatchSetHealth(false);
    PatchRemoveItem(false);
    PatchFreeCraftItems(false);
    PatchFreeCraftProcessorGates(false);
    PatchWeight(false);
    // MinHook lifecycle is owned by Render.cpp (DX12 overlay hooks).
    printf("[EXIT] Shutdown.\n");
    if (m_con) { fclose(m_con); FreeConsole(); }
}

void Trainer::FindPlayer() {
    // Find SetHealth and dump the actual bytes to see what we're patching
    if (!m_setHealthAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz, kSetHealthWriteAob);
        if (match) {
            // Dump bytes to verify what we're patching
            printf("[PATCH] Pattern found at 0x%p\n", (void*)match);
            printf("[PATCH] Bytes: ");
            uint8_t* p = reinterpret_cast<uint8_t*>(match);
            for (int i = 0; i < 22; i++) printf("%02X ", p[i]);
            printf("\n");

            // Find the exact position of "89 XX D8 01 00 00" within the match
            for (int i = 0; i < 20; i++) {
                if (p[i] == 0x89 && p[i+2] == 0xD8 && p[i+3] == 0x01 && p[i+4] == 0x00 && p[i+5] == 0x00) {
                    m_setHealthAddr = match + i;
                    printf("[PATCH] Found health write at offset +%d -> 0x%p\n", i, (void*)m_setHealthAddr);
                    break;
                }
            }
            if (!m_setHealthAddr) {
                printf("[PATCH] Could not find 89 XX D8 01 00 00 in pattern!\n");
            }
        } else {
            printf("[PATCH] SetHealth AOB not found!\n");
        }
    }

    if (!Off::World_GameInstance || !Off::GI_LocalPlayers || !Off::Player_Controller ||
        !Off::Ctrl_Character || !Off::Char_ActorState || !Off::State_Health ||
        !Off::State_MaxHealth || !Off::State_Stamina || !Off::State_MaxStamina) {
        static bool loggedMissingCore = false;
        if (!loggedMissingCore) {
            loggedMissingCore = true;
            printf("[FIND] Core runtime offsets unresolved, player scan skipped\n");
        }
        return;
    }

    // Craft cost functions — three-tier resolution:
    //   1. UObjectLookup name-based (patch-proof — uses HDE64 thunk walker)
    //   2. AOB pattern scan on the C++ impl prologue (game-update tolerant)
    //   3. PDB offset with prefix check (fast path, breaks on every patch)
    auto resolveCraftFn = [&](uintptr_t& out, const char* className, const char* fnName,
                              const char* aob, const char* logName,
                              uintptr_t modBase, size_t modSz) {
        if (out) return;
        out = ResolveNativeOrAob(className, fnName, modBase, modSz, aob, logName);
    };

    if (!m_scaledInputAddr || !m_scaledResourceAddr || !m_findItemCountAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);

        // Likely material-gating functions for FreeCraft. These are resolved
        // for reverse-engineering visibility even if we don't patch them yet.
        if (!s_getResourceRecipeValidityAddr) {
            s_getResourceRecipeValidityAddr =
                ResolveNativeOrAob("ProcessingComponent", "GetResourceRecipeValidity",
                                   b, sz, nullptr, "GetResourceRecipeValidity");
        }
        if (!s_hasSufficientResourceAddr) {
            s_hasSufficientResourceAddr =
                ResolveNativeOrAob("ProcessingComponent", "HasSufficientResource",
                                   b, sz, nullptr, "HasSufficientResource");
        }
        if (s_getResourceRecipeValidityAddr || s_hasSufficientResourceAddr) {
            printf("[GATE] Material gate candidate: GetResourceRecipeValidity=%p HasSufficientResource=%p\n",
                reinterpret_cast<void*>(s_getResourceRecipeValidityAddr),
                reinterpret_cast<void*>(s_hasSufficientResourceAddr));
        }

        resolveCraftFn(m_scaledInputAddr,   "CraftingFunctionLibrary", "GetScaledRecipeInputCount",
                       kScaledInputAob,    "GetScaledRecipeInputCount",        b, sz);
        resolveCraftFn(m_scaledResourceAddr,"CraftingFunctionLibrary", "GetScaledRecipeResourceItemCount",
                       kScaledResourceAob, "GetScaledRecipeResourceItemCount", b, sz);
        resolveCraftFn(m_findItemCountAddr, "Inventory", "FindItemCountByType",
                       kFindItemCountAob,  "FindItemCountByType",              b, sz);

        resolveCraftFn(m_canSatisfyQueryAddr, "ProcessingComponent", "CanSatisfyRecipeQueryInput",
                       kCanSatisfyRecipeQueryInputAob, "CanSatisfyRecipeQueryInput", b, sz);
        resolveCraftFn(m_getItemCountAddr, "Inventory", "GetItemCount",
                       kGetItemCountAob, "GetItemCount", b, sz);

        InstallCraftValidationHooks(b, sz);
    }


    // Find UInventory::ConsumeItem - CE confirmed AOB
    // cmp rdi,rcx; jne; sub [rsi+04],r12d; jmp
    // The sub [rsi+04],r12d (44 29 66 04) is what decrements item count
    if (!m_removeItemAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz, kConsumeItemAob);
        if (match) {
            m_removeItemAddr = match + 5; // points to "44 29 66 04"
            printf("[PATCH] ConsumeItem sub at 0x%p\n", (void*)m_removeItemAddr);
        } else {
            printf("[PATCH] ConsumeItem AOB not found\n");
        }
    }

    // (DIAG block removed — those addresses were UFunction thunks, not real
    // enclosing functions. We now resolve impls via UObjectLookup.)

    printf("[FIND] Scanning for player...\n");

    uintptr_t base; size_t size;
    UE4::GetModuleInfo(base, size);

    const char* pat = "48 8B 1D ?? ?? ?? ?? 48 85 DB 74";
    uintptr_t scanPos = base;
    size_t remain = size;
    int candidates = 0;

    while (remain > 64) {
        uintptr_t hit = UE4::PatternScan(scanPos, remain, pat);
        if (!hit) break;

        uintptr_t gwPtr = UE4::ResolveRIP(hit);

        __try {
            void* world = *(void**)gwPtr;
            if (!world) goto next;

            void* gi = ReadAt<void*>(world, Off::World_GameInstance);
            if (!gi) goto next;

            // TArray<ULocalPlayer*>: data pointer at +0, count at +8
            void** lpData = ReadAt<void**>(gi, Off::GI_LocalPlayers);
            int lpCount = ReadAt<int>(gi, Off::GI_LocalPlayers + 8);
            if (!lpData || lpCount <= 0) goto next;

            void* lp = lpData[0];
            if (!lp) goto next;

            void* pc = ReadAt<void*>(lp, Off::Player_Controller);
            if (!pc) goto next;

            void* ch = ReadAt<void*>(pc, Off::Ctrl_Character);
            if (!ch) goto next;

            void* as = ReadAt<void*>(ch, Off::Char_ActorState);
            if (!as) goto next;

            // Validate
            int hp = ReadAt<int>(as, Off::State_Health);
            int maxHp = ReadAt<int>(as, Off::State_MaxHealth);
            int sta = ReadAt<int>(as, Off::State_Stamina);
            int maxSta = ReadAt<int>(as, Off::State_MaxStamina);

            if (hp > 0 && maxHp >= 50 && maxHp <= 5000 && hp <= maxHp &&
                sta > 0 && maxSta >= 50 && maxSta <= 5000 && sta <= maxSta) {

                m_gworldPtr = (void**)gwPtr;
                m_character = ch;
                m_actorState = as;

                printf("[FOUND] GWorld #%d\n", candidates);
                printf("  Character:  0x%p\n", ch);
                printf("  ActorState: 0x%p\n", as);
                printf("  Health: %d/%d  Stamina: %d/%d\n", hp, maxHp, sta, maxSta);
                printf("  Armor: %d/%d\n",
                    ReadAt<int>(as, Off::State_Armor),
                    ReadAt<int>(as, Off::State_MaxArmor));
                return;
            }

            if (candidates < 5) {
                printf("  #%d: HP=%d/%d STA=%d/%d (rejected)\n", candidates, hp, maxHp, sta, maxSta);
            }
        }
        __except (1) {}

        next:
        candidates++;
        scanPos = hit + 1;
        remain = (base + size) - scanPos;
    }

    printf("[FIND] Not found (%d scanned). Retry in 2s.\n", candidates);
}

void Trainer::Tick() {
    if (!Off::State_Health || !Off::State_MaxHealth) return;

    // Re-find if lost
    if (!m_actorState) {
        if (m_retryTimer++ % 60 == 0) FindPlayer();
        return;
    }

    // Validate still valid
    __try {
        int hp = ReadAt<int>(m_actorState, Off::State_Health);
        int maxHp = ReadAt<int>(m_actorState, Off::State_MaxHealth);
        if (maxHp <= 0 || maxHp > 10000) {
            printf("[LOST] ActorState invalid, re-scanning...\n");
            m_actorState = nullptr;
            m_character = nullptr;
            return;
        }
    }
    __except (1) {
        m_actorState = nullptr;
        m_character = nullptr;
        return;
    }

    m_retryTimer = 0;

    // Apply cheats
    __try {
        if (GodMode && Off::State_MaxHealth && Off::State_Health && Off::State_AliveState) {
            // Patch SetHealth write instruction with NOP
            PatchSetHealth(true);
            // Verify patch is still in place (game might restore it)
            if (m_setHealthPatched && m_setHealthAddr) {
                uint8_t check = *reinterpret_cast<uint8_t*>(m_setHealthAddr);
                if (check != 0x90) {
                    // Patch was undone! Re-apply
                    printf("[PATCH] Health patch was restored by game! Re-patching...\n");
                    m_setHealthPatched = false;
                    PatchSetHealth(true);
                }
            }
            // Also keep health at max
            int maxHp = ReadAt<int>(m_actorState, Off::State_MaxHealth);
            WriteAt<int>(m_actorState, Off::State_Health, maxHp);
            WriteAt<uint8_t>(m_actorState, Off::State_AliveState, 0);
            RemoveDebuffs();
        } else {
            PatchSetHealth(false);
        }

        if (InfiniteStamina && Off::State_MaxStamina && Off::State_Stamina) {
            int maxSta = ReadAt<int>(m_actorState, Off::State_MaxStamina);
            WriteAt<int>(m_actorState, Off::State_Stamina, maxSta);
        }

        if (InfiniteArmor && Off::State_MaxArmor && Off::State_Armor) {
            int maxArmor = ReadAt<int>(m_actorState, Off::State_MaxArmor);
            int curArmor = ReadAt<int>(m_actorState, Off::State_Armor);
            static bool armorLogged = false;
            if (!armorLogged) {
                armorLogged = true;
                printf("[ARMOR] First tick - cur=%d max=%d\n", curArmor, maxArmor);
                if (maxArmor <= 0) {
                    printf("[ARMOR] MaxArmor is 0 - equip armor first or it has nothing to fill\n");
                }
            }
            if (maxArmor > 0) {
                WriteAt<int>(m_actorState, Off::State_Armor, maxArmor);
            }
        } else {
            // Reset log flag so next enable re-prints diagnostic
            // (no-op here, just allows re-toggling)
        }

        if (InfiniteOxygen && Off::State_MaxOxygen && Off::State_Oxygen) {
            int maxO2 = ReadAt<int>(m_actorState, Off::State_MaxOxygen);
            WriteAt<int>(m_actorState, Off::State_Oxygen, maxO2);
        }

        if (InfiniteFood && Off::State_MaxFood && Off::State_Food) {
            int maxFood = ReadAt<int>(m_actorState, Off::State_MaxFood);
            WriteAt<int>(m_actorState, Off::State_Food, maxFood);
        }

        if (InfiniteWater && Off::State_MaxWater && Off::State_Water) {
            int maxWater = ReadAt<int>(m_actorState, Off::State_MaxWater);
            WriteAt<int>(m_actorState, Off::State_Water, maxWater);
        }

        // Speed hack via CustomTimeDilation on the player actor
        // This accelerates everything: movement, animations, actions
        if (SpeedHack && Off::Actor_CustomTimeDilation) {
            WriteAt<float>(m_character, Off::Actor_CustomTimeDilation, SpeedMultiplier);
        } else if (Off::Actor_CustomTimeDilation) {
            // Restore to normal (1.0)
            float current = ReadAt<float>(m_character, Off::Actor_CustomTimeDilation);
            if (current != 1.0f) {
                WriteAt<float>(m_character, Off::Actor_CustomTimeDilation, 1.0f);
            }
        }

        // Time lock state change logging (outside __try so we always see transitions)
        {
            static bool wasTimeLocked = false;
            if (TimeLock && !wasTimeLocked) {
                wasTimeLocked = true;
                printf("[TIME] TimeLock ENABLED, target=%.2f, m_gworldPtr=%p\n", LockedTime, m_gworldPtr);
            } else if (!TimeLock && wasTimeLocked) {
                wasTimeLocked = false;
                printf("[TIME] TimeLock DISABLED\n");
            }
        }

        // Time Lock — dedicated GWorld scan using the AOB that the validated
        // CE table uses. The player-chain m_gworldPtr points to a different
        // global, but this AOB resolves the real UWorld* symbol used by
        // the GameState path.
        //
        // CE AOB (from TimeOfDayIcarus.CT):
        //   89 7C 24 ?? 55 48 8B EC 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 8B D9 ...
        // The RIP-relative instruction is `48 8B 05 ?? ?? ?? ??` at byte
        // offset 12 inside the match (3-byte opcode + 4-byte disp32, 7 total).
        if (TimeLock && Off::World_GameState && Off::GS_SecondsPerGameDay &&
            Off::GS_TimeOfDay && Off::GS_ReplicatedLastSessionProspectGameTime) {
            static void* const* cachedRealGWorld = nullptr;
            static bool scanAttempted = false;

            if (!scanAttempted) {
                scanAttempted = true;
                uintptr_t base; size_t sz;
                UE4::GetModuleInfo(base, sz);
                const char* realGWorldAob =
                    "89 7C 24 ?? 55 48 8B EC 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 8D 4D 10 48 8B 50 18 48";
                uintptr_t hit = UE4::PatternScan(base, sz, realGWorldAob);
                if (hit) {
                    // Instruction `48 8B 05 disp32` starts at hit + 12, length 7.
                    uintptr_t gworldAddr = UE4::ResolveRIP(hit + 12, /*opcodeLen*/3, /*instrLen*/7);
                    cachedRealGWorld = reinterpret_cast<void* const*>(gworldAddr);
                    printf("[TIME] Real GWorld symbol resolved at %p (AOB hit=%p)\n",
                        cachedRealGWorld, (void*)hit);
                } else {
                    printf("[TIME] ERROR: real GWorld AOB not found\n");
                }
            }

            if (cachedRealGWorld) {
                __try {
                    void* world = *cachedRealGWorld;
                    if (world) {
                        void* gameState = ReadAt<void*>(world, Off::World_GameState);
                        if (gameState) {
                            int secondsPerDay = ReadAt<int>(gameState, Off::GS_SecondsPerGameDay);
                            if (secondsPerDay <= 0) secondsPerDay = 3600;

                            float hour24 = NormalizeLockedHour(LockedTime);
                            float timeValue = (hour24 / 24.0f) * static_cast<float>(secondsPerDay);

                            WriteAt<float>(gameState, Off::GS_TimeOfDay, timeValue);
                            WriteAt<float>(gameState, Off::GS_ReplicatedLastSessionProspectGameTime, timeValue);

                            static void* lastLoggedGS = nullptr;
                            static float lastLoggedHour = -999.0f;
                            if (lastLoggedGS != gameState || lastLoggedHour != hour24) {
                                lastLoggedGS = gameState;
                                lastLoggedHour = hour24;
                                printf("[TIME] world=%p gs=%p spd=%d hour=%.2f -> %.2f\n",
                                    world, gameState, secondsPerDay, hour24, timeValue);
                            }
                        } else {
                            static bool loggedNullGS = false;
                            if (!loggedNullGS) {
                                loggedNullGS = true;
                                printf("[TIME] ERROR: GameState null at world+0x%llX (not in prospect?)\n",
                                    (unsigned long long)Off::World_GameState);
                            }
                        }
                    } else {
                        static bool loggedNullWorld = false;
                        if (!loggedNullWorld) {
                            loggedNullWorld = true;
                            printf("[TIME] ERROR: real GWorld is null\n");
                        }
                    }
                } __except(1) {
                    static bool loggedEx = false;
                    if (!loggedEx) {
                        loggedEx = true;
                        printf("[TIME] Exception in Time Lock tick\n");
                    }
                }
            }
        }

        // No weight - patch GetTotalWeight to return 0
        if (NoWeight) {
            PatchWeight(true);
        } else {
            PatchWeight(false);
        }

        if (FreeCraft != m_prevFreeCraft) {
            ResetFreeCraftTelemetry();
            printf("[FREECRAFT] %s\n", FreeCraft ? "enabled" : "disabled");
            m_prevFreeCraft = FreeCraft;
        }

        // FreeCraft item path:
        //   GetScaledRecipeInputCount / GetScaledRecipeResourceItemCount
        //   FindItemCountByType / GetItemCount / CanSatisfyRecipeQueryInput
        //   ConsumeItem inline SUB + entry hook
        //
        // FreeCraft resource path:
        //   GetResourceRecipeValidity / HasSufficientResource
        //   handled by dedicated hooks above, only when that category is used.
        if (FreeCraft) {
            PatchRemoveItem(true);
            PatchFreeCraftItems(true);
            PatchFreeCraftProcessorGates(true);

            if (s_pendingProcessorKickSelf && g_origServerActivateProcessor && g_origServerStartProcessing) {
                ULONGLONG now = GetTickCount64();
                if (s_pendingProcessorKickSelf != s_lastKickedProcessorSelf || (now - s_lastProcessorKickTick) > 500) {
                    void* target = s_pendingProcessorKickSelf;
                    s_lastKickedProcessorSelf = target;
                    s_lastProcessorKickTick = now;
                    printf("[PROC] ForceStart target=%p\n", target);
                    g_origServerActivateProcessor(target);
                    g_origServerStartProcessing(target);
                }
            }
        } else {
            PatchRemoveItem(false);
            PatchFreeCraftItems(false);
            PatchFreeCraftProcessorGates(false);
        }
    }
    __except (1) {
        m_actorState = nullptr;
        m_character = nullptr;
    }
}

void Trainer::RemoveDebuffs() {
    if (!m_character || !Off::Char_BlueprintComponents ||
        !Off::Mod_Lifetime || !Off::Mod_Remaining) return;

    __try {
        // Read BlueprintCreatedComponents TArray
        void** compData = ReadAt<void**>(m_character, Off::Char_BlueprintComponents);
        int compCount = ReadAt<int>(m_character, Off::Char_BlueprintComponents + 8);
        if (!compData || compCount <= 0 || compCount > 500) return;

        for (int i = 0; i < compCount; i++) {
            void* comp = compData[i];
            if (!comp) continue;

            __try {
                float lifetime = ReadAt<float>(comp, Off::Mod_Lifetime);
                float remaining = ReadAt<float>(comp, Off::Mod_Remaining);

                if (lifetime > 0.0f && lifetime < 100000.0f &&
                    remaining > 0.0f && remaining <= lifetime + 1.0f) {
                    WriteAt<float>(comp, Off::Mod_Remaining, 0.0f);
                }
            }
            __except (1) {}
        }
    }
    __except (1) {}
}

void Trainer::PatchSetHealth(bool enable) {
    if (!m_setHealthAddr) return;

    if (enable && !m_setHealthPatched) {
        // Verify bytes are correct before patching (must be 89 XX D8 01 00 00)
        uint8_t* check = reinterpret_cast<uint8_t*>(m_setHealthAddr);
        if (check[0] != 0x89 || check[2] != 0xD8 || check[3] != 0x01) {
            // Already NOPed or wrong address
            if (check[0] == 0x90) {
                m_setHealthPatched = true; // Already patched
                return;
            }
            printf("[PATCH] SetHealth bytes mismatch: %02X %02X %02X - skipping\n",
                check[0], check[1], check[2]);
            return;
        }
        // Save 6 original bytes and NOP the write instruction
        memcpy(m_setHealthBackup, reinterpret_cast<void*>(m_setHealthAddr), 6);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, PAGE_EXECUTE_READWRITE, &oldP);
        memset(reinterpret_cast<void*>(m_setHealthAddr), 0x90, 6); // 6x NOP
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, oldP, &oldP);
        m_setHealthPatched = true;
        printf("[PATCH] Health write NOPed (god mode ON)\n");
    }
    else if (!enable && m_setHealthPatched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(m_setHealthAddr), m_setHealthBackup, 6);
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, oldP, &oldP);
        m_setHealthPatched = false;
        printf("[PATCH] Health write restored (god mode OFF)\n");
    }
}

// Old PatchFunc removed - replaced by PatchBytes

void Trainer::PatchBytes(uintptr_t addr, const uint8_t* patch, uint8_t* backup, int size, bool enable, bool& patched, const char* name) {
    if (!addr) return;
    if (enable && !patched) {
        memcpy(backup, reinterpret_cast<void*>(addr), size);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(addr), patch, size);
        VirtualProtect(reinterpret_cast<void*>(addr), size, oldP, &oldP);
        patched = true;
        printf("[PATCH] %s patched\n", name);
    } else if (!enable && patched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(addr), backup, size);
        VirtualProtect(reinterpret_cast<void*>(addr), size, oldP, &oldP);
        patched = false;
        printf("[PATCH] %s restored\n", name);
    }
}

void Trainer::PatchFreeCraftItems(bool enable) {
    // 1. GetScaledRecipeInputCount        -> mov eax,1; ret
    //    (returning 0 makes the production at timer=100% bail because
     //     it sums input counts and rejects recipes with totalCost<=0)
    // 2. GetScaledRecipeResourceItemCount -> mov eax,1; ret
    // 3. FindItemCountByType              -> mov eax,9999; ret
    //
    // Combined with the ConsumeItem entry hook and the inline SUB NOP,
    // this gives infinite crafting as long as the player has >= 1 of
    // each required material (the slot search needs to find a slot to
    // consume from, after that our hooks keep the slot count stable).
    uint8_t retOne[6]  = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    uint8_t ret9999[6] = { 0xB8, 0x0F, 0x27, 0x00, 0x00, 0xC3 };
    uint8_t retTrue[3] = { 0xB0, 0x01, 0xC3 }; // mov al, 1; ret

    PatchBytes(m_scaledInputAddr,    retOne,  m_scaledInputBackup,    6, enable, m_scaledInputPatched,    "GetScaledRecipeInputCount");
    PatchBytes(m_scaledResourceAddr, retOne,  m_scaledResourceBackup, 6, enable, m_scaledResourcePatched, "GetScaledRecipeResourceItemCount");
    PatchBytes(m_findItemCountAddr,  ret9999, m_findItemCountBackup,  6, enable, m_findItemCountPatched,  "FindItemCountByType");
    PatchBytes(m_canSatisfyQueryAddr, retTrue, m_canSatisfyQueryBackup, 3, enable, m_canSatisfyQueryPatched, "CanSatisfyRecipeQueryInput");
    PatchBytes(m_getItemCountAddr,    ret9999, m_getItemCountBackup,    6, enable, m_getItemCountPatched,    "GetItemCount");
    // NOTE: EnclosingFn_CanSatisfyRecipeInput and EnclosingFn_CanQueueItem
    // were patched here in an earlier iteration but caused a crash in
    // FArrayProperty::CopyValuesInternal during Blueprint execution. Those
    // functions are called from the UE Blueprint VM and build complex
    // TArray results that the caller reads after the return — a naive
    // "mov [r8],1; ret" corrupts the result struct. Reverted, do not re-add.
}

void Trainer::PatchFreeCraftProcessorGates(bool enable) {
    if (!m_shelterRequirementsAddr || !m_canStartProcessingAddr) {
        if (!m_shelterRequirementsAddr) {
            m_shelterRequirementsAddr =
                ResolveNativeOnly("ProcessingComponent", "ShelterRequirementsMet",
                                  "ShelterRequirementsMet");
        }
        if (!m_canStartProcessingAddr) {
            m_canStartProcessingAddr =
                ResolveNativeOnly("ProcessingComponent", "CanStartProcessing",
                                  "CanStartProcessing");
        }
        if (m_shelterRequirementsAddr || m_canStartProcessingAddr) {
            printf("[FREECRAFT] Processor gate patches armed\n");
        }
    }

    // Force the startup/context checks to succeed. We intentionally do not
    // patch CanProcess here because it is polled in the processing tick path.
    uint8_t retTrue[3] = { 0xB0, 0x01, 0xC3 };
    PatchBytes(m_shelterRequirementsAddr, retTrue, m_shelterRequirementsBackup, 3,
               enable, m_shelterRequirementsPatched, "ShelterRequirementsMet");
    PatchBytes(m_canStartProcessingAddr, retTrue, m_canStartProcessingBackup, 3,
               enable, m_canStartProcessingPatched, "CanStartProcessing");
}

// CanQueueItem removed - using GetScaledRecipeInputCount instead

void Trainer::PatchRemoveItem(bool enable) {
    if (!m_removeItemAddr) return;
    if (enable && !m_removeItemPatched) {
        // Read and log current bytes
        uint8_t current[4];
        memcpy(current, reinterpret_cast<void*>(m_removeItemAddr), 4);
        printf("[PATCH] ConsumeItem at 0x%p: bytes = %02X %02X %02X %02X\n",
            (void*)m_removeItemAddr, current[0], current[1], current[2], current[3]);

        memcpy(m_removeItemBackup, current, 4);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, PAGE_EXECUTE_READWRITE, &oldP);
        memset(reinterpret_cast<void*>(m_removeItemAddr), 0x90, 4); // NOP 4 bytes
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, oldP, &oldP);

        // Verify
        memcpy(current, reinterpret_cast<void*>(m_removeItemAddr), 4);
        printf("[PATCH] After NOP: %02X %02X %02X %02X %s\n",
            current[0], current[1], current[2], current[3],
            (current[0] == 0x90) ? "(OK!)" : "(FAILED!)");

        m_removeItemPatched = true;
    } else if (!enable && m_removeItemPatched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(m_removeItemAddr), m_removeItemBackup, 4);
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, oldP, &oldP);
        m_removeItemPatched = false;
        printf("[PATCH] RemoveItem restored\n");
    }
}

void Trainer::PatchWeight(bool enable) {
    // GetTotalWeight - resolve the native entry at runtime, then patch it.
    if (!m_weightAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        m_weightAddr = ResolveNativeOrAob("InventoryComponent", "GetTotalWeight",
                                          b, sz, kGetTotalWeightAob, "GetTotalWeight");
        if (!m_weightAddr) {
            printf("[WEIGHT] GetTotalWeight runtime resolution failed\n");
        }
    }
    uint8_t retZero[3] = { 0x31, 0xC0, 0xC3 }; // xor eax,eax; ret (return 0)
    PatchBytes(m_weightAddr, retZero, m_weightBackup, 3, enable, m_weightPatched, "GetTotalWeight");
}

// ============================================================================
// Named Pipe Server — receives commands from Electron app
// ============================================================================

void Trainer::StartPipeServer() {
    CreateThread(nullptr, 0, PipeServerThread, this, 0, nullptr);
}

DWORD WINAPI Trainer::PipeServerThread(LPVOID param) {
    Trainer* self = static_cast<Trainer*>(param);
    printf("[PIPE] Server started on \\\\.\\pipe\\ZeusModPipe\n");

    while (true) {
        HANDLE pipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\ZeusModPipe",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 512, 512, 0, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }

        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[256];
            DWORD bytesRead;

            while (ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buf[bytesRead] = 0;
                char* sep = strchr(buf, ':');
                if (sep) {
                    *sep = 0;
                    const char* cmd = buf;
                    const char* val = sep + 1;
                    int v = atoi(val);
                    float fv = (float)atof(val);

                    if (strcmp(cmd, "godmode") == 0) self->GodMode = (v != 0);
                    else if (strcmp(cmd, "stamina") == 0) self->InfiniteStamina = (v != 0);
                    else if (strcmp(cmd, "armor") == 0) self->InfiniteArmor = (v != 0);
                    else if (strcmp(cmd, "oxygen") == 0) self->InfiniteOxygen = (v != 0);
                    else if (strcmp(cmd, "food") == 0) self->InfiniteFood = (v != 0);
                    else if (strcmp(cmd, "water") == 0) self->InfiniteWater = (v != 0);
                    else if (strcmp(cmd, "craft") == 0) self->FreeCraft = (v != 0);
                    else if (strcmp(cmd, "weight") == 0) self->NoWeight = (v != 0);
                    else if (strcmp(cmd, "speed") == 0) self->SpeedHack = (v != 0);
                    else if (strcmp(cmd, "speed_mult") == 0) self->SpeedMultiplier = fv;
                    else if (strcmp(cmd, "time") == 0) self->TimeLock = (v != 0);
                    else if (strcmp(cmd, "time_val") == 0) self->LockedTime = fv;

                    printf("[PIPE] %s = %s\n", cmd, val);
                    const char* ok = "OK";
                    DWORD written;
                    WriteFile(pipe, ok, 2, &written, nullptr);
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

void Trainer::TickGodModefast() {
    if (!m_actorState || !Off::State_Health || !Off::State_MaxHealth) return;
    __try {
        int maxHp = ReadAt<int>(m_actorState, Off::State_MaxHealth);
        if (maxHp > 0 && maxHp <= 10000) {
            WriteAt<int>(m_actorState, Off::State_Health, maxHp);
        }
    }
    __except (1) {}
}

// Status getters for overlay
int Trainer::GetHealth() const {
    if (!m_actorState || !Off::State_Health) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Health); } __except(1) { return 0; }
}
int Trainer::GetMaxHealth() const {
    if (!m_actorState || !Off::State_MaxHealth) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxHealth); } __except(1) { return 0; }
}
int Trainer::GetStamina() const {
    if (!m_actorState || !Off::State_Stamina) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Stamina); } __except(1) { return 0; }
}
int Trainer::GetMaxStamina() const {
    if (!m_actorState || !Off::State_MaxStamina) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxStamina); } __except(1) { return 0; }
}
int Trainer::GetArmor() const {
    if (!m_actorState || !Off::State_Armor) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Armor); } __except(1) { return 0; }
}
int Trainer::GetMaxArmor() const {
    if (!m_actorState || !Off::State_MaxArmor) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxArmor); } __except(1) { return 0; }
}
