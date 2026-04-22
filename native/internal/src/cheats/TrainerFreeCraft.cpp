#include "Trainer.h"
#include "UE4.h"
#include "UObjectLookup.h"
#include "Logger.h"
#include "MinHook.h"
#include "TrainerInternal.h"
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <string>

// ============================================================================
// Free-Craft subsystem
// ----------------------------------------------------------------------------
// Hook + telemetry + tick-subsystem patching that lets the player queue any
// recipe without consuming materials. All the file-static state for this
// feature lives inside the anonymous namespace below. Shared helpers and AOB
// constants live in TrainerInternal.h so the lifecycle code in Trainer.cpp
// can reuse them without duplicating.
// ============================================================================

namespace {

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
using FnCanStartProcessing = bool(__fastcall*)(void*);
using FnCanProcess = bool(__fastcall*)(void*);
// Generic 6-ptr signature used for FindItemCountByType / GetItemCount detours.
// The real functions take 4-6 args (inventory*, item type, flags, filters).
// x64 fastcall puts the first 4 in RCX/RDX/R8/R9 and spills the rest onto the
// stack; six void* is enough headroom that the trampoline call forwards every
// real argument cleanly — extra slots are ignored by the real function.
using FnInvCountQuery = int(__fastcall*)(void*, void*, void*, void*, void*, void*);
// Standard UE UHT thunk signature: void exec(UObject* Context, FFrame& Stack, void* Result).
// In x64 fastcall: rcx=Context, rdx=Stack, r8=Result (unused for void returns).
using FnUEThunk = void(__fastcall*)(void*, void*, void*);

static FnCanSatisfy           g_origCanSatisfyRecipeInput      = nullptr;
static FnCanQueueItem         g_origCanQueueItem               = nullptr;
static FnGetMaxCraftableStack g_origGetMaxCraftableStack       = nullptr;
static FnConsumeItem          g_origConsumeItem                = nullptr;
static FnHasSufficientResource g_origHasSufficientResource     = nullptr;
static FnGetResourceRecipeValidity g_origGetResourceRecipeValidity = nullptr;
static FnHasWaterSourceConnection g_origHasWaterSourceConnection = nullptr;
static FnServerStartProcessing g_origServerStartProcessing = nullptr;
static FnServerActivateProcessor g_origServerActivateProcessor = nullptr;
static FnCanStartProcessing   g_origCanStartProcessing         = nullptr;
static FnCanProcess           g_origCanProcess                 = nullptr;
static FnInvCountQuery        g_origFindItemCountByType        = nullptr;
static FnInvCountQuery        g_origGetItemCount               = nullptr;
static FnUEThunk              g_origAddProcessingRecipeThunk   = nullptr;
// Talent UI refresh hooks — these are UFunction thunks on
// TalentControllerComponent. Hooking them lets us clamp point fields on
// the Model right before the game propagates state to the UI widget.
static FnUEThunk              g_origTriggerModelStateRefresh   = nullptr;
static FnUEThunk              g_origBPForceRefresh             = nullptr;
static FnUEThunk              g_origOnModelViewChanged         = nullptr;
static FnUEThunk              g_origNativeModelStateChanged    = nullptr;
// Generic-return-typed probe signature for newly discovered candidates we
// want to observe (and possibly force) without knowing their exact return
// type. uintptr_t covers bool (low byte), int (low 32) and pointer (full)
// returns equivalently — we just read or override rax.
using FnGenericProbe = uintptr_t(__fastcall*)(void*, void*, void*, void*, void*, void*);
static FnGenericProbe         g_origInvHasItems                = nullptr;
static FnGenericProbe         g_origInvFindFirstItem           = nullptr;
static FnGenericProbe         g_origPCAddItem                  = nullptr;
static FnGenericProbe         g_origInvFindItemByType          = nullptr;
static FnGenericProbe         g_origInvFind                    = nullptr;
static FnGenericProbe         g_origInvHasValidItemInSlot      = nullptr;
static FnGenericProbe         g_origICFindItems                = nullptr;
static FnGenericProbe         g_origPCOnServerStopProcessing   = nullptr;
static FnGenericProbe         g_origInvFindItemCountByQuery    = nullptr;
static FnGenericProbe         g_origInvFindItemCountWithMatchingData = nullptr;
static FnGenericProbe         g_origInvFindItemsByType         = nullptr;
static FnGenericProbe         g_origInvFindItemsByQuery        = nullptr;
static FnGenericProbe         g_origInvAddItem                 = nullptr;
static FnGenericProbe         g_origICAddItem                  = nullptr;
static FnGenericProbe         g_origICCAddItem                 = nullptr; // InventoryContainerComponent::AddItem

// Craft-validation scope counter. Incremented on entry to the craft-side
// UFunction hooks (CanSatisfyRecipeInput / CanQueueItem / HasSufficientResource
// / GetResourceRecipeValidity), decremented on exit. The FindItemCountByType /
// GetItemCount detours only force 9999 when this depth > 0 — outside of that
// window (notably during UProcessingComponent::DoProcessInternal's output
// delivery) they call the original function so the destination inventory sees
// its real slot counts and can actually store the crafted item.
//
// thread_local is fine here: the game calls all craft UFunctions on the main
// game thread and our DLL is loaded before any game thread exists, so every
// thread gets a fresh TLS slot.
static thread_local int s_craftValidationDepth = 0;

// UClass pointer for "InventoryComponent", resolved once at hook install.
// Used to identify player-owned inventories in the count hooks so FreeCraft
// can force 9999 for any query on the player's bag — including UI pre-queue
// checks that don't pass through any of our UFunction validation hooks.
// A non-zero value means "resolution succeeded" and the ownership check is
// active. If resolution ever fails, the ownership fast path is skipped and
// the detours fall back to the depth-counter-only behavior.
static uintptr_t s_invCompUClass = 0;
static int s_logOwnershipForceCount = 0;
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
static int s_logCanStartProcessingCount = 0;
static int s_logAddProcessingRecipeCount = 0;
static int s_logFindItemCountForceCount = 0;
static int s_logFindItemCountPassthroughCount = 0;
static int s_logGetItemCountForceCount = 0;
static int s_logGetItemCountPassthroughCount = 0;
static int s_logProbeHasItems = 0;
static int s_logProbeFindFirstItem = 0;
static int s_logProbePCAddItem = 0;
static int s_logProbeFindItemByType = 0;
static int s_logProbeFind = 0;
static int s_logProbeHasValidItemInSlot = 0;
static int s_logProbeICFindItems = 0;
static int s_logProbeOnServerStopProcessing = 0;
static int s_logProbeFindItemCountByQuery = 0;
static int s_logProbeFindItemCountWithMatchingData = 0;
static int s_logProbeFindItemsByType = 0;
static int s_logProbeFindItemsByQuery = 0;
static int s_logProbeInvAddItem       = 0;
static int s_logProbeICAddItem        = 0;
static int s_logProbeICCAddItem       = 0;
static bool s_findItemCountPassthroughDumped = false;
static bool s_getItemCountPassthroughDumped  = false;

// === Post-ARPC diagnostic tracker ===========================================
// When the OnServer_AddProcessingRecipe thunk successfully grows the queue,
// we remember the processor instance and watch its state for ~3 seconds so
// we can see exactly when the queue drains, whether the pop ever lands the
// recipe into ProcessingItem, and whether MillijoulesProcessed ticks. The
// goal is to catch the silent "pop + drop" path that rejects our forced
// queue entry before DoProcessInternal commits to running it.
static void*     s_arpcTrackedProcessor     = nullptr;
static ULONGLONG s_arpcTrackingDeadlineMs   = 0;
static int32_t   s_arpcTrackedLastQ         = -1;
static uint8_t   s_arpcTrackedLastA         = 0xFF;
static uintptr_t s_arpcTrackedLastPI0       = 0;
static uintptr_t s_arpcTrackedLastPI8       = 0;
static int32_t   s_arpcTrackedLastMJ        = -1;
static float     s_arpcTrackedLastPP        = -1.0f;  // ProcessingProgress (+0x1B8 float)

// CanProcess is polled every frame for every active processor by
// UDeployableTickSubsystem::Tick. NEVER override its return value —
// forcing true made the state machine transition without a CurrentRecipe
// and blew up in StopProcessing last time. HookCanProcess is both an
// observer and a targeted fixup: it trampolines to the original, reads
// the ProcessingQueue.Count and bProcessorActive fields for this self,
// logs transitions, and — if queue has content but active=0 — flips the
// active bit to 1 so the processor actually ticks the recipe. Writing a
// byte from the game thread while the object is being ticked is safe.
struct CanProcessTracker {
    void* self;
    bool lastOrig;
    int32_t lastQueueCount;
    uint8_t lastActive;
    uintptr_t lastProcItem0;   // ProcessingItem first qword (often DataTable*)
    uintptr_t lastProcItem8;   // second qword (FName RowName for DataTable rows)
    bool inited;
};
static CanProcessTracker s_canProcessTracker[64] = {};
static int s_canProcessTrackerCnt = 0;

// One-shot byte-diff state for HookCanQueueItem (see note above the hook).
static bool s_canQueueDiffDone = false;
constexpr int kStateSnapshotBytes = 0x800;
static uint8_t s_stateBefore[kStateSnapshotBytes];

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
    s_logCanStartProcessingCount = 0;
    s_logAddProcessingRecipeCount = 0;
    s_logFindItemCountForceCount = 0;
    s_logFindItemCountPassthroughCount = 0;
    s_logGetItemCountForceCount = 0;
    s_logGetItemCountPassthroughCount = 0;
    s_logProbeHasItems = 0;
    s_logProbeFindFirstItem = 0;
    s_logProbePCAddItem = 0;
    s_logProbeFindItemByType = 0;
    s_logProbeFind = 0;
    s_logProbeHasValidItemInSlot = 0;
    s_logProbeICFindItems = 0;
    s_logProbeOnServerStopProcessing = 0;
    s_logProbeFindItemCountByQuery = 0;
    s_logProbeFindItemCountWithMatchingData = 0;
    s_logProbeFindItemsByType = 0;
    s_logProbeFindItemsByQuery = 0;
    s_logProbeInvAddItem = 0;
    s_logProbeICAddItem = 0;
    s_logProbeICCAddItem = 0;
    s_findItemCountPassthroughDumped = false;
    s_getItemCountPassthroughDumped = false;
    s_arpcTrackedProcessor = nullptr;
    s_arpcTrackingDeadlineMs = 0;
    s_arpcTrackedLastQ = -1;
    s_arpcTrackedLastA = 0xFF;
    s_arpcTrackedLastPI0 = 0;
    s_arpcTrackedLastPI8 = 0;
    s_arpcTrackedLastMJ = -1;
    s_arpcTrackedLastPP = -1.0f;
    s_logOwnershipForceCount = 0;
    for (int i = 0; i < 64; ++i) s_canProcessTracker[i] = {};
    s_canProcessTrackerCnt = 0;
    s_canQueueDiffDone = false;
    s_pendingProcessorKickSelf = nullptr;
    s_lastKickedProcessorSelf = nullptr;
    s_lastProcessorKickTick = 0;
}

void QueueProcessorKick(void* /*self*/) {
    // No-op: cross-thread kick from Trainer::Tick was unsafe (see comment
    // in Trainer::Tick FreeCraft branch). Kept as a stub so existing hooks
    // can call it without ifdef clutter.
}

bool __fastcall HookCanSatisfyRecipeInput(void* self, void* input, int multiplier, void* inventories, int* currentAmount) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "CanSatisfyRecipeInput", ret);
        // Bump the craft-validation depth around the trampoline call so any
        // nested FindItemCountByType / GetItemCount detours know they are
        // inside a UI-side "do I have the materials?" query and should
        // answer with 9999. Outside this window they pass through to the
        // real count — see HookFindItemCountByType for why that matters
        // for processor output delivery (seed extractor).
        s_craftValidationDepth++;
        bool origResult = g_origCanSatisfyRecipeInput(self, input, multiplier, inventories, currentAmount);
        s_craftValidationDepth--;
        if (s_logCanSatisfyCount++ < kMaxHookLogPerHook)
            LOG_HOOK("CanSatisfyRecipeInput#%d caller=+0x%llX self=%p input=%p mult=%d orig=%d -> 1", s_logCanSatisfyCount, (unsigned long long)RelCaller(ret), self, input, multiplier,
                (int)origResult);
        if (!s_dumpedCanSatisfyCtx) {
            s_dumpedCanSatisfyCtx = true;
            DumpCallerContext(ret, "CanSatisfyRecipeInput");
        }
        return true;
    }
    return g_origCanSatisfyRecipeInput(self, input, multiplier, inventories, currentAmount);
}

// One-shot byte-diff on a ProcessingComponent around the CanQueueItem call.
// If g_orig mutates the object (meaning it has side effects — e.g. it
// actually adds the recipe to an internal TArray), we'll see exactly which
// bytes change and at which offsets. The DumpClassProperties probe gives us
// the name map to interpret those offsets afterward.
bool __fastcall HookCanQueueItem(void* self, void* recipeToQueue, void* inventories) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "CanQueueItem", ret);

        bool takeSnapshot = !s_canQueueDiffDone && self;
        if (takeSnapshot) {
            __try {
                memcpy(s_stateBefore, self, kStateSnapshotBytes);
            } __except (1) {
                takeSnapshot = false;
            }
        }

        s_craftValidationDepth++;
        bool origResult = g_origCanQueueItem(self, recipeToQueue, inventories);
        s_craftValidationDepth--;

        if (takeSnapshot) {
            s_canQueueDiffDone = true;
            __try {
                printf("[DIFF] CanQueueItem self=%p orig=%d mutations over 0x%X bytes:\n",
                    self, (int)origResult, kStateSnapshotBytes);
                uint8_t* cur = reinterpret_cast<uint8_t*>(self);
                int diffGroups = 0;
                for (int i = 0; i < kStateSnapshotBytes; i += 8) {
                    if (memcmp(s_stateBefore + i, cur + i, 8) != 0) {
                        printf("[DIFF]   +0x%03X : ", i);
                        for (int j = 0; j < 8; ++j) printf("%02X ", s_stateBefore[i + j]);
                        printf(" -> ");
                        for (int j = 0; j < 8; ++j) printf("%02X ", cur[i + j]);
                        printf("\n");
                        ++diffGroups;
                    }
                }
                if (diffGroups == 0) {
                    printf("[DIFF]   (no byte changes — CanQueueItem is a pure check, "
                           "real queueing must be in a different function)\n");
                }
                printf("[DIFF] === end (%d changed qwords) ===\n", diffGroups);
            } __except (1) {
                printf("[DIFF] exception reading self\n");
            }
        }

        if (s_logCanQueueCount++ < kMaxHookLogPerHook)
            LOG_HOOK("CanQueueItem#%d caller=+0x%llX self=%p recipe=%p inv=%p orig=%d -> 1", s_logCanQueueCount, (unsigned long long)RelCaller(ret), self, recipeToQueue, inventories,
                (int)origResult);
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
            LOG_HOOK("GetMaxCraftableStack#%d caller=+0x%llX self=%p recipe=%p", s_logGetMaxStackCount, (unsigned long long)RelCaller(ret), self, recipe);
        return 9999;
    }
    return g_origGetMaxCraftableStack(self, recipe, inventories, craftingPlayer);
}

// UInventory::ConsumeItem(int location, int amount, bool clearItemSave).
// Standard __fastcall, NOT a Blueprint UFunction. Returning true bypasses
// the slot-search loop entirely - critical when FreeCraft is on with zero
// real items, because the loop's "slot not found" early-exit makes the
// inline-SUB NOP useless. Recipe sits in queue forever otherwise.
// Global capture: the most recent UInventory* the game has actually used.
// GiveItem uses this as a guaranteed-valid placement target when memory
// scanning of the InventoryComponent fails to yield a working Inventory.
// Thread-safe via aligned pointer write (8 bytes atomic on x64).
extern "C" void* g_lastConsumeInventory = nullptr;

bool __fastcall HookConsumeItem(void* self, int location, int amount, bool clearItemSave) {
    void* ret = _ReturnAddress();
    Trainer& t = Trainer::Get();
    // Every ConsumeItem call arrives with a real UInventory in `self`. Cache
    // it ONLY if it looks like a player/container bag (>=10 slots). Torches,
    // fuel slots and similar tiny-inventory callers would otherwise overwrite
    // the player bag pointer we need for GiveItem.
    __try {
        uintptr_t tArrayBase = reinterpret_cast<uintptr_t>(self) + 0x108;
        int32_t   num = *reinterpret_cast<int32_t*>(tArrayBase + 0x08);
        if (num >= 10 && num <= 128) {
            g_lastConsumeInventory = self;
        }
    } __except (1) { /* ignore */ }
    if (t.FreeCraft || t.InfiniteItems) {
        LogFreeCraftPath(FreeCraftPathKind::Items, "ConsumeItem", ret);
        if (s_logConsumeItemCount++ < kMaxHookLogPerHook)
            LOG_HOOK("ConsumeItem#%d self=%p loc=%d amt=%d clear=%d", s_logConsumeItemCount, self, location, amount, (int)clearItemSave);
        return true;
    }
    return g_origConsumeItem(self, location, amount, clearItemSave);
}

bool __fastcall HookHasSufficientResource(void* self, void* resourceType, int requiredAmount,
                                          int recipeCost, void* additionalInventories) {
    void* ret = _ReturnAddress();
    bool freeCraft = Trainer::Get().FreeCraft;
    if (freeCraft) s_craftValidationDepth++;
    bool orig = g_origHasSufficientResource(self, resourceType, requiredAmount,
                                            recipeCost, additionalInventories);
    if (freeCraft) s_craftValidationDepth--;
    if (freeCraft) {
        QueueProcessorKick(self);
        LogFreeCraftPath(FreeCraftPathKind::Resources, "HasSufficientResource", ret);
        if (s_logHasSufficientResourceCount++ < kMaxHookLogPerHook) {
            LOG_HOOK("HasSufficientResource#%d caller=+0x%llX self=%p resource=%p req=%d cost=%d addInv=%p orig=%d -> 1", s_logHasSufficientResourceCount, (unsigned long long)RelCaller(ret), self, resourceType, requiredAmount,
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
    bool freeCraft = Trainer::Get().FreeCraft;
    if (freeCraft) s_craftValidationDepth++;
    int orig = g_origGetResourceRecipeValidity(self, resourceType, requiredAmount, additionalInventories);
    if (freeCraft) s_craftValidationDepth--;
    if (freeCraft) {
        LogFreeCraftPath(FreeCraftPathKind::Resources, "GetResourceRecipeValidity", ret);
        if (s_logGetRecipeValidityCount++ < kMaxHookLogPerHook) {
            LOG_HOOK("GetResourceRecipeValidity#%d caller=+0x%llX self=%p resource=%p req=%d addInv=%p orig=%d -> 0", s_logGetRecipeValidityCount, (unsigned long long)RelCaller(ret), self, resourceType, requiredAmount,
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
            printf("[PROC] HasWaterSourceConnection#%d caller=+0x%llX self=%p mustActive=%d orig=%d (PASSTHROUGH)\n",
                s_logHasWaterSourceCount, (unsigned long long)RelCaller(ret), self,
                (int)mustBeActiveConnection, (int)orig);
        }
    }
    // Passthrough. The force-to-true version did not fix the tick-stuck
    // bug either, so we're back to observing the real value. Once the
    // root cause of the stuck-tick is found we can revisit this.
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

// CanStartProcessing is the gate that decides whether an idle processor
// with a queued recipe can transition to "processing". With FreeCraft on
// and the original CanQueueItem already returning 1, the recipe IS in the
// internal queue — so forcing this gate to true should make the transition
// succeed cleanly (there's a valid CurrentRecipe available, no more
// States[INDEX_NONE] crash like the earlier byte-patch suffered).
//
// A byte patch on this function crashed earlier; a MinHook detour is safer
// because it leaves the original prologue intact (so re-entry, inlined
// helpers, and any cross-call paths keep working). We call the original
// for diagnostics, log its verdict, then override.
bool __fastcall HookCanStartProcessing(void* self) {
    void* ret = _ReturnAddress();
    bool orig = g_origCanStartProcessing(self);
    if (Trainer::Get().FreeCraft) {
        if (s_logCanStartProcessingCount++ < kMaxHookLogPerHook) {
            printf("[PROC] CanStartProcessing#%d caller=+0x%llX self=%p orig=%d -> 1\n",
                s_logCanStartProcessingCount, (unsigned long long)RelCaller(ret), self, (int)orig);
        }
        return true;
    }
    return orig;
}

// Offsets of ProcessingComponent fields we care about — resolved at init
// via DumpClassProperties and kept here so the RPC hook can read/mutate
// them without another name lookup. Match the layout logged at boot:
//   +0x0E0 ProcessingQueue  (TArray: data ptr, +8 Count int32, +12 Max int32)
//   +0x1BC bProcessorActive (1-byte bool)
//   +0x1C0 ProcessingItem   (FProcessingItem struct, 0x218 bytes)
constexpr uintptr_t kPC_ProcessingQueueOff      = 0x0E0;
constexpr uintptr_t kPC_ProcessingQueueCountOff = 0x0E8;
constexpr uintptr_t kPC_bProcessorActiveOff     = 0x1BC;

static bool SafeReadI32(uintptr_t addr, int32_t& out) {
    __try { out = *(int32_t*)addr; return true; } __except (1) { out = 0; return false; }
}
static bool SafeReadU8(uintptr_t addr, uint8_t& out) {
    __try { out = *(uint8_t*)addr; return true; } __except (1) { out = 0; return false; }
}
static bool SafeWriteU8(uintptr_t addr, uint8_t v) {
    __try { *(uint8_t*)addr = v; return true; } __except (1) { return false; }
}
static bool SafeReadUPtr(uintptr_t addr, uintptr_t& out) {
    __try { out = *(uintptr_t*)addr; return true; } __except (1) { out = 0; return false; }
}
static bool SafeReadFloat(uintptr_t addr, float& out) {
    __try { out = *(float*)addr; return true; } __except (1) { out = 0.0f; return false; }
}
static bool SafeWriteI32(uintptr_t addr, int32_t v) {
    __try { *(int32_t*)addr = v; return true; } __except (1) { return false; }
}
static bool SafeWriteU64(uintptr_t addr, uint64_t v) {
    __try { *(uint64_t*)addr = v; return true; } __except (1) { return false; }
}

// SEH-guarded one-shot: call a `void(void*)` trampoline with `ctx`. Returns
// true on clean return, false if the call raised an SEH exception. Kept in
// its own function (no std::string) because MSVC refuses __try inside any
// function that has C++ object unwinding.
static bool SafeCallVoidVoidPtr(void* fn, void* ctx) {
    if (!fn) return false;
    __try {
        reinterpret_cast<void(__fastcall*)(void*)>(fn)(ctx);
        return true;
    } __except (1) {
        return false;
    }
}

// ──────────────────────────────────────────────────────────────────────
// FreeCraft v2: direct output delivery via OnServer_AddItem
// ──────────────────────────────────────────────────────────────────────
// When a recipe is queued while FreeCraft is on, we look up the recipe's
// Outputs in D_ProcessorRecipes and call IcarusController::OnServer_AddItem
// for each output (same code path as the Give menu). This bypasses the
// game's processing tick entirely so crafts complete instantly regardless
// of inputs / state. Requires no hook on the tick subsystem.
//
// D_ProcessorRecipes layout (resolved via `listobj ProcessorRecipesTable`):
//   +0x030  TSparseArray pairs buffer ptr  (row entries)
//   +0x038  int32   num
//   +0x03C  int32   max
//   Each pair entry (24 bytes):
//     +0x00  FName    RowName    (8 bytes)
//     +0x08  void*    StructPtr  (8 bytes, points to the FProcessorRecipe)
//     +0x10  int64    next-free-index / unused
//
// FProcessorRecipe layout (from propswalk ProcessorRecipe):
//   +0x2C8  TArray<FRecipeOutput> Outputs
//     stride 0x20 per entry:
//       +0x00  FItemTemplateRowHandle (24 bytes, points at D_ItemTemplate)
//       +0x18  int32 Count
static void* g_dProcessorRecipes = nullptr;

static void* ResolveDProcessorRecipes() {
    if (g_dProcessorRecipes) return g_dProcessorRecipes;
    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cls = UObjectLookup::GetObjectClassName(obj);
        if (cls != "ProcessorRecipesTable") continue;
        std::string name = UObjectLookup::GetObjectName(obj);
        if (name.rfind("Default__", 0) == 0) continue;
        if (name != "D_ProcessorRecipes") continue;
        g_dProcessorRecipes = reinterpret_cast<void*>(obj);
        printf("[FC-V2] resolved D_ProcessorRecipes @ %p\n", g_dProcessorRecipes);
        return g_dProcessorRecipes;
    }
    printf("[FC-V2] D_ProcessorRecipes NOT FOUND in GObjects\n");
    return nullptr;
}

// Linear scan over the RowMap sparse array for a matching FName comparison
// index. Returns the struct ptr, or nullptr if not found. Brute force is
// fine here: ~2200 entries, runs once per craft click.
static uint8_t* LookupRecipeStruct(int32_t targetCompIdx) {
    void* table = ResolveDProcessorRecipes();
    if (!table) return nullptr;
    uintptr_t t = reinterpret_cast<uintptr_t>(table);
    uint8_t* pairsData = nullptr;
    int32_t  pairsNum  = 0;
    SafeReadUPtr(t + 0x30, *reinterpret_cast<uintptr_t*>(&pairsData));
    SafeReadI32(t + 0x38, pairsNum);
    if (!pairsData || pairsNum <= 0 || pairsNum > 0x4000) return nullptr;
    for (int32_t i = 0; i < pairsNum; ++i) {
        uint8_t* entry = pairsData + i * 0x18;
        int32_t key = 0;
        uintptr_t val = 0;
        if (!SafeReadI32(reinterpret_cast<uintptr_t>(entry), key)) continue;
        if (!SafeReadUPtr(reinterpret_cast<uintptr_t>(entry) + 0x08, val)) continue;
        if (key != targetCompIdx) continue;
        // Value pointer may be decorated by the sparse-array free-list (negative
        // for free slots) — sanity check.
        if (val < 0x10000 || val > 0x00007FFFFFFFFFFFULL) continue;
        return reinterpret_cast<uint8_t*>(val);
    }
    return nullptr;
}

// Find the player's IcarusController (BP_IcarusPlayerControllerSurvival_C).
static void* FindPlayerController() {
    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cls = UObjectLookup::GetObjectClassName(obj);
        if (cls.find("IcarusPlayerController") == std::string::npos
            && cls != "IcarusController") continue;
        std::string name = UObjectLookup::GetObjectName(obj);
        if (name.rfind("Default__", 0) == 0) continue;
        return reinterpret_cast<void*>(obj);
    }
    return nullptr;
}

// Route outputs: station inventory when the processor has its own bag
// (furnace, breeding pen, biofuel), player backpack otherwise (personal craft).
// Inventory is a UInventory* field at ProcessingComponent+0x1A8 (propsall).
static constexpr uintptr_t kPC_InventoryOff = 0x1A8;

static void* GetProcessorInventory(uintptr_t ctx) {
    if (!ctx) return nullptr;
    uintptr_t inv = 0;
    SafeReadUPtr(ctx + kPC_InventoryOff, inv);
    return reinterpret_cast<void*>(inv);
}

static bool IsPlayerBagOrQuickbar(void* inv) {
    if (!inv) return false;
    void* bp = Trainer::Get().ResolvePlayerBackpack();
    void* qb = Trainer::Get().ResolvePlayerQuickbar();
    return inv == bp || inv == qb;
}

// Deliver every output of a recipe. Returns count of output kinds placed.
static int DeliverRecipeOutputs(uint8_t* recipeStruct, int craftCount, void* procCtx) {
    if (!recipeStruct) return 0;
    uint8_t* outputsTArray = recipeStruct + 0x2C8;
    uint8_t* data = nullptr;
    int32_t  num  = 0;
    SafeReadUPtr(reinterpret_cast<uintptr_t>(outputsTArray), *reinterpret_cast<uintptr_t*>(&data));
    SafeReadI32(reinterpret_cast<uintptr_t>(outputsTArray) + 0x08, num);
    if (!data || num <= 0 || num > 16) return 0;

    void* procInv = GetProcessorInventory(reinterpret_cast<uintptr_t>(procCtx));
    bool deliverToProcessor = (procInv != nullptr && !IsPlayerBagOrQuickbar(procInv));
    printf("[FC-V2]   target: %s (procInv=%p)\n",
        deliverToProcessor ? "STATION output" : "PLAYER backpack", procInv);

    int delivered = 0;
    for (int i = 0; i < num; ++i) {
        uint8_t* entry = data + i * 0x20;
        int32_t rowCompIdx = 0;
        SafeReadI32(reinterpret_cast<uintptr_t>(entry) + 0x08, rowCompIdx);
        int32_t perCraft = 1;
        SafeReadI32(reinterpret_cast<uintptr_t>(entry) + 0x18, perCraft);
        std::string rowName = UObjectLookup::ResolveFNameByIndex(rowCompIdx);
        if (rowName.empty() || rowName == "None") continue;
        int totalCount = perCraft * (craftCount > 0 ? craftCount : 1);
        printf("[FC-V2]   giving %s x%d (%d per craft * %d crafts)\n",
            rowName.c_str(), totalCount, perCraft, craftCount);
        bool ok = deliverToProcessor
            ? Trainer_AddItemToProcessor(procCtx, rowName.c_str(), totalCount)
            : Trainer_GiveItem(rowName.c_str(), totalCount);
        if (ok) ++delivered;
    }
    return delivered;
}

// Hook for the UHT-generated thunk of UProcessingComponent::OnServer_AddProcessingRecipe.
// This is the real server RPC that actually appends the recipe to the
// internal ProcessingQueue TArray at +0xE0. Unlike CanQueueItem (which is a
// pure check — confirmed by the byte-diff), this function mutates state.
//
// We hook the THUNK (not the C++ impl) because the thunk-to-impl resolver
// is unreliable for void-returning UFunctions. The thunk is regular x86
// code with a stable UE fastcall signature, so MinHook detours it cleanly.
//
// Strategy:
//   1. Snapshot ProcessingQueue.Count and bProcessorActive before the call.
//   2. Trampoline to the real thunk (which runs the impl and mutates state).
//   3. Re-read both fields. If Count grew and bProcessorActive is still 0,
//      set it to 1 — that's the missing transition we've been chasing.
//   4. Directly append the processor to DeployableTickSubsystem's internal
//      ActiveProcessors TArray so the per-frame tick actually visits it.

// === Tick subsystem direct-write state ====================================
// Discovered via runtime reflection: UDeployableTickSubsystem has three
// TArray<TWeakObjectPtr<>> C++ members (no UPROPERTY reflection on them):
//   +0x40  deployable actors (doors, drills)
//   +0x50  UGeneratorComponent instances
//   +0x60  UProcessingComponent instances  <-- THIS IS US
// The tick function iterates +0x60 and advances MillijoulesProcessed /
// ProcessingProgress for each processor. When the game's internal
// AddProcessingRecipe path skips the registration (some pre-check fails
// and we forced the queue growth anyway), the processor stays out of
// this list and never ticks. We append it directly from the ARPC hook.
constexpr uintptr_t kTickSubsysProcListOff = 0x60;
static uintptr_t s_tickSubsysInstance = 0;
static bool      s_tickSubsysValidated = false;
static int32_t   s_tickSubsysLastCount = -1;
static int32_t   s_tickSubsysLastMax   = -1;

// Lazily resolve the live DeployableTickSubsystem instance by scanning
// GObjects for the non-Default__ instance of that class. Validates the
// offset +0x60 by reading the TArray header and (if non-empty) checking
// that the first element decodes to an FWeakObjectPtr pointing at a
// UProcessingComponent. Safe to call repeatedly — only does real work
// until s_tickSubsysValidated becomes true.
static void ResolveAndValidateTickSubsystem() {
    if (s_tickSubsysValidated) return;
    if (!UObjectLookup::IsInitialized()) return;

    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cls = UObjectLookup::GetObjectClassName(obj);
        if (cls != "DeployableTickSubsystem") continue;
        std::string nm = UObjectLookup::GetObjectName(obj);
        if (nm.rfind("Default__", 0) == 0) continue;

        // Read TArray header at +0x60
        uintptr_t data = 0;
        int32_t count = 0, max = 0;
        SafeReadUPtr(obj + kTickSubsysProcListOff + 0, data);
        SafeReadI32 (obj + kTickSubsysProcListOff + 8, count);
        SafeReadI32 (obj + kTickSubsysProcListOff + 12, max);

        if (!data || count < 0 || max < count || max <= 0 || max > 100000) {
            printf("[TICKFIX] instance @ 0x%p has bad TArray header at +0x%llX "
                   "(data=%p count=%d max=%d), skipping\n",
                (void*)obj, (unsigned long long)kTickSubsysProcListOff,
                (void*)data, count, max);
            continue;
        }

        // If count > 0, verify first element decodes to a ProcessingComponent
        bool valid = (count == 0);
        if (count > 0) {
            uintptr_t firstElem = 0;
            if (SafeReadUPtr(data, firstElem)) {
                int32_t firstIdx = static_cast<int32_t>(firstElem & 0xFFFFFFFFull);
                if (firstIdx > 0 && firstIdx < total) {
                    uintptr_t firstObj = UObjectLookup::GetObjectByIndex(firstIdx);
                    if (firstObj) {
                        std::string firstCls = UObjectLookup::GetObjectClassName(firstObj);
                        if (firstCls == "ProcessingComponent") valid = true;
                    }
                }
            }
        }

        if (valid) {
            s_tickSubsysInstance = obj;
            s_tickSubsysValidated = true;
            s_tickSubsysLastCount = count;
            s_tickSubsysLastMax = max;
            printf("[TICKFIX] RESOLVED DeployableTickSubsystem @ 0x%p  "
                   "list@+0x%llX data=0x%p count=%d max=%d\n",
                (void*)obj, (unsigned long long)kTickSubsysProcListOff,
                (void*)data, count, max);
            return;
        } else {
            printf("[TICKFIX] instance @ 0x%p failed validation at +0x%llX "
                   "(count=%d, first elem not a ProcessingComponent)\n",
                (void*)obj, (unsigned long long)kTickSubsysProcListOff, count);
        }
    }

    // Not found this pass — will retry on next call
}

// Append a UProcessingComponent ctx into the tick subsystem's active list
// via direct memory write. Returns true on success or if the processor
// is already present. The write is a pure memory operation — no UFunction
// call, no script VM — so it is safe to invoke from within the ARPC hook
// without the reentrance crash we hit with OnServer_ActivateProcessor.
//
// Write order is important: data first (Data[Count] = weakPtr) then count
// bump. A concurrent iterator on the same thread would either see the old
// count (ignores new entry) or the new count with a fully-written entry.
// Cross-thread is irrelevant because UE game logic runs on the game thread
// and our hook also runs on the game thread.
static bool AppendCtxToTickSubsystem(void* ctx) {
    ResolveAndValidateTickSubsystem();
    if (!s_tickSubsysValidated || !s_tickSubsysInstance || !ctx) return false;

    uintptr_t arrayAddr = s_tickSubsysInstance + kTickSubsysProcListOff;
    uintptr_t data = 0;
    int32_t count = 0, max = 0;
    if (!SafeReadUPtr(arrayAddr, data))     return false;
    if (!SafeReadI32 (arrayAddr + 8, count)) return false;
    if (!SafeReadI32 (arrayAddr + 12, max))  return false;

    if (!data || count < 0 || count > max || max <= 0 || max > 100000) {
        LOG_TICKFIX("TArray looks stale: data=%p count=%d max=%d — invalidating", (void*)data, count, max);
        s_tickSubsysValidated = false;
        s_tickSubsysInstance = 0;
        return false;
    }

    // Read ctx's UObject::InternalIndex (+0xC in UObject layout)
    int32_t ctxIdx = 0;
    if (!SafeReadI32(reinterpret_cast<uintptr_t>(ctx) + 0xC, ctxIdx) || ctxIdx <= 0) {
        return false;
    }

    // Dedup: scan existing entries for ctxIdx
    for (int i = 0; i < count; ++i) {
        uintptr_t elem = 0;
        if (!SafeReadUPtr(data + static_cast<uintptr_t>(i) * 8, elem)) break;
        int32_t existingIdx = static_cast<int32_t>(elem & 0xFFFFFFFFull);
        if (existingIdx == ctxIdx) {
            // Already present — nothing to do
            return true;
        }
    }

    // Need a free slot. We do NOT grow the TArray because UE uses its own
    // FMemory allocator and inventing a new buffer would require copying
    // all existing entries and hoping the allocator metadata matches.
    // 10 slots of slack (max=24, count=14 typically) is enough for normal
    // play. If a user ever hits the limit, log and bail.
    if (count >= max) {
        LOG_TICKFIX("TArray full (count=%d max=%d) — cannot append without realloc", count, max);
        return false;
    }

    // Construct the FWeakObjectPtr value
    int32_t serial = UObjectLookup::GetObjectSerialNumberByIndex(ctxIdx);
    uint64_t weakPtr =
        (static_cast<uint64_t>(static_cast<uint32_t>(serial)) << 32) |
         static_cast<uint64_t>(static_cast<uint32_t>(ctxIdx));

    // Write the element first, then bump the count
    uintptr_t slotAddr = data + static_cast<uintptr_t>(count) * 8;
    if (!SafeWriteU64(slotAddr, weakPtr)) {
        LOG_TICKFIX("write to slot @ 0x%p failed", (void*)slotAddr);
        return false;
    }
    if (!SafeWriteI32(arrayAddr + 8, count + 1)) {
        LOG_TICKFIX("count bump failed — list may be corrupt");
        return false;
    }

    LOG_TICKFIX("appended ctx=%p idx=%d ser=%d at slot %d (count %d->%d max=%d)", ctx, ctxIdx, serial, count, count, count + 1, max);
    s_tickSubsysLastCount = count + 1;
    return true;
}

void __fastcall HookAddProcessingRecipeThunk(void* context, void* frame, void* result) {
    uintptr_t ctx = reinterpret_cast<uintptr_t>(context);
    int32_t qcBefore = 0;
    uint8_t activeBefore = 0;
    if (ctx) {
        SafeReadI32(ctx + kPC_ProcessingQueueCountOff, qcBefore);
        SafeReadU8(ctx + kPC_bProcessorActiveOff, activeBefore);
    }

    g_origAddProcessingRecipeThunk(context, frame, result);

    if (Trainer::Get().FreeCraft && ctx) {
        int32_t qcAfter = 0;
        uint8_t activeAfter = 0;
        SafeReadI32(ctx + kPC_ProcessingQueueCountOff, qcAfter);
        SafeReadU8(ctx + kPC_bProcessorActiveOff, activeAfter);

        // ── FreeCraft v2: direct delivery path ───────────────────────
        // If the queue grew (we got a valid recipe), look it up in
        // D_ProcessorRecipes, harvest its Outputs, and hand each one to
        // OnServer_AddItem via the existing give pipeline. Then decrement
        // the queue so the game doesn't also try to process it.
        if (qcAfter > qcBefore) {
            uintptr_t queueData = 0;
            if (SafeReadUPtr(ctx + kPC_ProcessingQueueOff, queueData) && queueData) {
                // New entry at queue[qcBefore], stride 0x24:
                //   +0x00  TWeakObjectPtr<CraftingPlayer>
                //   +0x08  ProcessorRecipesRowHandle (24 bytes)
                //     +0x08 inside = RowName FName comp idx
                //   +0x20  int32 CraftCount
                uintptr_t newItem = queueData + static_cast<uintptr_t>(qcBefore) * 0x24;
                int32_t rowCompIdx = 0;
                int32_t craftCount = 1;
                SafeReadI32(newItem + 0x08 + 0x08, rowCompIdx);  // handle.RowName
                SafeReadI32(newItem + 0x20, craftCount);
                std::string recipeName = UObjectLookup::ResolveFNameByIndex(rowCompIdx);
                printf("[FC-V2] craft click: recipe='%s' count=%d ctx=%p\n",
                    recipeName.c_str(), craftCount, context);
                uint8_t* struct_ = LookupRecipeStruct(rowCompIdx);
                if (struct_) {
                    int d = DeliverRecipeOutputs(struct_, craftCount, context);
                    printf("[FC-V2]   delivered %d output kind(s)\n", d);
                    // Cancel the queued work so the processor doesn't also
                    // try to run it (would stall or double-deliver).
                    SafeWriteI32(ctx + kPC_ProcessingQueueCountOff, qcBefore);
                    // Don't force-active — we already delivered.
                    return;
                }
                printf("[FC-V2]   recipe struct lookup FAILED — falling back to legacy queue path\n");
            }
        }

        bool forcedActive = false;
        if (qcAfter > 0 && activeAfter == 0) {
            // Queue has content but the processor is still flagged inactive.
            // Flip the bit so the next tick's Process() actually runs.
            if (SafeWriteU8(ctx + kPC_bProcessorActiveOff, 1)) {
                activeAfter = 1;
                forcedActive = true;
            }
        }

        // Identify the processor and recipe we just touched — without this
        // the numeric addresses in the rest of the log are meaningless.
        std::string procCls = UObjectLookup::GetObjectClassName(ctx);
        std::string recipeRow = "?";
        uintptr_t queueData = 0;
        if (SafeReadUPtr(ctx + kPC_ProcessingQueueOff, queueData) && queueData && qcAfter > 0) {
            // ProcessingItem[0] layout: CraftingPlayer(0x08) + Recipe(0x18) + CraftCount(0x04).
            // Recipe (ProcessorRecipesRowHandle) starts at +0x08 with DataTablePtr(0x08)
            // then RowName FName at +0x10 inside the item.
            recipeRow = UObjectLookup::ReadFNameAt(queueData + 0x10);
            if (recipeRow.empty()) recipeRow = "<empty-fname>";
        }

        if (s_logAddProcessingRecipeCount++ < kMaxHookLogPerHook) {
            printf("[ARPC] OnServer_AddProcessingRecipe#%d ctx=%p class=%s recipeRow=%s "
                   "Queue.Count %d->%d  bProcessorActive %d->%d%s\n",
                s_logAddProcessingRecipeCount, context,
                procCls.empty() ? "?" : procCls.c_str(),
                recipeRow.c_str(),
                qcBefore, qcAfter, (int)activeBefore, (int)activeAfter,
                forcedActive ? "  [forced=1]" : "");
        }

        // NOTE (historical): an earlier iteration tried calling
        // g_origServerActivateProcessor(context) here to explicitly
        // register the processor with UDeployableTickSubsystem. That
        // CRASHED the game in UObject::CallFunction because we were
        // already inside a UFunction thunk (AddProcessingRecipe) when we
        // invoked another UFunction (ActivateProcessor), and the UE4.27
        // script VM cannot handle synchronous nested thunk reentry at
        // this depth. The current approach instead writes our processor
        // directly into the tick subsystem's internal C++ TArray — a
        // pure memory operation, no UFunction, no script VM, no
        // reentrance risk. See AppendCtxToTickSubsystem above.
        if (qcAfter > 0) {
            bool registered = AppendCtxToTickSubsystem(context);
            printf("[ARPC-FIX] AppendCtxToTickSubsystem(%p) -> %s\n",
                context, registered ? "OK (or already present)" : "FAILED");
        }

        // Pre-populate ProcessingItem directly from the queue entry we
        // just appended. The game's internal tick has a C++ (non-UFunction)
        // "ValidateQueueItem" step between Queue.Pop() and
        // ProcessingItem = item that rejects recipes the player lacks
        // ingredients for. When it rejects, the pop has already decremented
        // Q but PI is never written, so the processor stays default and
        // no amount of CanProcess force on our side helps (because our
        // guard requires PI to be populated).
        //
        // By copying the new queue entry into PI ourselves BEFORE the game
        // gets a chance to run the validate step, the game's next tick
        // sees a valid PI and advances MJ normally. Our CanProcess force
        // (piPopulated && bActive==1) then covers any other gates, and the
        // ConsumeItem hook keeps the player's nonexistent items stable.
        //
        // Only runs when PI is currently default (we won't clobber an
        // active craft) and the queue actually grew (we have a fresh
        // entry to copy).
        if (qcAfter > qcBefore) {
            uintptr_t currentPI0 = 0;
            SafeReadUPtr(ctx + 0x1C0, currentPI0);
            bool piDefault = (currentPI0 == 0 || currentPI0 == 0x00000000FFFFFFFFull);
            if (piDefault) {
                uintptr_t queueData = 0;
                if (SafeReadUPtr(ctx + kPC_ProcessingQueueOff, queueData) && queueData) {
                    // ProcessingItem struct is 0x24 bytes:
                    //   +0x00  TWeakObjectPtr<CraftingPlayer>  (8 bytes)
                    //   +0x08  ProcessorRecipesRowHandle Recipe (24 bytes)
                    //   +0x20  int32 CraftCount
                    // The freshly appended entry lives at queue[qcBefore].
                    uintptr_t newItemAddr = queueData + static_cast<uintptr_t>(qcBefore) * 0x24;
                    bool copyOk = true;
                    // Copy 4 qwords (+0..+0x20) = 32 bytes
                    for (int i = 0; i < 0x20 && copyOk; i += 8) {
                        uintptr_t qv = 0;
                        if (!SafeReadUPtr(newItemAddr + i, qv)) { copyOk = false; break; }
                        if (!SafeWriteU64(ctx + 0x1C0 + i, qv)) { copyOk = false; break; }
                    }
                    // Copy the tail int32 CraftCount at +0x20
                    if (copyOk) {
                        int32_t cc = 0;
                        if (!SafeReadI32(newItemAddr + 0x20, cc) ||
                            !SafeWriteI32(ctx + 0x1C0 + 0x20, cc)) {
                            copyOk = false;
                        }
                    }
                    if (copyOk) {
                        // Re-assert bProcessorActive=1 (we already set it
                        // above but the copy window may have raced).
                        SafeWriteU8(ctx + kPC_bProcessorActiveOff, 1);
                        // Decrement the queue so we don't double-process
                        // the same entry (once via PI, once via queue pop).
                        // We removed the LAST element, which is the one
                        // we copied — standard TArray::Pop semantics.
                        SafeWriteI32(ctx + kPC_ProcessingQueueCountOff, qcAfter - 1);

                        // Read back for the log
                        uintptr_t pi0 = 0, pi8 = 0;
                        SafeReadUPtr(ctx + 0x1C0, pi0);
                        SafeReadUPtr(ctx + 0x1C8, pi8);
                        printf("[TICKFIX] pre-populated PI for ctx=%p from queue[%d] "
                               "(PI0=0x%p PI8=0x%p, Q now=%d)\n",
                            context, qcBefore, (void*)pi0, (void*)pi8, qcAfter - 1);
                    } else {
                        LOG_TICKFIX("pre-populate read/write failed for ctx=%p", context);
                    }
                }
            } else {
                LOG_TICKFIX("PI already populated (PI0=0x%p), leaving queue in place", (void*)currentPI0);
            }
        }

        // Arm the post-ARPC state poller so Trainer::Tick will watch this
        // exact processor for 10 seconds and log every transition. Long
        // enough to observe either completion (MJ or PP advancing to max)
        // or a clearly frozen state where nothing moves.
        if (qcAfter > 0) {
            s_arpcTrackedProcessor   = context;
            s_arpcTrackingDeadlineMs = GetTickCount64() + 10000;
            s_arpcTrackedLastQ       = qcAfter;
            s_arpcTrackedLastA       = activeAfter;
            SafeReadUPtr (ctx + 0x1C0, s_arpcTrackedLastPI0);
            SafeReadUPtr (ctx + 0x1C8, s_arpcTrackedLastPI8);
            SafeReadI32  (ctx + 0x450, s_arpcTrackedLastMJ);
            SafeReadFloat(ctx + 0x1B8, s_arpcTrackedLastPP);
            printf("[ARPC-WATCH] armed for ctx=%p Q=%d A=%d PI=[%p %p] MJ=%d PP=%.4f\n",
                context, s_arpcTrackedLastQ, (int)s_arpcTrackedLastA,
                (void*)s_arpcTrackedLastPI0, (void*)s_arpcTrackedLastPI8,
                s_arpcTrackedLastMJ, s_arpcTrackedLastPP);
        }
    }
}

// ============================================================================
// Fast-poll watcher thread — catches sub-frame state transitions
// ----------------------------------------------------------------------------
// PollArpcTrackedProcessor runs off Trainer::Tick (worker, ~30ms period) so it
// misses transitions faster than a frame. The Stone_Shovel craft reset at 98.8%
// happens in a single frame. The fast watcher below runs on a dedicated
// thread polling at ~0.25ms intervals, only logging MEANINGFUL transitions
// (PI reset, A going to 0, MJ/PP dropping) with QueryPerformanceCounter
// microsecond timestamps so we can see exactly when the reset writes happen
// vs. our other log events.
// ============================================================================
struct FastState {
    uintptr_t pi0, pi8;
    int32_t   mj;
    float     pp;
    uint8_t   a;
    int32_t   q;
    uint64_t  qpc;  // QueryPerformanceCounter at read time
};

// Mem-diff capture. We snapshot THREE regions at the start of a craft and
// diff them when FASTW detects the reset:
//   1. ProcessingComponent (0x800 bytes) — the recipe state-machine fields
//   2. OwnerActor (0x1000 bytes)         — the bench actor itself, where
//                                          linked components live
//   3. A few pointer-addressable "inventory candidate" components, discovered
//      by scanning the proc / actor for pointers that land at plausible
//      UInventory-shaped offsets.
// ProcessingComponent snapshot alone showed ONLY local state fields changing
// during Thatch_Wall completion — no slot/TArray — which means the delivered
// item is written to a sibling component, not the processor itself.
// UObject::OuterPrivate lives at +0x20 in UE 4.27 (and for UActorComponent
// the Outer is always the owning AActor — so using Outer is both simpler
// and more build-independent than trying to locate the later OwnerPrivate
// UPROPERTY which shifts with unrelated field additions.
constexpr size_t kProcSnapshotBytes  = 0x800;
constexpr size_t kActorSnapshotBytes = 0x1000;
constexpr uintptr_t kUObj_OuterPrivate = 0x20;

static uint8_t  g_procSnapshotBefore[kProcSnapshotBytes];
static uint8_t  g_actorSnapshotBefore[kActorSnapshotBytes];
static bool     g_memSnapshotBeforeValid = false;
static void*    g_memSnapshotProc  = nullptr;
static void*    g_memSnapshotActor = nullptr;

// Raw-memory-only inner helper. Isolated from the caller so its __try is not
// contaminated by std::string destructors in the outer scope (MSVC C2712).
// Returns owner pointer read from the proc, or nullptr on exception.
static void* SnapshotRawInner(void* proc) {
    void* owner = nullptr;
    __try {
        memcpy(g_procSnapshotBefore, proc, kProcSnapshotBytes);
        // UObject::OuterPrivate — for an ActorComponent, Outer is the AActor
        owner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(proc) + kUObj_OuterPrivate);
        if (owner) {
            memcpy(g_actorSnapshotBefore, owner, kActorSnapshotBytes);
        }
    } __except (1) {
        return nullptr;
    }
    return owner;
}

static void CaptureMemSnapshotBefore(void* proc) {
    g_memSnapshotBeforeValid = false;
    g_memSnapshotProc  = proc;
    g_memSnapshotActor = nullptr;
    void* owner = SnapshotRawInner(proc);
    if (!owner && proc != nullptr) {
        // We distinguish "owner pointer was null" from "copy threw" only by
        // whether SnapshotRawInner succeeded on the proc memcpy. Without a
        // richer return there's no way to tell here; log ambiguous.
        printf("[MEMDIFF] capture BEFORE proc=%p (%d B) — owner was null or read faulted\n",
            proc, (int)kProcSnapshotBytes);
        g_memSnapshotBeforeValid = true;  // proc snapshot may still be valid
        return;
    }
    if (owner) {
        std::string actorClass = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(owner));
        printf("[MEMDIFF] captured BEFORE proc=%p (%d B) + actor=%p class=%s (%d B)\n",
            proc, (int)kProcSnapshotBytes, owner, actorClass.c_str(), (int)kActorSnapshotBytes);
        g_memSnapshotActor = owner;
    }
    g_memSnapshotBeforeValid = true;
}

// Fixed-size stack buffer for the diff — kActorSnapshotBytes (4 KB) is
// the largest region we compare. Stack is cheap on the watcher thread.
static void DumpOneRegionDiff(const char* label, const uint8_t* before, void* liveAddr, size_t bytes) {
    uint8_t after[kActorSnapshotBytes];  // must be >= any region we compare
    if (bytes > sizeof(after)) {
        printf("[MEMDIFF:%s] region too big (%zu > %zu)\n", label, bytes, sizeof(after));
        return;
    }
    bool readOk = true;
    __try {
        memcpy(after, liveAddr, bytes);
    } __except (1) {
        readOk = false;
    }
    if (!readOk) {
        printf("[MEMDIFF:%s] AFTER read exception @ %p\n", label, liveAddr);
        return;
    }
    int changedRanges = 0;
    size_t i = 0;
    printf("[MEMDIFF:%s] diffing 0x%zX bytes @ %p\n", label, bytes, liveAddr);
    while (i < bytes) {
        if (before[i] != after[i]) {
            size_t start = i;
            size_t end = i;
            while (end < bytes && before[end] != after[end]) end++;
            size_t qStart = start & ~7ull;
            size_t qEnd   = (end + 7) & ~7ull;
            if (qEnd > bytes) qEnd = bytes;
            printf("[MEMDIFF:%s]   +0x%03zX-+0x%03zX changed:\n", label, qStart, qEnd);
            for (size_t j = qStart; j < qEnd; j += 8) {
                printf("[MEMDIFF:%s]     +0x%03zX  before:", label, j);
                for (int k = 0; k < 8 && (j + k) < qEnd; ++k) printf(" %02X", before[j + k]);
                printf("   after:");
                for (int k = 0; k < 8 && (j + k) < qEnd; ++k) printf(" %02X", after[j + k]);
                printf("\n");
            }
            changedRanges++;
            i = qEnd;
        } else {
            i++;
        }
    }
    printf("[MEMDIFF:%s] done — %d changed range(s)\n", label, changedRanges);
}

static void DumpMemSnapshotDiff(void* proc) {
    if (!g_memSnapshotBeforeValid || g_memSnapshotProc != proc) {
        printf("[MEMDIFF] cannot diff — no valid before snapshot (valid=%d proc=%p snap=%p)\n",
            (int)g_memSnapshotBeforeValid, proc, g_memSnapshotProc);
        return;
    }
    DumpOneRegionDiff("proc", g_procSnapshotBefore, proc, kProcSnapshotBytes);
    if (g_memSnapshotActor) {
        DumpOneRegionDiff("actor", g_actorSnapshotBefore, g_memSnapshotActor, kActorSnapshotBytes);
    } else {
        printf("[MEMDIFF:actor] no actor pointer captured at BEFORE — skipping\n");
    }
    g_memSnapshotBeforeValid = false;  // one-shot
}

static volatile LONG g_fastWatcherStarted = 0;
static volatile LONG g_fastWatcherStop = 0;
static uint64_t      g_qpcFreq = 0;

static void LogFastState(const char* tag, const FastState& s) {
    if (!g_qpcFreq) {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_qpcFreq = (uint64_t)f.QuadPart;
    }
    uint64_t us = (s.qpc * 1000000ULL) / (g_qpcFreq ? g_qpcFreq : 1);
    printf("[FASTW %s] t=%llu.%03llus Q=%d A=%d PI0=0x%p PI8=0x%p MJ=%d PP=%.4f\n",
        tag, us / 1000000ULL, (us / 1000ULL) % 1000ULL,
        s.q, (int)s.a, (void*)s.pi0, (void*)s.pi8, s.mj, s.pp);
}

static DWORD WINAPI ArpcFastWatcherThread(LPVOID) {
    constexpr int kRing = 32;
    FastState ring[kRing]{};
    int ringIdx = 0;
    bool ringWrapped = false;

    FastState last{};
    bool haveLast = false;
    void* lastProcessor = nullptr;

    while (!g_fastWatcherStop) {
        void* proc = s_arpcTrackedProcessor;
        if (!proc || !Trainer::Get().FreeCraft) {
            haveLast = false;
            lastProcessor = nullptr;
            Sleep(5);
            continue;
        }
        if (proc != lastProcessor) {
            haveLast = false;
            ringIdx = 0;
            ringWrapped = false;
            lastProcessor = proc;
            printf("[FASTW] started for proc=%p\n", proc);
            // Capture initial memory state for the post-completion diff.
            CaptureMemSnapshotBefore(proc);
        }

        uintptr_t ctx = reinterpret_cast<uintptr_t>(proc);
        FastState cur{};
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        cur.qpc = (uint64_t)qpc.QuadPart;
        __try {
            SafeReadI32  (ctx + kPC_ProcessingQueueCountOff, cur.q);
            SafeReadU8   (ctx + kPC_bProcessorActiveOff,     cur.a);
            SafeReadUPtr (ctx + 0x1C0,                       cur.pi0);
            SafeReadUPtr (ctx + 0x1C8,                       cur.pi8);
            SafeReadI32  (ctx + 0x450,                       cur.mj);
            SafeReadFloat(ctx + 0x1B8,                       cur.pp);
        } __except (1) {
            Sleep(10);
            continue;
        }

        ring[ringIdx] = cur;
        ringIdx = (ringIdx + 1) % kRing;
        if (ringIdx == 0) ringWrapped = true;

        if (haveLast) {
            bool piReset = (last.pi0 != 0x00000000FFFFFFFFull && cur.pi0 == 0x00000000FFFFFFFFull);
            bool aDown   = (last.a == 1 && cur.a == 0);
            bool mjDrop  = (cur.mj < last.mj - 100);
            bool ppDrop  = (cur.pp < last.pp - 0.1f);

            if (piReset || aDown || mjDrop || ppDrop) {
                const char* reason =
                    piReset ? "PI-RESET" :
                    aDown   ? "A-DOWN" :
                    mjDrop  ? "MJ-DROP" : "PP-DROP";
                printf("[FASTW *** %s DETECTED ***] proc=%p — dumping last %d samples --------\n",
                    reason, proc, kRing);
                int start = ringWrapped ? ringIdx : 0;
                int count = ringWrapped ? kRing : ringIdx;
                for (int i = 0; i < count; ++i) {
                    int k = (start + i) % kRing;
                    char tag[8];
                    _snprintf_s(tag, sizeof(tag), _TRUNCATE, "h%02d", i);
                    LogFastState(tag, ring[k]);
                }
                LogFastState("NOW", cur);
                printf("[FASTW] ----- end of ring dump --------------------------------------\n");
                // NOW dump the memory diff: what fields on the processor
                // actually got written during the completion tick?
                DumpMemSnapshotDiff(proc);
                printf("[FASTW] ----- watcher quiet until new ARPC arms ---------------------\n");
                haveLast = false;
                while (s_arpcTrackedProcessor == proc && !g_fastWatcherStop) Sleep(5);
                continue;
            }
        }

        last = cur;
        haveLast = true;

        // ~0.25ms poll — single read takes < 1µs, short spin keeps CPU in cache
        Sleep(0);
        for (volatile int i = 0; i < 500; ++i) { /* ~µs spin */ }
    }
    return 0;
}

void StartArpcFastWatcher() {
    if (InterlockedCompareExchange(&g_fastWatcherStarted, 1, 0) != 0) return;
    CreateThread(nullptr, 0, ArpcFastWatcherThread, nullptr, 0, nullptr);
}

// Called from Trainer::Tick (worker thread) once per frame. Polls the last
// ARPC'd processor for up to 3 seconds and logs every field transition.
// SEH-guarded reads mean a destroyed/invalid pointer is handled gracefully.
void PollArpcTrackedProcessor() {
    if (!s_arpcTrackedProcessor) return;
    if (GetTickCount64() >= s_arpcTrackingDeadlineMs) {
        printf("[ARPC-WATCH] expired for ctx=%p (final Q=%d A=%d PI=[%p %p] MJ=%d PP=%.4f)\n",
            s_arpcTrackedProcessor, s_arpcTrackedLastQ, (int)s_arpcTrackedLastA,
            (void*)s_arpcTrackedLastPI0, (void*)s_arpcTrackedLastPI8,
            s_arpcTrackedLastMJ, s_arpcTrackedLastPP);
        s_arpcTrackedProcessor = nullptr;
        return;
    }

    uintptr_t ctx = reinterpret_cast<uintptr_t>(s_arpcTrackedProcessor);
    int32_t q = 0;
    uint8_t a = 0;
    uintptr_t pi0 = 0, pi8 = 0;
    int32_t mj = 0;
    float pp = 0.0f;
    __try {
        SafeReadI32 (ctx + kPC_ProcessingQueueCountOff, q);
        SafeReadU8  (ctx + kPC_bProcessorActiveOff, a);
        SafeReadUPtr(ctx + 0x1C0, pi0);
        SafeReadUPtr(ctx + 0x1C8, pi8);
        SafeReadI32 (ctx + 0x450, mj);
        SafeReadFloat(ctx + 0x1B8, pp);
    } __except (1) {
        printf("[ARPC-WATCH] read exception for ctx=%p — stopping watch\n", s_arpcTrackedProcessor);
        s_arpcTrackedProcessor = nullptr;
        return;
    }

    bool changed = (q != s_arpcTrackedLastQ) ||
                   (a != s_arpcTrackedLastA) ||
                   (pi0 != s_arpcTrackedLastPI0) ||
                   (pi8 != s_arpcTrackedLastPI8) ||
                   (mj != s_arpcTrackedLastMJ) ||
                   (pp != s_arpcTrackedLastPP);
    if (changed) {
        printf("[ARPC-WATCH] ctx=%p Q %d->%d A %d->%d PI0 %p->%p PI8 %p->%p MJ %d->%d PP %.4f->%.4f\n",
            s_arpcTrackedProcessor,
            s_arpcTrackedLastQ, q,
            (int)s_arpcTrackedLastA, (int)a,
            (void*)s_arpcTrackedLastPI0, (void*)pi0,
            (void*)s_arpcTrackedLastPI8, (void*)pi8,
            s_arpcTrackedLastMJ, mj,
            s_arpcTrackedLastPP, pp);
        s_arpcTrackedLastQ   = q;
        s_arpcTrackedLastA   = a;
        s_arpcTrackedLastPI0 = pi0;
        s_arpcTrackedLastPI8 = pi8;
        s_arpcTrackedLastMJ  = mj;
        s_arpcTrackedLastPP  = pp;
    }
}

// ============================================================================
// Talent Model writer — SAFE version.
//
// Earlier iterations flooded ~9 int32 slots and hooked 4 BP-event
// UFunctions. Both caused issues: the multi-offset flood corrupted the
// UMG widget's internal state (crash on execNotEqual_ByteByte), and
// hooking BP-event thunks caused a script VM reentrance crash with
// FFrame.Code = 0xFFFFFFFFFFFFFFFF during the widget Tick.
//
// Keep it to +0xD8 only. That's the slot we confirmed matches the
// user's actual "21 Solo points available" value. It doesn't make the
// UI refresh visually (the widget reads through an opaque getter), but
// writing it raises the internal budget the Model hands out to spend
// paths, so the user CAN spend more points than the UI shows. The UI
// label refreshes on next tab re-open or next level-up event.
// ============================================================================
static void ClampTalentModelAvailablePoints(void* controller, int32_t value) {
    if (!controller) return;
    __try {
        void* model = *(void**)((uintptr_t)controller + 0xC8);
        if (!model) return;
        int32_t* p = (int32_t*)((uintptr_t)model + 0xD8);
        if (*p < value) *p = value;
    }
    __except (1) { }
}

bool __fastcall HookCanProcess(void* self) {
    // CRITICAL: read state BEFORE calling g_origCanProcess. The orig has
    // a state-machine integrity check that resets the processor (PI back
    // to default, bProcessorActive=0) if it detects inconsistencies
    // between ProcessingItem and the unreflected C++ cached-recipe-ptr
    // field the game normally populates during its own pop-to-PI flow.
    // Our pre-populate hack writes PI but can't set those cached fields,
    // so orig would see "PI populated but no cached ptr" and reset.
    // By skipping orig entirely when we're in the "post-prepopulate"
    // state (PI populated + A=1), the game never gets a chance to detect
    // the inconsistency, the tick's advance-MJ path runs based solely
    // on PI+bProcessorActive, and the craft progresses to completion.
    if (Trainer::Get().FreeCraft && self) {
        uintptr_t ctx = reinterpret_cast<uintptr_t>(self);
        uintptr_t prePI0 = 0;
        uint8_t preA = 0;
        SafeReadUPtr(ctx + 0x1C0, prePI0);
        SafeReadU8 (ctx + kPC_bProcessorActiveOff, preA);
        bool prePiPopulated = (prePI0 != 0 && prePI0 != 0x00000000FFFFFFFFull);

        // RESET DETECTOR: per-self boolean remembering whether this processor
        // was ever in the "SKIP-OK" state (A=1 + PI populated). If it was and
        // now isn't, someone just reset it BETWEEN our CanProcess calls. Log
        // the exact observation so the timing lines up with ARPC-WATCH.
        static thread_local struct { void* self; bool wasSkipOk; } s_lastSkipState[16] = {};
        int slot = -1;
        for (int i = 0; i < 16; ++i) {
            if (s_lastSkipState[i].self == self) { slot = i; break; }
            if (s_lastSkipState[i].self == nullptr && slot < 0) slot = i;
        }
        if (slot >= 0 && s_lastSkipState[slot].self == nullptr) s_lastSkipState[slot].self = self;
        bool wasSkipOk = (slot >= 0) ? s_lastSkipState[slot].wasSkipOk : false;
        bool isSkipOk  = prePiPopulated && (preA == 1);
        if (slot >= 0) s_lastSkipState[slot].wasSkipOk = isSkipOk;
        if (wasSkipOk && !isSkipOk) {
            int32_t mj = 0; float pp = 0.0f;
            SafeReadI32(ctx + 0x450, mj);
            SafeReadFloat(ctx + 0x1B8, pp);
            printf("[PROC] *** RESET DETECTED *** self=%p prePI0=0x%p preA=%d MJ=%d PP=%.4f (lastTick was SKIP-OK, this tick is NOT) — orig will now be called\n",
                self, (void*)prePI0, (int)preA, mj, pp);
        }
        if (prePiPopulated && preA == 1) {
            // Log every Nth SKIP so we don't spam but still see sub-frame
            // trailing behavior near the 98.8% reset moment.
            static int s_canProcessSkipCount = 0;
            s_canProcessSkipCount++;
            if (s_canProcessSkipCount <= 30 || (s_canProcessSkipCount % 200) == 0) {
                // Snapshot current progress fields for better diagnostic
                int32_t mj = 0; float pp = 0.0f;
                SafeReadI32(ctx + 0x450, mj);
                SafeReadFloat(ctx + 0x1B8, pp);
                printf("[PROC] CanProcess SKIP-ORIG#%d self=%p prePI0=0x%p preA=1 MJ=%d PP=%.4f -> 1 (orig never called)\n",
                    s_canProcessSkipCount, self, (void*)prePI0, mj, pp);
            }
            return true;
        }
    }

    bool orig = g_origCanProcess(self);
    if (Trainer::Get().FreeCraft && self) {
        uintptr_t ctx = reinterpret_cast<uintptr_t>(self);
        int32_t queueCount = 0;
        uint8_t bActive = 0;
        uintptr_t pi0 = 0, pi8 = 0;
        SafeReadI32(ctx + kPC_ProcessingQueueCountOff, queueCount);
        SafeReadU8(ctx + kPC_bProcessorActiveOff, bActive);
        // Read first 16 bytes of ProcessingItem (FProcessingItem struct).
        // Typical FDataTableRowHandle layout has DataTable* at +0 and
        // FName RowName at +8 — a fresh recipe write will make both
        // qwords transition from 0/0 to (nonzero, nonzero).
        SafeReadUPtr(ctx + 0x1C0, pi0);
        SafeReadUPtr(ctx + 0x1C8, pi8);

        // Find or insert in tracker (linear scan — 64 slots max).
        int idx = -1;
        for (int i = 0; i < s_canProcessTrackerCnt; ++i) {
            if (s_canProcessTracker[i].self == self) { idx = i; break; }
        }
        if (idx < 0) {
            if (s_canProcessTrackerCnt < 64) {
                idx = s_canProcessTrackerCnt++;
                s_canProcessTracker[idx] = { self, orig, queueCount, bActive, pi0, pi8, true };
                std::string className = UObjectLookup::GetObjectClassName(ctx);
                printf("[PROC] CanProcess FIRST self=%p class=%s orig=%d Q=%d A=%d PI=[%p %p]\n",
                    self, className.c_str(), (int)orig, queueCount, (int)bActive,
                    (void*)pi0, (void*)pi8);
            }
        } else {
            auto& t = s_canProcessTracker[idx];
            // Log a transition whenever ANY of the tracked fields changes
            // (including ProcessingItem's first 16 bytes). That's how we'll
            // catch the exact frame a recipe is written into the slot.
            if (t.lastOrig != orig || t.lastQueueCount != queueCount || t.lastActive != bActive
                || t.lastProcItem0 != pi0 || t.lastProcItem8 != pi8) {
                printf("[PROC] CanProcess TRANSITION self=%p orig %d->%d Q %d->%d A %d->%d "
                       "PI0 %p->%p PI8 %p->%p\n",
                    self,
                    (int)t.lastOrig, (int)orig,
                    t.lastQueueCount, queueCount,
                    (int)t.lastActive, (int)bActive,
                    (void*)t.lastProcItem0, (void*)pi0,
                    (void*)t.lastProcItem8, (void*)pi8);
                t.lastOrig = orig;
                t.lastQueueCount = queueCount;
                t.lastActive = bActive;
                t.lastProcItem0 = pi0;
                t.lastProcItem8 = pi8;
            }
        }

        // FreeCraft bypass of the internal input-availability gate.
        //
        // For recipes the player has <1 of the required materials for,
        // g_origCanProcess returns 0 — not because the recipe is invalid
        // but because of an internal check inside DoProcessInternal that
        // walks the player's inventory slots. That check isn't one of our
        // hooked UFunctions (it's a plain C++ method), so we can't force
        // it directly. Instead we override CanProcess's return value.
        //
        // Safe conditions (all three must hold):
        //   1. FreeCraft is enabled
        //   2. PI0 != 0x00000000FFFFFFFF (default/empty marker) — a real
        //      recipe is committed to ProcessingItem, guaranteed by the
        //      TickFix path above. Without this guard, forcing true on
        //      an idle processor triggers the "StopProcessing reads
        //      INDEX_NONE" crash the historical comment warned about.
        //   3. bProcessorActive == 1 — the processor is in a processing
        //      state, not idle or shutting down.
        //
        // With these guards, MJ advances to completion, ConsumeItem hook
        // returns true (keeping nonexistent items stable), output is
        // delivered. Empirically proven working for Pistol_Round when
        // the player had ≥1 of each ingredient; this extends it to 0.
        bool piPopulated = (pi0 != 0 && pi0 != 0x00000000FFFFFFFFull);
        if (piPopulated && bActive == 1) {
            static int s_canProcessForceCount = 0;
            if (!orig && s_canProcessForceCount < 20) {
                s_canProcessForceCount++;
                printf("[PROC] CanProcess FORCE#%d self=%p orig=0 PI0=0x%p -> 1\n",
                    s_canProcessForceCount, self, (void*)pi0);
            }
            return true;
        }
    }
    return orig;
}

// Read UInventory::OwningComponent (+0xB0, TraitBehaviours*) and look up its
// UClass name. Used by the FindItemCountByType / GetItemCount passthrough
// logs so we can tell at a glance whether an inventory belongs to the
// player (InventoryComponent) or a deployable/processor.
static std::string InventoryOwnerClassName(void* inv) {
    if (!inv) return "<null>";
    uintptr_t invPtr = reinterpret_cast<uintptr_t>(inv);
    uintptr_t owning = 0;
    if (!SafeReadUPtr(invPtr + 0xB0, owning) || !owning) return "<no-owner>";
    std::string name = UObjectLookup::GetObjectClassName(owning);
    if (name.empty()) return "<unknown>";
    return name;
}

// Fast (no string allocation) ownership check: is this UInventory owned by
// a UInventoryComponent? Two pointer reads — OwningComponent at +0xB0 and
// then the UClass at +0x10 — and one compare against the cached UClass*.
//
// Called on every FindItemCountByType / GetItemCount hit, so it has to be
// cheap and SEH-safe. On any read failure or missing cache, returns false
// and the caller falls back to the depth-counter logic.
static bool IsPlayerOwnedInventory(void* inv) {
    if (!inv || !s_invCompUClass) return false;
    uintptr_t invPtr = reinterpret_cast<uintptr_t>(inv);
    uintptr_t owning = 0;
    if (!SafeReadUPtr(invPtr + 0xB0, owning) || !owning) return false;
    uintptr_t cls = 0;
    if (!SafeReadUPtr(owning + 0x10, cls) || !cls) return false;
    return cls == s_invCompUClass;
}

// UInventory::FindItemCountByType / UInventory::GetItemCount detours.
//
// Why these are hooks and not byte patches: the previous iteration force-returned
// 9999 from the function prologue unconditionally. That broke seed-extractor
// output delivery: when DoProcessInternal finishes a recipe and asks the
// destination inventory "how many of this item are already here?" it got 9999
// back, concluded the stack was full, and silently dropped the output. Every
// processor that delivers its output into its own +0x1A8 Inventory (seed
// extractor, some crop plots) was affected. Player-bag crafting kept working
// because the AddItem path to the player's main inventory doesn't consult
// these count functions.
//
// The detours now return 9999 only while s_craftValidationDepth > 0 (i.e. we
// are nested inside one of the craft-UI validation hooks above). DoProcessInternal
// runs outside that window, so it sees the real slot state and the AddItem
// call succeeds.
int __fastcall HookFindItemCountByType(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (Trainer::Get().FreeCraft) {
        // Fast path 1 — inside a craft-UI validation hook. Covers the
        // canonical CanSatisfyRecipeInput → ... → FindItemCountByType chain.
        if (s_craftValidationDepth > 0) {
            if (s_logFindItemCountForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("FindItemCountByType FORCE#%d depth=%d self=%p caller=+0x%llX -> 9999", s_logFindItemCountForceCount, s_craftValidationDepth, a0,
                    (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
        // Fast path 2 (DISABLED as diagnostic): ownership-based 9999 force.
        // Hypothesis: the output-delivery path at the end of DoProcessInternal
        // queries the destination inventory to see if the target slot can
        // hold the crafted item. When the destination is the player bag,
        // OWNER-FORCE returned 9999 unconditionally and the game interpreted
        // that as "stack already at cap" → silently dropped the output.
        // Disabling this and relying only on the depth-gated path above means
        // the UI pre-queue check (+0x1BB2C70) will see the real player count;
        // if that check fails we can revisit with a narrower gate.
        #if 0
        if (IsPlayerOwnedInventory(a0)) {
            if (s_logOwnershipForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("FindItemCountByType OWNER-FORCE#%d self=%p caller=+0x%llX -> 9999", s_logOwnershipForceCount, a0, (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
        #endif
    }
    int orig = g_origFindItemCountByType(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logFindItemCountPassthroughCount < kMaxHookLogPerHook) {
        s_logFindItemCountPassthroughCount++;
        void* ret = _ReturnAddress();
        std::string owner = InventoryOwnerClassName(a0);
        LOG_HOOK("FindItemCountByType PASSTHROUGH#%d self=%p owner=%s caller=+0x%llX -> %d", s_logFindItemCountPassthroughCount, a0, owner.c_str(),
            (unsigned long long)RelCaller(ret), orig);
        // Identify the caller the first time we see a passthrough — that's
        // the function running outside any of our validation hooks, most
        // likely inside DoProcessInternal's pop validation path.
        if (!s_findItemCountPassthroughDumped) {
            s_findItemCountPassthroughDumped = true;
            DumpCallerContext(ret, "FindItemCountByType/PASSTHROUGH");
        }
    }
    return orig;
}

int __fastcall HookGetItemCount(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (Trainer::Get().FreeCraft) {
        if (s_craftValidationDepth > 0) {
            if (s_logGetItemCountForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("GetItemCount FORCE#%d depth=%d self=%p caller=+0x%llX -> 9999", s_logGetItemCountForceCount, s_craftValidationDepth, a0,
                    (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
        // OWNER-FORCE disabled — see matching block in HookFindItemCountByType
        // for rationale. We want to confirm output delivery isn't being
        // suppressed by ownership-forced 9999 during DoProcessInternal.
        #if 0
        if (IsPlayerOwnedInventory(a0)) {
            if (s_logOwnershipForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("GetItemCount OWNER-FORCE#%d self=%p caller=+0x%llX -> 9999", s_logOwnershipForceCount, a0, (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
        #endif
    }
    int orig = g_origGetItemCount(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logGetItemCountPassthroughCount < kMaxHookLogPerHook) {
        s_logGetItemCountPassthroughCount++;
        void* ret = _ReturnAddress();
        std::string owner = InventoryOwnerClassName(a0);
        LOG_HOOK("GetItemCount PASSTHROUGH#%d self=%p owner=%s caller=+0x%llX -> %d", s_logGetItemCountPassthroughCount, a0, owner.c_str(),
            (unsigned long long)RelCaller(ret), orig);
        if (!s_getItemCountPassthroughDumped) {
            s_getItemCountPassthroughDumped = true;
            DumpCallerContext(ret, "GetItemCount/PASSTHROUGH");
        }
    }
    return orig;
}

// =============================================================================
// PROBE HOOKS — discovered via UObjectLookup::DumpFunctionsOf
//
// The 5-layer freecraft stack (ownership filter, validation hooks, TickFix,
// pre-populate PI, CanProcess skip-orig) successfully drives crafts to
// MJ ~99% even with 0 real materials, but the OUTPUT delivery still fails
// for missing-input recipes. The completion path appears to be gated by
// an internal C++ check we haven't covered. These three probes target
// the most likely candidates:
//
//   UInventory::HasItems — likely the bool gate "do you have all required
//     items?". Forced to true under FreeCraft. If this is the gate, the
//     fix is immediate; if not, the log shows what other functions fire.
//
//   UProcessingComponent::AddItem — likely the output delivery function.
//     Observer only — its presence in the log when a craft completes
//     confirms it is the path; its absence means output goes through
//     something else (delegate broadcast, manual slot write, etc.).
//
//   UInventory::FindFirstItem — likely the slot-search the consume path
//     uses. Observer only — if it returns -1/null for the failing craft
//     and a real value for the working one, that's our gate.
// =============================================================================
uintptr_t __fastcall HookInvHasItems(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvHasItems(a0, a1, a2, a3, a4, a5);
    bool freeCraft = Trainer::Get().FreeCraft;
    if (freeCraft && s_logProbeHasItems < 30) {
        s_logProbeHasItems++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::HasItems#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX orig=%llu -> %d (FORCED)\n",
            s_logProbeHasItems, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig, freeCraft ? 1 : 0);
    }
    if (freeCraft) return 1;  // force true — the bold fix attempt
    return orig;
}

uintptr_t __fastcall HookInvFindFirstItem(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindFirstItem(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindFirstItem < 30) {
        s_logProbeFindFirstItem++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindFirstItem#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindFirstItem, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;  // observer — no override
}

uintptr_t __fastcall HookPCAddItem(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origPCAddItem(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbePCAddItem < 200) {
        s_logProbePCAddItem++;
        void* ret = _ReturnAddress();
        std::string cls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(a0));
        printf("[PROBE] PC::AddItem#%d self=%p class=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbePCAddItem, a0, cls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;  // observer — no override
}

// Probe hook on UInventory::AddItem — this is where the processor delivers
// crafted output. Runs for ALL AddItem calls (player inventory, deployable
// output slots, container inventories). Observer only so behavior is
// unchanged; presence/absence in the log during a craft completion tells us
// if delivery is even being attempted.
uintptr_t __fastcall HookInvAddItem(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvAddItem(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeInvAddItem < 200) {
        s_logProbeInvAddItem++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::AddItem#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeInvAddItem, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

// Probe hook on UInventoryComponent::AddItem — Inventory and
// InventoryComponent have separate UFunction tables per the SDK dump, so
// hook both and let safeInstall skip whichever ones aren't resolvable.
uintptr_t __fastcall HookICAddItem(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origICAddItem(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeICAddItem < 200) {
        s_logProbeICAddItem++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] IC::AddItem#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeICAddItem, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

// Probe hook on UInventoryContainerComponent::AddItem — the crafting bench's
// output slots live on this subclass. Test showed no Inv::AddItem/IC::AddItem
// call fired during Wood_Window completion; this may be the missing path.
uintptr_t __fastcall HookICCAddItem(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origICCAddItem(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeICCAddItem < 200) {
        s_logProbeICCAddItem++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] ICC::AddItem#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeICCAddItem, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvFindItemByType(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindItemByType(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindItemByType < 30) {
        s_logProbeFindItemByType++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindItemByType#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindItemByType, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvFind(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFind(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFind < 30) {
        s_logProbeFind++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::Find#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFind, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvHasValidItemInSlot(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvHasValidItemInSlot(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeHasValidItemInSlot < 30) {
        s_logProbeHasValidItemInSlot++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::HasValidItemInSlot#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeHasValidItemInSlot, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookICFindItems(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origICFindItems(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeICFindItems < 30) {
        s_logProbeICFindItems++;
        void* ret = _ReturnAddress();
        std::string cls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(a0));
        printf("[PROBE] IC::FindItems#%d self=%p class=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeICFindItems, a0, cls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookPCOnServerStopProcessing(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origPCOnServerStopProcessing(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeOnServerStopProcessing < 30) {
        s_logProbeOnServerStopProcessing++;
        void* ret = _ReturnAddress();
        std::string cls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(a0));
        printf("[PROBE] PC::OnServerStopProcessing#%d self=%p class=%s caller=+0x%llX\n",
            s_logProbeOnServerStopProcessing, a0, cls.c_str(),
            (unsigned long long)RelCaller(ret));
    }
    return orig;
}

uintptr_t __fastcall HookInvFindItemCountByQuery(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindItemCountByQuery(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindItemCountByQuery < 30) {
        s_logProbeFindItemCountByQuery++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindItemCountByQuery#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindItemCountByQuery, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvFindItemCountWithMatchingData(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindItemCountWithMatchingData(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindItemCountWithMatchingData < 30) {
        s_logProbeFindItemCountWithMatchingData++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindItemCountWithMatchingData#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindItemCountWithMatchingData, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvFindItemsByType(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindItemsByType(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindItemsByType < 30) {
        s_logProbeFindItemsByType++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindItemsByType#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindItemsByType, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

uintptr_t __fastcall HookInvFindItemsByQuery(void* a0, void* a1, void* a2, void* a3, void* a4, void* a5) {
    uintptr_t orig = g_origInvFindItemsByQuery(a0, a1, a2, a3, a4, a5);
    if (Trainer::Get().FreeCraft && s_logProbeFindItemsByQuery < 30) {
        s_logProbeFindItemsByQuery++;
        void* ret = _ReturnAddress();
        std::string ownerCls = InventoryOwnerClassName(a0);
        printf("[PROBE] Inv::FindItemsByQuery#%d self=%p owner=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbeFindItemsByQuery, a0, ownerCls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;
}

void InstallCraftValidationHooks(uintptr_t base, size_t sz) {
    if (s_craftHooksInstalled) return;
    s_moduleBase = base;

    // Cache InventoryComponent's UClass* so HookFindItemCountByType /
    // HookGetItemCount can tell a player-owned inventory from a
    // processor/container one with a single 2-pointer read + compare.
    // Resolution might fail on a first call before UObjectLookup is ready —
    // in that case the ownership fast path stays disabled and we fall back
    // to the depth-counter filter alone. Non-fatal.
    if (UObjectLookup::IsInitialized() && !s_invCompUClass) {
        s_invCompUClass = UObjectLookup::FindClassByName("InventoryComponent");
        LOG_FC("InventoryComponent UClass cached at 0x%p", reinterpret_cast<void*>(s_invCompUClass));
    }

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
    uintptr_t findItemCountAddr =
        ResolveNativeOrAob("Inventory", "FindItemCountByType",
                           base, sz, kFindItemCountAob, "FindItemCountByType");
    uintptr_t getItemCountAddr =
        ResolveNativeOrAob("Inventory", "GetItemCount",
                           base, sz, kGetItemCountAob, "GetItemCount");
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
    uintptr_t canStartProcessingAddr =
        ResolveNativeOnly("ProcessingComponent", "CanStartProcessing", "CanStartProcessing");
    uintptr_t canProcessAddr =
        ResolveNativeOnly("ProcessingComponent", "CanProcess", "CanProcess");
    // Probe candidates discovered via DumpFunctionsOf — see HookInvHasItems
    // / HookInvFindFirstItem / HookPCAddItem above for the rationale.
    uintptr_t invHasItemsAddr =
        ResolveNativeOnly("Inventory", "HasItems", "Inventory::HasItems");
    uintptr_t invFindFirstItemAddr =
        ResolveNativeOnly("Inventory", "FindFirstItem", "Inventory::FindFirstItem");
    uintptr_t pcAddItemAddr =
        ResolveNativeOnly("ProcessingComponent", "AddItem", "ProcessingComponent::AddItem");
    uintptr_t invFindItemByTypeAddr =
        ResolveNativeOnly("Inventory", "FindItemByType", "Inventory::FindItemByType");
    uintptr_t invFindAddr =
        ResolveNativeOnly("Inventory", "Find", "Inventory::Find");
    uintptr_t invHasValidItemInSlotAddr =
        ResolveNativeOnly("Inventory", "HasValidItemInSlot", "Inventory::HasValidItemInSlot");
    uintptr_t icFindItemsAddr =
        ResolveNativeOnly("InventoryComponent", "FindItems", "InventoryComponent::FindItems");
    uintptr_t pcOnServerStopProcessingAddr =
        ResolveNativeOnly("ProcessingComponent", "OnServerStopProcessing", "ProcessingComponent::OnServerStopProcessing");
    uintptr_t invFindItemCountByQueryAddr =
        ResolveNativeOnly("Inventory", "FindItemCountByQuery", "Inventory::FindItemCountByQuery");
    uintptr_t invFindItemCountWithMatchingDataAddr =
        ResolveNativeOnly("Inventory", "FindItemCountWithMatchingData", "Inventory::FindItemCountWithMatchingData");
    uintptr_t invFindItemsByTypeAddr =
        ResolveNativeOnly("Inventory", "FindItemsByType", "Inventory::FindItemsByType");
    uintptr_t invFindItemsByQueryAddr =
        ResolveNativeOnly("Inventory", "FindItemsByQuery", "Inventory::FindItemsByQuery");
    uintptr_t invAddItemAddr =
        ResolveNativeOnly("Inventory", "AddItem", "Inventory::AddItem");
    uintptr_t icAddItemAddr =
        ResolveNativeOnly("InventoryComponent", "AddItem", "InventoryComponent::AddItem");
    uintptr_t iccAddItemAddr =
        ResolveNativeOnly("InventoryContainerComponent", "AddItem", "InventoryContainerComponent::AddItem");

    // Talent refresh pipeline — hook the TalentControllerComponent
    // UFunctions so we can clamp the live Model point fields every time
    // the game kicks a UI refresh. This is the only reliable way to
    // propagate our clamped values to the UMG widget since the widget
    // reads through an opaque getter that isn't UProperty-discoverable.
    uintptr_t triggerRefreshAddr =
        ResolveNativeOnly("TalentControllerComponent", "TriggerModelStateRefresh",
                          "TalentControllerComponent::TriggerModelStateRefresh");
    uintptr_t bpForceRefreshAddr =
        ResolveNativeOnly("TalentControllerComponent", "BP_ForceRefresh",
                          "TalentControllerComponent::BP_ForceRefresh");
    uintptr_t onModelViewChangedAddr =
        ResolveNativeOnly("TalentControllerComponent", "OnModelViewChanged",
                          "TalentControllerComponent::OnModelViewChanged");
    uintptr_t nativeModelStateChangedAddr =
        ResolveNativeOnly("TalentControllerComponent", "NativeModelStateChanged",
                          "TalentControllerComponent::NativeModelStateChanged");

    // Resolve the THUNK address (UFunction::Func at +0xD8) for
    // OnServer_AddProcessingRecipe directly, bypassing ResolveThunkToImpl
    // (which is unreliable for void-returning UFunctions). The thunk is a
    // regular fastcall function with a stable UE signature, hookable by
    // MinHook without any issue.
    uintptr_t addProcessingRecipeThunkAddr = 0;
    if (UObjectLookup::IsInitialized()) {
        uintptr_t pcCls = UObjectLookup::FindClassByName("ProcessingComponent");
        if (pcCls) {
            uintptr_t ufunc = UObjectLookup::FindFunctionInClass(pcCls, "OnServer_AddProcessingRecipe");
            if (ufunc) {
                addProcessingRecipeThunkAddr = UObjectLookup::GetUFunctionNativeAddr(ufunc);
                if (addProcessingRecipeThunkAddr) {
                    LOG_RESOLVE("OnServer_AddProcessingRecipe THUNK -> 0x%p", (void*)addProcessingRecipeThunkAddr);
                }
            }
        }
    }

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
                LOG_HOOK("%s: name lookup -> 0x%p", logName, (void*)addr);
                return addr;
            }
        }
        // Tier 2: AOB pattern scan
        uintptr_t hit = UE4::PatternScan(base, sz, aob);
        if (hit) {
            LOG_HOOK("%s: AOB scan -> 0x%p", logName, (void*)hit);
            return hit;
        }
        // Tier 3: PDB offset (last resort)
        uintptr_t cand = base + pdbOff;
        if (MatchPrefix(cand, prefix, prefixLen)) {
            LOG_HOOK("%s: PDB offset -> 0x%p", logName, (void*)cand);
            return cand;
        }
        LOG_HOOK("%s: ALL RESOLVERS FAILED — hook skipped", logName);
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
            LOG_HOOK("ConsumeItem: WARN prologue mismatch (%02X %02X %02X %02X %02X %02X %02X %02X)", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_HOOK("MH_Initialize failed: %d", (int)init);
        return;
    }

    auto install = [](uintptr_t addr, void* detour, void** orig, const char* name) {
        if (!addr) return;
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(addr), detour, orig);
        if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
            LOG_HOOK("%s create failed: %d", name, (int)s);
            return;
        }
        s = MH_EnableHook(reinterpret_cast<void*>(addr));
        if (s != MH_OK && s != MH_ERROR_ENABLED) {
            LOG_HOOK("%s enable failed: %d", name, (int)s);
            return;
        }
        LOG_HOOK("%s installed at 0x%p", name, reinterpret_cast<void*>(addr));
    };

    install(csriAddr,     reinterpret_cast<void*>(&HookCanSatisfyRecipeInput), reinterpret_cast<void**>(&g_origCanSatisfyRecipeInput), "CanSatisfyRecipeInput");
    install(cqiAddr,      reinterpret_cast<void*>(&HookCanQueueItem),          reinterpret_cast<void**>(&g_origCanQueueItem),          "CanQueueItem");
    install(maxStackAddr, reinterpret_cast<void*>(&HookGetMaxCraftableStack),  reinterpret_cast<void**>(&g_origGetMaxCraftableStack),  "GetMaxCraftableStack");
    install(consumeAddr,  reinterpret_cast<void*>(&HookConsumeItem),           reinterpret_cast<void**>(&g_origConsumeItem),           "ConsumeItem");
    install(findItemCountAddr, reinterpret_cast<void*>(&HookFindItemCountByType),
            reinterpret_cast<void**>(&g_origFindItemCountByType), "FindItemCountByType");
    install(getItemCountAddr,  reinterpret_cast<void*>(&HookGetItemCount),
            reinterpret_cast<void**>(&g_origGetItemCount),        "GetItemCount");
    if (csriAddr || cqiAddr || maxStackAddr || consumeAddr || findItemCountAddr || getItemCountAddr) {
        LOG_FC("Item path hooks armed");
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
    install(canStartProcessingAddr, reinterpret_cast<void*>(&HookCanStartProcessing),
            reinterpret_cast<void**>(&g_origCanStartProcessing), "CanStartProcessing");
    install(canProcessAddr, reinterpret_cast<void*>(&HookCanProcess),
            reinterpret_cast<void**>(&g_origCanProcess), "CanProcess");
    install(addProcessingRecipeThunkAddr, reinterpret_cast<void*>(&HookAddProcessingRecipeThunk),
            reinterpret_cast<void**>(&g_origAddProcessingRecipeThunk),
            "OnServer_AddProcessingRecipe(thunk)");
    // Probe hooks — HasItems and FindFirstItem only. PC::AddItem is
    // STILL disabled because its previous resolve landed at +0x4_5F0_D60
    // from module base (~70MB), way outside the normal function code
    // range where the working hooks live (~25MB). MinHook patching that
    // address corrupted adjacent code and crashed the inject. The two
    // remaining probes are in the safe range — safeInstall validates
    // that explicitly.
    auto safeInstall = [&](uintptr_t addr, void* detour, void** orig, const char* name) {
        if (!addr) return;
        uintptr_t off = (addr >= base) ? (addr - base) : 0;
        if (off < 0x10000 || off >= 0x4000000) {
            LOG_HOOK("%s: addr 0x%p (offset 0x%llX) outside plausible code range — SKIP", name, (void*)addr, (unsigned long long)off);
            return;
        }
        install(addr, detour, orig, name);
    };
    safeInstall(invHasItemsAddr, reinterpret_cast<void*>(&HookInvHasItems),
            reinterpret_cast<void**>(&g_origInvHasItems), "Inventory::HasItems (probe)");
    safeInstall(invFindFirstItemAddr, reinterpret_cast<void*>(&HookInvFindFirstItem),
            reinterpret_cast<void**>(&g_origInvFindFirstItem), "Inventory::FindFirstItem (probe)");
    safeInstall(invFindItemByTypeAddr, reinterpret_cast<void*>(&HookInvFindItemByType),
            reinterpret_cast<void**>(&g_origInvFindItemByType), "Inventory::FindItemByType (probe)");
    safeInstall(invFindAddr, reinterpret_cast<void*>(&HookInvFind),
            reinterpret_cast<void**>(&g_origInvFind), "Inventory::Find (probe)");
    safeInstall(invHasValidItemInSlotAddr, reinterpret_cast<void*>(&HookInvHasValidItemInSlot),
            reinterpret_cast<void**>(&g_origInvHasValidItemInSlot), "Inventory::HasValidItemInSlot (probe)");
    safeInstall(icFindItemsAddr, reinterpret_cast<void*>(&HookICFindItems),
            reinterpret_cast<void**>(&g_origICFindItems), "InventoryComponent::FindItems (probe)");
    safeInstall(pcOnServerStopProcessingAddr, reinterpret_cast<void*>(&HookPCOnServerStopProcessing),
            reinterpret_cast<void**>(&g_origPCOnServerStopProcessing), "ProcessingComponent::OnServerStopProcessing (probe)");
    safeInstall(invFindItemCountByQueryAddr, reinterpret_cast<void*>(&HookInvFindItemCountByQuery),
            reinterpret_cast<void**>(&g_origInvFindItemCountByQuery), "Inventory::FindItemCountByQuery (probe)");
    safeInstall(invFindItemCountWithMatchingDataAddr, reinterpret_cast<void*>(&HookInvFindItemCountWithMatchingData),
            reinterpret_cast<void**>(&g_origInvFindItemCountWithMatchingData), "Inventory::FindItemCountWithMatchingData (probe)");
    safeInstall(invFindItemsByTypeAddr, reinterpret_cast<void*>(&HookInvFindItemsByType),
            reinterpret_cast<void**>(&g_origInvFindItemsByType), "Inventory::FindItemsByType (probe)");
    safeInstall(invFindItemsByQueryAddr, reinterpret_cast<void*>(&HookInvFindItemsByQuery),
            reinterpret_cast<void**>(&g_origInvFindItemsByQuery), "Inventory::FindItemsByQuery (probe)");
    safeInstall(invAddItemAddr, reinterpret_cast<void*>(&HookInvAddItem),
            reinterpret_cast<void**>(&g_origInvAddItem), "Inventory::AddItem (probe — output delivery trace)");
    safeInstall(icAddItemAddr, reinterpret_cast<void*>(&HookICAddItem),
            reinterpret_cast<void**>(&g_origICAddItem), "InventoryComponent::AddItem (probe — output delivery trace)");
    safeInstall(iccAddItemAddr, reinterpret_cast<void*>(&HookICCAddItem),
            reinterpret_cast<void**>(&g_origICCAddItem), "InventoryContainerComponent::AddItem (probe — output delivery trace)");
    // PC::AddItem re-enabled: the old DISABLED comment was stale. On the
    // current build PC::AddItem resolves at ~+0x2D243A0 (45 MB from base),
    // comfortably inside safeInstall's 64 MB upper bound. Leaving it as a
    // pure observer (orig called, return passed through) — the risk the old
    // comment warned about was code corruption at 70 MB offset, which is
    // no longer the case.
    safeInstall(pcAddItemAddr, reinterpret_cast<void*>(&HookPCAddItem),
            reinterpret_cast<void**>(&g_origPCAddItem), "ProcessingComponent::AddItem (probe — output delivery trace)");

    // Diagnostic summary — output this LAST so it's easy to grep from the log
    // and see at a glance which probes actually armed. Hook install messages
    // come earlier and can be drowned out by UObjectLookup noise.
    printf("\n[FCSUMMARY] ================= AddItem probe install status =================\n");
    auto armedStr = [](void* orig) { return orig ? "ARMED" : "NOT ARMED"; };
    printf("[FCSUMMARY]   Inventory::AddItem                resolved=0x%p  %s\n",
        (void*)invAddItemAddr, armedStr((void*)g_origInvAddItem));
    printf("[FCSUMMARY]   InventoryComponent::AddItem       resolved=0x%p  %s\n",
        (void*)icAddItemAddr, armedStr((void*)g_origICAddItem));
    printf("[FCSUMMARY]   InventoryContainerComponent::AddItem resolved=0x%p  %s\n",
        (void*)iccAddItemAddr, armedStr((void*)g_origICCAddItem));
    printf("[FCSUMMARY]   ProcessingComponent::AddItem      resolved=0x%p  %s\n",
        (void*)pcAddItemAddr, armedStr((void*)g_origPCAddItem));
    printf("[FCSUMMARY] ================================================================\n\n");
    if (getResourceRecipeValidityAddr || hasSufficientResourceAddr) {
        LOG_FC("Resource path hooks armed");
    }
    if (hasWaterSourceConnectionAddr || serverStartProcessingAddr || serverActivateProcessorAddr
        || canStartProcessingAddr) {
        LOG_FC("Processor runtime hooks armed");
    }

    // Talent refresh hooks DISABLED — hooking these UFunctions caused a
    // script VM reentrance crash. They are all BP-event thunks invoked
    // through UObject::ProcessEvent; swapping their Native pointer via
    // MinHook leaves the FFrame in an inconsistent state and the widget
    // Tick BP crashes later on execNotEqual_ByteByte with Code=0xFFFF..FF.
    // We keep the resolves below for future reference but never install.
    (void)triggerRefreshAddr;
    (void)bpForceRefreshAddr;
    (void)onModelViewChangedAddr;
    (void)nativeModelStateChangedAddr;

    s_craftHooksInstalled = true;
}

}

// ----------------------------------------------------------------------------
// External-linkage trampolines so Trainer.cpp (other TU) can invoke the
// anonymous-namespace implementations above.
// ----------------------------------------------------------------------------

void Trainer_ResetFreeCraftTelemetry()                           { ResetFreeCraftTelemetry(); }
void Trainer_PollArpcTrackedProcessor()                          { PollArpcTrackedProcessor(); }
void Trainer_InstallCraftValidationHooks(uintptr_t base, size_t sz) { InstallCraftValidationHooks(base, sz); }
void Trainer_ResolveAndValidateTickSubsystem()                   { ResolveAndValidateTickSubsystem(); }
void Trainer_ClampTalentModelAvailablePoints(void* ctrl, int32_t v) { ClampTalentModelAvailablePoints(ctrl, v); }
void Trainer_StartArpcFastWatcher()                              { StartArpcFastWatcher(); }

void Trainer::PatchFreeCraftItems(bool enable) {
    // GetScaledRecipeInputCount / GetScaledRecipeResourceItemCount -> mov eax,1; ret
    //   (returning 0 makes production at timer=100% bail because it sums
    //   input counts and rejects recipes with totalCost<=0)
    // CanSatisfyRecipeQueryInput -> mov al,1; ret
    //   (BP UFunction, byte-patch safe, __fastcall detour unsafe — see
    //   header comment about ILLEGAL_INSTRUCTION on the script frame).
    //
    // FindItemCountByType / GetItemCount are deliberately NOT patched here.
    // They used to be "mov eax,9999; ret" but that broke seed-extractor
    // output delivery: DoProcessInternal asks the destination inventory
    // how many of the output item already exist, 9999 made the AddItem
    // path conclude the slot was full and silently drop the result. They
    // are now MinHook detours installed by InstallCraftValidationHooks,
    // gated on s_craftValidationDepth so they only force 9999 while we
    // are nested inside a craft-UI validation hook.
    uint8_t retOne[6]  = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    uint8_t retTrue[3] = { 0xB0, 0x01, 0xC3 }; // mov al, 1; ret

    PatchBytes(m_scaledInputAddr,     retOne,  m_scaledInputBackup,     6, enable, m_scaledInputPatched,     "GetScaledRecipeInputCount");
    PatchBytes(m_scaledResourceAddr,  retOne,  m_scaledResourceBackup,  6, enable, m_scaledResourcePatched,  "GetScaledRecipeResourceItemCount");
    PatchBytes(m_canSatisfyQueryAddr, retTrue, m_canSatisfyQueryBackup, 3, enable, m_canSatisfyQueryPatched, "CanSatisfyRecipeQueryInput");
    // NOTE: EnclosingFn_CanSatisfyRecipeInput and EnclosingFn_CanQueueItem
    // were patched here in an earlier iteration but caused a crash in
    // FArrayProperty::CopyValuesInternal during Blueprint execution. Those
    // functions are called from the UE Blueprint VM and build complex
    // TArray results that the caller reads after the return — a naive
    // "mov [r8],1; ret" corrupts the result struct. Reverted, do not re-add.
}

void Trainer::PatchFreeCraftProcessorGates(bool /*enable*/) {
    // DISABLED — these byte patches caused a delayed crash:
    //   UDeployableTickSubsystem::Tick
    //     -> UProcessingComponent::Process
    //       -> CanProcess
    //         -> StopProcessing
    //           -> SetProcessorState (reads States[INDEX_NONE] -> AV)
    //
    // Forcing CanStartProcessing to always return true made the processor
    // transition into the "processing" state without a valid CurrentRecipe,
    // so the very next tick tried to cleanly stop it and dereferenced an
    // INDEX_NONE entry. The trainer's own header warns: "NEVER add hooks
    // for functions called by the processing tick (CanProcess,
    // CanStartProcessing, DoProcessInternal)".
    //
    // ShelterRequirementsMet is in the same family and removed as a
    // precaution. If free-craft for processors is needed, it has to be
    // driven by actually queueing a valid recipe (let g_origCanQueueItem
    // run) and not by forcing the state machine.
}

// CanQueueItem removed - using GetScaledRecipeInputCount instead

void Trainer::PatchRemoveItem(bool enable) {
    if (!m_removeItemAddr) return;
    if (enable && !m_removeItemPatched) {
        // Read and log current bytes
        uint8_t current[4];
        memcpy(current, reinterpret_cast<void*>(m_removeItemAddr), 4);
        LOG_PATCH("ConsumeItem at 0x%p: bytes = %02X %02X %02X %02X", (void*)m_removeItemAddr, current[0], current[1], current[2], current[3]);

        memcpy(m_removeItemBackup, current, 4);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, PAGE_EXECUTE_READWRITE, &oldP);
        memset(reinterpret_cast<void*>(m_removeItemAddr), 0x90, 4); // NOP 4 bytes
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, oldP, &oldP);

        // Verify
        memcpy(current, reinterpret_cast<void*>(m_removeItemAddr), 4);
        LOG_PATCH("After NOP: %02X %02X %02X %02X %s", current[0], current[1], current[2], current[3],
            (current[0] == 0x90) ? "(OK!)" : "(FAILED!)");

        m_removeItemPatched = true;
    } else if (!enable && m_removeItemPatched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(m_removeItemAddr), m_removeItemBackup, 4);
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 4, oldP, &oldP);
        m_removeItemPatched = false;
        LOG_PATCH("RemoveItem restored");
    }
}
