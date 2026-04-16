#include "Trainer.h"
#include "UE4.h"
#include "UObjectLookup.h"
#include "Logger.h"
#include "libs/minhook/include/MinHook.h"
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <string>

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

uintptr_t ResolveNativeOnly(const char* className, const char* fnName, const char* logName) {
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
bool __fastcall HookConsumeItem(void* self, int location, int amount, bool clearItemSave) {
    void* ret = _ReturnAddress();
    if (Trainer::Get().FreeCraft) {
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
        if (prePiPopulated && preA == 1) {
            static int s_canProcessSkipCount = 0;
            if (s_canProcessSkipCount < 20) {
                s_canProcessSkipCount++;
                printf("[PROC] CanProcess SKIP-ORIG#%d self=%p prePI0=0x%p preA=1 -> 1 (orig never called)\n",
                    s_canProcessSkipCount, self, (void*)prePI0);
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
        // Fast path 2 — the inventory we are querying is player-owned
        // (UInventoryComponent). This catches the UI pre-queue check at
        // +0x1BB2C70 and any other helper that hits player state outside a
        // hooked validation function. Returning 9999 here is what lets you
        // craft with 0 real materials. Processor / deployable inventories
        // (InventoryContainerComponent owner) fall through to the real
        // count so output delivery isn't broken.
        if (IsPlayerOwnedInventory(a0)) {
            if (s_logOwnershipForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("FindItemCountByType OWNER-FORCE#%d self=%p caller=+0x%llX -> 9999", s_logOwnershipForceCount, a0, (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
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
        if (IsPlayerOwnedInventory(a0)) {
            if (s_logOwnershipForceCount++ < kMaxHookLogPerHook) {
                void* ret = _ReturnAddress();
                LOG_HOOK("GetItemCount OWNER-FORCE#%d self=%p caller=+0x%llX -> 9999", s_logOwnershipForceCount, a0, (unsigned long long)RelCaller(ret));
            }
            return 9999;
        }
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
    if (Trainer::Get().FreeCraft && s_logProbePCAddItem < 30) {
        s_logProbePCAddItem++;
        void* ret = _ReturnAddress();
        std::string cls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(a0));
        printf("[PROBE] PC::AddItem#%d self=%p class=%s args=[%p %p %p %p %p] caller=+0x%llX -> 0x%llx\n",
            s_logProbePCAddItem, a0, cls.c_str(), a1, a2, a3, a4, a5,
            (unsigned long long)RelCaller(ret), (unsigned long long)orig);
    }
    return orig;  // observer — no override
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
    // PC::AddItem still disabled
    LOG_FC("  ProcessingComponent::AddItem = 0x%p (DISABLED — bad address range)", (void*)pcAddItemAddr);
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
            LOG_WARN("unresolved property %s::%s", className, propName);
        }
    };

    LOG_SECTION("Resolving UPROPERTY offsets via name lookup");

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
    resolve(Off::State_ModInternalTemp, "SurvivalCharacterState", "ModifiedInternalTemperature");
    resolve(Off::State_InternalTemp,    "SurvivalCharacterState", "InternalTemperature");
    resolve(Off::State_TotalExp,        "SurvivalCharacterState", "TotalExperience");
    resolve(Off::State_Level,           "SurvivalCharacterState", "Level");

    // --- AController / AIcarusPlayerState ---
    resolve(Off::Ctrl_PlayerState,       "Controller",        "PlayerState");
    // These two are embedded structs on the PlayerState. The struct types
    // themselves (OnlineProfileCharacter / OnlineProfileUser) are USTRUCTs,
    // but the enclosing UPROPERTY lives on IcarusPlayerState so the
    // reflection walk succeeds. On failure we keep the SDK-dump fallback.
    resolve(Off::PS_ActiveCharacter,     "IcarusPlayerState", "ActiveCharacter");
    resolve(Off::PS_ActiveUserProfile,   "IcarusPlayerState", "ActiveUserProfile");

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

    LOG_OK("offset resolution complete");
    LOG_DEBUG("Off ModInternalTemp=0x%llx InternalTemp=0x%llx TotalExp=0x%llx Level=0x%llx",
        (unsigned long long)Off::State_ModInternalTemp,
        (unsigned long long)Off::State_InternalTemp,
        (unsigned long long)Off::State_TotalExp,
        (unsigned long long)Off::State_Level);
    LOG_DEBUG("Off Ctrl_PlayerState=0x%llx PS_ActiveCharacter=0x%llx PS_ActiveUserProfile=0x%llx",
        (unsigned long long)Off::Ctrl_PlayerState,
        (unsigned long long)Off::PS_ActiveCharacter,
        (unsigned long long)Off::PS_ActiveUserProfile);
}

// SEH helpers — __try/__except can't coexist with C++ object unwinding
// (like std::string), so keep the raw reads in small no-object functions.
static bool SafeReadPtr(uintptr_t addr, uintptr_t& out) {
    __try { out = *(uintptr_t*)addr; return true; }
    __except (1) { out = 0; return false; }
}
static bool SafeReadInt32(uintptr_t addr, int32_t& out) {
    __try { out = *(int32_t*)addr; return true; }
    __except (1) { out = 0; return false; }
}

// Scan GObjects for live instances whose UClass name contains `needle`
// (case-insensitive). Used to discover where Icarus actually stores talent
// / tech / solo progression at runtime when the PlayerState profile
// structs turn out to be empty. Slow (full GObjects walk) but one-shot.
static void FindLiveInstancesByClassSubstring(const char* needle, int maxResults = 20) {
    if (!UObjectLookup::IsInitialized()) return;
    int32_t count = UObjectLookup::GetObjectCount();
    int found = 0;
    size_t needleLen = 0;
    while (needle[needleLen]) ++needleLen;

    auto icontains = [](const std::string& hay, const char* nd, size_t ndLen) -> bool {
        if (hay.size() < ndLen) return false;
        for (size_t i = 0; i + ndLen <= hay.size(); ++i) {
            bool ok = true;
            for (size_t j = 0; j < ndLen; ++j) {
                char a = hay[i + j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                char b = nd[j];      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };

    LOG_SCAN("'%s': walking %d UObjects...", needle, count);
    for (int32_t i = 0; i < count && found < maxResults; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cn = UObjectLookup::GetObjectClassName(obj);
        if (cn.empty()) continue;
        if (!icontains(cn, needle, needleLen)) continue;
        std::string on = UObjectLookup::GetObjectName(obj);
        LOG_SCAN("  [#%d] 0x%llx name=%s class=%s", i, (unsigned long long)obj, on.c_str(), cn.c_str());
        ++found;
    }
    LOG_SCAN("'%s': %d live matches", needle, found);
}

// Safely read a TArray<UObject*> header (data pointer + count) from an
// offset on a live actor. SEH-only, no C++ objects, so C2712 doesn't
// trip. Returns false on any bad read.
static bool SafeReadTArrayHeader(void* owner, uintptr_t fieldOff,
                                  void*** outData, int32_t* outNum) {
    __try {
        *outData = *(void***)((uintptr_t)owner + fieldOff);
        *outNum  = *(int32_t*)((uintptr_t)owner + fieldOff + 8);
        return true;
    }
    __except (1) {
        *outData = nullptr;
        *outNum  = 0;
        return false;
    }
}

static bool SafeReadPtrIndex(void** data, int32_t index, void** outPtr) {
    __try { *outPtr = data[index]; return true; }
    __except (1) { *outPtr = nullptr; return false; }
}

// SEH-wrapped 16-byte read. Used by HexDump to walk a UObject's bytes
// without crashing on short reads at the end of an allocation.
static bool SafeReadBytes16(const void* base, size_t off, unsigned char out[16]) {
    __try {
        const unsigned char* p = (const unsigned char*)base + off;
        for (int i = 0; i < 16; ++i) out[i] = p[i];
        return true;
    }
    __except (1) { return false; }
}

// SEH-wrapped pointer read at an arbitrary offset.
static void* SafeReadPtrAt(const void* base, uintptr_t off) {
    __try { return *(void**)((const unsigned char*)base + off); }
    __except (1) { return nullptr; }
}

// Hex dump of the first `bytes` bytes of a UObject, for layout discovery.
// Helps locate valid-looking pointer candidates (0x000001e0xxxxxxxx heap
// range) when a known-good field offset isn't yet discovered.
static void HexDump(const char* label, void* obj, size_t bytes) {
    if (!obj) { LOG_LAYOUT("%s: null", label); return; }
    LOG_LAYOUT("=== %s @ 0x%p ===", label, obj);
    for (size_t off = 0; off < bytes; off += 16) {
        unsigned char b[16];
        if (!SafeReadBytes16(obj, off, b)) return;
        LOG_LAYOUT("  +0x%03llx  %02x %02x %02x %02x %02x %02x %02x %02x  "
                   "%02x %02x %02x %02x %02x %02x %02x %02x",
            (unsigned long long)off,
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
}

// Enumerate every live (non-CDO) instance of a class and print its
// address + the pointer at a configurable "probe" offset. Used to find
// the correct TalentController instance whose Model is non-null.
static void EnumerateLiveInstancesWithProbe(const char* className,
                                             uintptr_t probeOff,
                                             int maxResults) {
    if (!UObjectLookup::IsInitialized()) return;
    int32_t count = UObjectLookup::GetObjectCount();
    int found = 0;
    LOG_SCAN("enumerate '%s' probe@+0x%llx", className,
             (unsigned long long)probeOff);
    for (int32_t i = 0; i < count && found < maxResults; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cn = UObjectLookup::GetObjectClassName(obj);
        if (cn != className) continue;
        std::string on = UObjectLookup::GetObjectName(obj);
        if (on.compare(0, 9, "Default__") == 0) continue;
        void* probe = SafeReadPtrAt((void*)obj, probeOff);
        LOG_SCAN("  #%d  obj=0x%llx name=%s  probe=0x%p",
                 i, (unsigned long long)obj, on.c_str(), probe);
        ++found;
    }
    if (found == 0) LOG_WARN("no live '%s' instances", className);
}

// Dump every component attached to an Icarus character via the
// InstanceComponents / BlueprintCreatedComponents TArrays. Helps locate
// things like TalentsComponent / PlayerProgressionComponent that the
// character may own directly. Split into SafeRead* (SEH) + printf/std::string
// (no SEH) because __try + C++ unwinding can't coexist in one function.
static void DumpCharacterComponentsOne(const char* label, void* character, uintptr_t offT) {
    if (!offT) { LOG_COMP("%s: offset 0 - unresolved", label); return; }
    void** data = nullptr;
    int32_t num = 0;
    if (!SafeReadTArrayHeader(character, offT, &data, &num)) {
        LOG_COMP("%s: TArray header read crashed", label);
        return;
    }
    LOG_COMP("%s @ +0x%llx: num=%d data=0x%llx", label, (unsigned long long)offT, num, (unsigned long long)data);
    if (!data || num <= 0 || num > 256) return;

    for (int32_t i = 0; i < num; ++i) {
        void* comp = nullptr;
        if (!SafeReadPtrIndex(data, i, &comp) || !comp) continue;
        std::string cn = UObjectLookup::GetObjectClassName((uintptr_t)comp);
        LOG_COMP("  [%2d] 0x%p  class=%s", i, comp, cn.c_str());
    }
}

static void DumpCharacterComponents(void* character) {
    if (!character) return;
    DumpCharacterComponentsOne("InstanceComponents",         character, Off::Char_InstanceComponents);
    DumpCharacterComponentsOne("BlueprintCreatedComponents", character, Off::Char_BlueprintComponents);
}

// Scan character component lists for a component whose class name matches
// exactly (case-sensitive). Returns the component pointer, or nullptr.
// Used to cache the player's ExperienceComponent.
static void* FindCharacterComponentByClass(void* character, const char* className) {
    if (!character) return nullptr;
    for (int pass = 0; pass < 2; ++pass) {
        uintptr_t off = (pass == 0) ? Off::Char_InstanceComponents : Off::Char_BlueprintComponents;
        if (!off) continue;
        void** data = nullptr;
        int32_t num = 0;
        if (!SafeReadTArrayHeader(character, off, &data, &num)) continue;
        if (!data || num <= 0 || num > 256) continue;
        for (int32_t i = 0; i < num; ++i) {
            void* comp = nullptr;
            if (!SafeReadPtrIndex(data, i, &comp) || !comp) continue;
            std::string cn = UObjectLookup::GetObjectClassName((uintptr_t)comp);
            if (cn == className) return comp;
        }
    }
    return nullptr;
}

// Walk GObjects for the first live (non-CDO) instance of a class. The CDO
// has the name prefix "Default__" which we skip. Returns nullptr if no
// live instance is found.
static void* FindFirstLiveInstance(const char* className) {
    if (!UObjectLookup::IsInitialized()) return nullptr;
    int32_t count = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cn = UObjectLookup::GetObjectClassName(obj);
        if (cn != className) continue;
        std::string on = UObjectLookup::GetObjectName(obj);
        if (on.compare(0, 9, "Default__") == 0) continue; // skip CDO
        return (void*)obj;
    }
    return nullptr;
}

// Walk GObjects for the first live instance of a class whose pointer at
// `probeOff` is non-null. Used to pick the "real" TalentController (the
// one with an initialized Model) over the several dormant shells that
// Icarus keeps around per session / per screen.
static void* FindFirstInstanceWithNonNullProbe(const char* className,
                                                uintptr_t probeOff) {
    if (!UObjectLookup::IsInitialized()) return nullptr;
    int32_t count = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cn = UObjectLookup::GetObjectClassName(obj);
        if (cn != className) continue;
        std::string on = UObjectLookup::GetObjectName(obj);
        if (on.compare(0, 9, "Default__") == 0) continue;
        void* probe = SafeReadPtrAt((void*)obj, probeOff);
        if (!probe) continue;
        return (void*)obj;
    }
    return nullptr;
}

// One-shot diagnostic dump of a TArray<MetaResource>. Prints every entry
// (MetaRow FString + Count int32) so we can see the REAL key names the
// game uses. Kept in its own no-object function because of the __try.
static void DumpMetaResourceArray(const char* label, uintptr_t arrayAddr) {
    LOG_META("%s  array @ 0x%llx", label, (unsigned long long)arrayAddr);
    __try {
        uintptr_t dataPtr = *(uintptr_t*)arrayAddr;
        int32_t   num     = *(int32_t*)(arrayAddr + 8);
        int32_t   cap     = *(int32_t*)(arrayAddr + 12);
        LOG_META("  data=0x%llx  num=%d  max=%d", (unsigned long long)dataPtr, num, cap);
        if (!dataPtr || num <= 0 || num > 256) return;

        for (int32_t i = 0; i < num; ++i) {
            uintptr_t entry = dataPtr + (uintptr_t)i * Off::MR_Size;
            wchar_t*  str   = *(wchar_t**)(entry + Off::MR_MetaRow);
            int32_t   sLen  = *(int32_t*)(entry + Off::MR_MetaRow + 8);
            int32_t   count = *(int32_t*)(entry + Off::MR_Count);

            // Best-effort narrow conversion for printf. FString len
            // includes the null terminator.
            char narrow[80] = {0};
            if (str && sLen > 0 && sLen < 80) {
                int effective = sLen - 1;
                if (effective > 79) effective = 79;
                for (int c = 0; c < effective; ++c) {
                    wchar_t wc = str[c];
                    narrow[c] = (wc < 0x80) ? (char)wc : '?';
                }
            }
            LOG_META("  [%2d] MetaRow=\"%s\" (len=%d)  Count=%d", i, narrow, sLen, count);
        }
    }
    __except (1) {
        LOG_META("  walk crashed");
    }
}

// Walk a TArray<MetaResource> inside an OnlineProfileCharacter /
// OnlineProfileUser struct and clamp the Count field of the entry whose
// FString MetaRow equals `key`. Returns true if a matching entry was
// found and written. Kept in its own no-object function so __try/__except
// covers every raw read without fighting C++ unwinding.
//
// Memory layout (UE 4.27, Icarus SDK dump):
//   MetaResource {
//       FString MetaRow;  // +0x00 { wchar_t* data, int32 num, int32 max }
//       int32   Count;    // +0x10
//       uint8   Pad[4];   // +0x14
//   }  // sizeof == 0x18
static bool ClampMetaResourceByName(uintptr_t arrayAddr, const wchar_t* key, int32_t target) {
    __try {
        uintptr_t dataPtr = *(uintptr_t*)arrayAddr;
        int32_t   num     = *(int32_t*)(arrayAddr + 8);
        if (!dataPtr || num <= 0 || num > 256) return false;

        size_t keyLen = 0;
        while (key[keyLen]) ++keyLen;

        for (int32_t i = 0; i < num; ++i) {
            uintptr_t entry = dataPtr + (uintptr_t)i * Off::MR_Size;
            wchar_t*  str   = *(wchar_t**)(entry + Off::MR_MetaRow);
            int32_t   sLen  = *(int32_t*)(entry + Off::MR_MetaRow + 8);
            // FString Num includes the null terminator.
            if (!str || sLen <= 1 || sLen > 64) continue;
            if ((size_t)(sLen - 1) != keyLen) continue;

            bool match = true;
            for (size_t c = 0; c < keyLen; ++c) {
                if (str[c] != key[c]) { match = false; break; }
            }
            if (!match) continue;

            int32_t* countPtr = (int32_t*)(entry + Off::MR_Count);
            if (*countPtr < target) *countPtr = target;
            return true;
        }
    } __except (1) {
        return false;
    }
    return false;
}

// Walk the FField ChildProperties chain of a UClass and log every property
// with its type + offset. Walks the Super chain too. UE 4.27 layout:
//   UStruct.ChildProperties @ +0x50  (head of FField linked list)
//   UStruct.SuperStruct    @ +0x40  (parent UClass)
//   FField.ClassPrivate    @ +0x00  (FFieldClass* — has its own FName at +0)
//   FField.Next            @ +0x20
//   FField.NamePrivate     @ +0x28  (FName)
//   FProperty.Offset_Internal @ +0x4C (int32 byte offset within instance)
// Used as a one-shot probe to map the internal layout of ProcessingComponent
// without hardcoding offsets.
static void DumpClassProperties(const char* className) {
    if (!UObjectLookup::IsInitialized()) return;
    uintptr_t cls = UObjectLookup::FindClassByName(className);
    if (!cls) {
        LOG_PROPS("%s: class not found", className);
        return;
    }

    constexpr int OFF_SUPER       = 0x40;
    constexpr int OFF_CHILDPROPS  = 0x50;
    constexpr int OFF_FFIELD_NEXT = 0x20;
    constexpr int OFF_FFIELD_NAME = 0x28;
    constexpr int OFF_FPROP_OFF   = 0x4C;

    LOG_PROPS("=== %s layout ===", className);
    uintptr_t walker = cls;
    int hops = 0;
    int totalProps = 0;
    while (walker && hops < 16 && totalProps < 600) {
        std::string clsNameStr = UObjectLookup::GetObjectName(walker);
        LOG_PROPS("class %s (UClass=0x%llX)", clsNameStr.c_str(),
            (unsigned long long)walker);

        uintptr_t childProp = 0;
        SafeReadPtr(walker + OFF_CHILDPROPS, childProp);

        while (childProp && totalProps < 600) {
            uintptr_t ffieldClass = 0;
            uintptr_t nextField = 0;
            int32_t propOff = 0;
            if (!SafeReadPtr(childProp, ffieldClass)) break;
            if (!SafeReadPtr(childProp + OFF_FFIELD_NEXT, nextField)) break;
            if (!SafeReadInt32(childProp + OFF_FPROP_OFF, propOff)) break;

            std::string typeName = ffieldClass
                ? UObjectLookup::ReadFNameAt(ffieldClass)
                : std::string("<?>");
            std::string propName = UObjectLookup::ReadFNameAt(childProp + OFF_FFIELD_NAME);
            LOG_PROPS("  +0x%03X  %-22s  %s", propOff, typeName.c_str(), propName.c_str());

            childProp = nextField;
            ++totalProps;
        }

        uintptr_t super = 0;
        SafeReadPtr(walker + OFF_SUPER, super);
        if (super == walker) break;
        walker = super;
        ++hops;
    }
    LOG_PROPS("=== %s done (%d properties across %d classes) ===", className, totalProps, hops);
}

// =============================================================================
// Subsystem instance layout inspector
//
// DeployableTickSubsystem has zero reflected UPROPERTY fields — its active
// processors live in a plain C++ TArray member that UObjectLookup can't
// locate by name. So we fall back to signature-based pattern scanning on
// the live instance's raw memory: a UE TArray is a distinctive 16-byte
// record { void* Data, int32 Count, int32 Max }, and at init (before any
// recipes are queued) the active list is typically small with a valid
// heap pointer. Dumping the first 0x200 bytes also gives us a visual
// reference we can cross-check.
//
// Heap-range heuristic: any address within the game process's user-mode
// address space (0x00007FFxxxxxxxxx typically, or 0x0000026xxxxxxxxx for
// UE allocations) is considered "looks heapy". We reject kernel addresses,
// tiny values, and known UClass pointer ranges.
// =============================================================================
static bool LooksLikeHeapPtr(uintptr_t p) {
    if (p < 0x10000) return false;                    // null / small ints
    if (p >= 0x00007FFF00000000ULL) return false;     // kernel / non-user
    // UE heap allocations on Windows live in 0x0000000100000000..
    // 0x00007FF000000000 roughly. Anything in that range is plausible.
    return true;
}

// SEH-only helper — reads N bytes from `addr` into `buf`, returns the number
// of bytes successfully read before hitting an inaccessible page. No C++
// objects, no std::string: MSVC refuses __try inside any function that
// requires object unwinding.
static int SafeReadBytes(uintptr_t addr, uint8_t* buf, int n) {
    int read = 0;
    __try {
        for (int i = 0; i < n; ++i) {
            buf[i] = *reinterpret_cast<uint8_t*>(addr + i);
            read = i + 1;
        }
    } __except (1) {}
    return read;
}

// SEH-only helper — reads up to `n` pointer-sized elements from `arr` into
// `out`. Used to sample the first entries of a suspected TArray without
// crashing on freed or garbage Data pointers.
static int SafeReadPtrArray(uintptr_t arr, uintptr_t* out, int n) {
    int read = 0;
    __try {
        for (int i = 0; i < n; ++i) {
            out[i] = reinterpret_cast<uintptr_t*>(arr)[i];
            read = i + 1;
        }
    } __except (1) {}
    return read;
}

static void InspectSubsystemInstanceLayout(const char* className) {
    if (!UObjectLookup::IsInitialized()) return;

    uintptr_t cls = UObjectLookup::FindClassByName(className);
    if (!cls) {
        LOG_LAYOUT("%s: class not found", className);
        return;
    }

    // Walk GObjects to find the first non-Default__ live instance of this
    // class. CDO (Default__) is useless because it's a template not used
    // by the running world.
    uintptr_t instance = 0;
    int32_t instanceIdx = -1;
    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string objClsName = UObjectLookup::GetObjectClassName(obj);
        if (objClsName != className) continue;
        std::string objName = UObjectLookup::GetObjectName(obj);
        if (objName.rfind("Default__", 0) == 0) continue;  // skip CDO
        instance = obj;
        instanceIdx = i;
        break;
    }

    if (!instance) {
        LOG_LAYOUT("%s: no live (non-Default__) instance in GObjects", className);
        return;
    }

    LOG_LAYOUT("=== %s live instance #%d @ 0x%p ===", className, instanceIdx, (void*)instance);

    // Dump the first 0x200 bytes in hex, 16 bytes per row, with ASCII to
    // the right. SEH-guarded in case the instance is smaller than 0x200.
    constexpr int kDumpBytes = 0x200;
    uint8_t buf[kDumpBytes] = {};
    int readOK = SafeReadBytes(instance, buf, kDumpBytes);

    for (int row = 0; row < readOK; row += 16) {
        LOG_LAYOUT("  +0x%03X", row);
        for (int c = 0; c < 16 && row + c < readOK; ++c)
            printf("%02X ", buf[row + c]);
        printf(" ");
        for (int c = 0; c < 16 && row + c < readOK; ++c) {
            uint8_t b = buf[row + c];
            printf("%c", (b >= 0x20 && b < 0x7F) ? b : '.');
        }
        printf("\n");
    }
    if (readOK < kDumpBytes)
        LOG_LAYOUT("  (truncated at +0x%X)", readOK);

    // TArray scanner. At each 8-byte aligned offset check whether the
    // slot looks like { void* Data, int32 Count, int32 Max }: Data is a
    // plausible heap pointer, Count is in [0, 10000], Max >= Count.
    // For each hit we decode the first up-to-8 elements TWO WAYS: first
    // as raw UObject pointers (TArray<UObject*>), second as FWeakObjectPtr
    // (TArray<TWeakObjectPtr<UObject>> — 8 bytes { int32 Index, int32
    // Serial }). Whichever interpretation yields resolvable UObjects with
    // recognizable class names tells us which TArray is the one we want.
    LOG_LAYOUT("Scanning for TArray-shaped fields:");
    int candidates = 0;
    int32_t gObjCount = UObjectLookup::GetObjectCount();
    for (int off = 0x30; off + 16 <= readOK; off += 8) {
        uintptr_t data = *reinterpret_cast<uintptr_t*>(buf + off);
        int32_t   cnt  = *reinterpret_cast<int32_t*>(buf + off + 8);
        int32_t   mx   = *reinterpret_cast<int32_t*>(buf + off + 12);
        if (!LooksLikeHeapPtr(data)) continue;
        if (cnt < 0 || cnt > 10000) continue;
        if (mx  < cnt || mx > 10000) continue;

        // Read up to 8 8-byte elements from Data. Each is EITHER a raw
        // ptr OR a weak-ptr { idx, serial } — we'll show both decodings.
        constexpr int kMaxSamples = 8;
        int sampleCount = cnt < kMaxSamples ? cnt : kMaxSamples;
        uintptr_t samples[kMaxSamples] = {};
        int readPtrs = SafeReadPtrArray(data, samples, sampleCount);

        LOG_LAYOUT("  +0x%03X TArray data=0x%p count=%d max=%d (sampling %d)", off, (void*)data, cnt, mx, readPtrs);

        for (int i = 0; i < readPtrs; ++i) {
            uintptr_t raw = samples[i];
            // Interpretation A: raw UObject*
            const char* asPtrClass = "-";
            std::string asPtrClassStr;
            if (LooksLikeHeapPtr(raw)) {
                asPtrClassStr = UObjectLookup::GetObjectClassName(raw);
                if (!asPtrClassStr.empty()) asPtrClass = asPtrClassStr.c_str();
            }
            // Interpretation B: FWeakObjectPtr { int32 Index, int32 Serial }
            int32_t wIdx  = static_cast<int32_t>(raw & 0xFFFFFFFFull);
            int32_t wSer  = static_cast<int32_t>((raw >> 32) & 0xFFFFFFFFull);
            uintptr_t wObj = 0;
            std::string wClsStr;
            std::string wNameStr;
            if (wIdx > 0 && wIdx < gObjCount) {
                wObj = UObjectLookup::GetObjectByIndex(wIdx);
                if (wObj) {
                    wClsStr  = UObjectLookup::GetObjectClassName(wObj);
                    wNameStr = UObjectLookup::GetObjectName(wObj);
                }
            }
            const char* wCls  = wClsStr.empty()  ? "-" : wClsStr.c_str();
            const char* wName = wNameStr.empty() ? "-" : wNameStr.c_str();

            LOG_LAYOUT("    [%d] raw=0x%016llx  asPtr:%-24s  asWeak:idx=%d ser=%d -> %s (%s)", i, (unsigned long long)raw, asPtrClass, wIdx, wSer, wName, wCls);
        }
        candidates++;
    }
    LOG_LAYOUT("%d TArray candidate(s) at +0x30..+0x%X", candidates, readOK);
    LOG_LAYOUT("=== %s done ===", className);
}

// =============================================================================
// Tick subsystem discovery
//
// The crafts-don't-progress bug is because processors we queue via ARPC are
// never visited by the per-frame tick — setting bProcessorActive=1 is a flag,
// not a subsystem registration. The real registration happens via a write to
// an internal TArray on a UDeployableTickSubsystem (or similarly-named class)
// that the game's world subsystem manager maintains.
//
// We can't call OnServer_ActivateProcessor from within an ARPC thunk because
// of script VM reentrance (proved by the 0xFFFFFFFFFFFFFFFF CallFunction
// crash). Instead we will locate the subsystem's active-processor TArray via
// UObjectLookup reflection and push to it with a direct memory write (no
// UFunction call, no BP VM, no reentrance).
//
// This function is the discovery phase: it enumerates every candidate class
// name and dumps its property layout + function list so the next iteration
// can hardcode the exact names of the field and register method.
// =============================================================================
static void DiscoverTickSubsystem() {
    if (!UObjectLookup::IsInitialized()) return;
    LOG_DISC("=== Scanning for tick / deployable / processing subsystem classes ===");

    // Broad enumeration — log every UClass whose name contains these
    // substrings. Case-insensitive. Covers the typical Icarus/UE naming.
    const char* substrings[] = {
        "DeployableTick",
        "DeployableSubsystem",
        "ProcessorSubsystem",
        "ProcessingSubsystem",
        "DeployableManager",
        "ProcessingManager",
        "IcarusWorldSubsystem",
        "IcarusGameSubsystem",
        "DeployableWorldSubsystem",
        "ProcessorWorldSubsystem",
    };
    for (const char* s : substrings) {
        UObjectLookup::DumpClassesContaining(s, 20);
    }

    // For each plausible candidate name, try resolving the UClass and if
    // found, dump full property layout (including parent classes) and full
    // UFunction list. Hits on multiple candidates are fine — we just want
    // the output so we can see what actually exists.
    const char* candidates[] = {
        "DeployableTickSubsystem",
        "IcarusDeployableTickSubsystem",
        "DeployableSubsystem",
        "IcarusDeployableSubsystem",
        "ProcessorSubsystem",
        "IcarusProcessorSubsystem",
        "ProcessingSubsystem",
        "IcarusProcessingSubsystem",
        "DeployableManagerSubsystem",
        "DeployableWorldSubsystem",
        "ProcessingWorldSubsystem",
        "IcarusWorldSubsystem",
    };
    for (const char* name : candidates) {
        uintptr_t cls = UObjectLookup::FindClassByName(name);
        if (!cls) continue;
        LOG_DISC("FOUND class '%s' @ UClass=0x%p", name, (void*)cls);
        DumpClassProperties(name);
        UObjectLookup::DumpFunctionsOf(cls, 100);
    }

    // Also dump any live UObject whose CLASS NAME contains 'Deployable' +
    // 'Subsystem' — this catches the actual instance even if its UClass
    // sits under an unexpected outer chain. We log at most 20 hits.
    LOG_DISC("=== Searching GObjects for live subsystem instances ===");
    int total = UObjectLookup::GetObjectCount();
    int hits = 0;
    for (int32_t i = 0; i < total && hits < 20; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string clsName = UObjectLookup::GetObjectClassName(obj);
        if (clsName.empty()) continue;
        // Look for any class name containing both "Subsystem" and either
        // "Deployable" or "Processor" or "Processing".
        std::string lower = clsName;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.find("subsystem") == std::string::npos) continue;
        if (lower.find("deployable") == std::string::npos &&
            lower.find("processor") == std::string::npos &&
            lower.find("processing") == std::string::npos) continue;
        std::string objName = UObjectLookup::GetObjectName(obj);
        LOG_DISC("  instance #%d class=%s name=%s @ 0x%p", i, clsName.c_str(), objName.c_str(), (void*)obj);
        hits++;
    }
    LOG_DISC("=== %d instance match(es) ===", hits);

    // Raw layout inspection: for each subsystem class the active list of
    // processors lives in an unreflected C++ TArray member, so we scan
    // the live instance's raw memory for a TArray-shaped field.
    InspectSubsystemInstanceLayout("DeployableTickSubsystem");
    InspectSubsystemInstanceLayout("DeployableSubsystem");
    InspectSubsystemInstanceLayout("DeployableManagerSubsystem");

    // Dump UFunctions on inventory-related classes. We need to find the
    // function the game's craft-completion path calls to locate a slot
    // containing a specific item type — for crafts where the player has
    // 0 of an input, this function returns -1 and the completion aborts
    // before delivering the output. By hooking it to return slot 0, our
    // existing ConsumeItem hook no-ops the actual decrement and output
    // delivery proceeds.
    LOG_DISC("=== Inventory class function dumps ===");
    const char* invClassCandidates[] = {
        "Inventory",
        "InventoryComponent",
        "InventoryContainerComponent",
        "InventoryFunctionLibrary",
        "InventoryItemLibrary",
        "CraftingFunctionLibrary",
        "ProcessingFunctionLibrary",
    };
    for (const char* cn : invClassCandidates) {
        uintptr_t cls = UObjectLookup::FindClassByName(cn);
        if (!cls) {
            LOG_DISC("  %s: class not found", cn);
            continue;
        }
        LOG_DISC("  %s @ 0x%p:", cn, (void*)cls);
        UObjectLookup::DumpFunctionsOf(cls, 200);
    }

    // Also dump ProcessingComponent functions — we know some by name but
    // there might be more we missed (input-validate / consume-helper).
    LOG_DISC("=== ProcessingComponent function dump ===");
    uintptr_t pcCls = UObjectLookup::FindClassByName("ProcessingComponent");
    if (pcCls) UObjectLookup::DumpFunctionsOf(pcCls, 200);

    LOG_DISC("=== end ===");
}

void Trainer::Initialize() {
    Log::InitConsole();
    LOG_OK("ZeusMod trainer loaded into Icarus-Win64-Shipping.exe");
    LOG_INFO("DLL base probed, spinning up subsystems");
    StartPipeServer();

    // Bring up the UE4 reflection-based name lookup BEFORE FindPlayer.
    // The thunk-to-impl resolver uses HDE64 to walk thunk bytecode, identify
    // the result-buffer register from the prologue, and find the CALL that
    // precedes a write to [resultReg] — that's the C++ impl. This makes the
    // entire resolution chain patch-proof across game updates.
    if (UObjectLookup::Initialize()) {
        LOG_INFO("UObjectLookup ready (name-based resolution active)");
        ResolveAllOffsets();
        // One-shot: dump every UProperty of ProcessingComponent with its
        // offset so we can decode any memory region on that instance by
        // name instead of guessing. Used in combination with the
        // pre/post byte-diff on HookCanQueueItem below.
        DumpClassProperties("ProcessingComponent");
        // Discover the subsystem that owns the per-frame processor tick.
        // Outputs the full class hierarchy, properties and functions of
        // any class matching tick/deployable/processor subsystem naming.
        DiscoverTickSubsystem();
        // Cache the live DeployableTickSubsystem instance and validate
        // the ProcessingComponent list at +0x60. If the prospect isn't
        // loaded yet this is a no-op and we retry lazily on the first
        // ARPC hit.
        ResolveAndValidateTickSubsystem();
    } else {
        LOG_INFO("UObjectLookup failed — falling back to hardcoded offsets");
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

void Trainer::RunOnceDiagnostics() {
    static bool s_diagDumped = false;
    if (s_diagDumped) return;
    s_diagDumped = true;

    LOG_OK("player components cached");
    LOG_DEBUG("  ExperienceComponent           = 0x%p", m_expComp);
    LOG_DEBUG("  PlayerTalentControllerComp    = 0x%p", m_playerTalentCtrl);
    LOG_DEBUG("  SoloTalentControllerComp      = 0x%p", m_soloTalentCtrl);
    LOG_DEBUG("  BlueprintTalentModel (live)   = 0x%p", m_playerTalentModel);
    LOG_DEBUG("  SoloTalentModel (live)        = 0x%p", m_soloTalentModel);

    LOG_SECTION("Class properties");
    DumpClassProperties("ExperienceComponent");
    DumpClassProperties("PlayerTalentControllerComponent");
    DumpClassProperties("SoloTalentControllerComponent");
    DumpClassProperties("TalentControllerComponent");

    LOG_SECTION("TalentController.Model follow-up");
    {
        void* pm = SafeReadPtrAt(m_playerTalentCtrl, 0xC8);
        if (pm) {
            std::string cn = UObjectLookup::GetObjectClassName((uintptr_t)pm);
            LOG_OK("Player Model = 0x%p class=%s", pm, cn.c_str());
            if (!cn.empty()) DumpClassProperties(cn.c_str());
        } else {
            LOG_WARN("Player: Model @ +0xC8 is null");
        }
        void* sm = SafeReadPtrAt(m_soloTalentCtrl, 0xC8);
        if (sm) {
            std::string cn = UObjectLookup::GetObjectClassName((uintptr_t)sm);
            LOG_OK("Solo Model = 0x%p class=%s", sm, cn.c_str());
            if (!cn.empty()) DumpClassProperties(cn.c_str());
        } else {
            LOG_WARN("Solo: Model @ +0xC8 is null");
        }
    }

    LOG_SECTION("Enumerate controller instances with Model probe @ +0xC8");
    EnumerateLiveInstancesWithProbe("PlayerTalentControllerComponent", 0xC8, 10);
    EnumerateLiveInstancesWithProbe("SoloTalentControllerComponent",   0xC8, 10);

    LOG_SECTION("Player/Solo TalentModel (resolved via controller +0xC8)");
    auto dumpModelAndFunctions = [](const char* label, void* model) {
        if (!model) { LOG_WARN("%s model is null", label); return; }
        std::string cn = UObjectLookup::GetObjectClassName((uintptr_t)model);
        std::string on = UObjectLookup::GetObjectName((uintptr_t)model);
        LOG_OK("%s model = 0x%p name=%s class=%s",
               label, model, on.c_str(), cn.c_str());
        if (cn.empty()) return;

        DumpClassProperties(cn.c_str());

        // Dump UFunctions on the model class so we can find callable
        // things like AddPoints / GrantTalentPoints / SetUnspentPoints.
        uintptr_t cls = UObjectLookup::FindClassByName(cn.c_str());
        if (cls) {
            LOG_INFO("%s: dumping UFunctions on %s", label, cn.c_str());
            UObjectLookup::DumpFunctionsOf(cls, 120);
        }

        HexDump((std::string(label) + " bytes").c_str(), model, 0x300);
    };
    dumpModelAndFunctions("Player", m_playerTalentModel);
    dumpModelAndFunctions("Solo",   m_soloTalentModel);

    LOG_SECTION("TalentController UFunctions + raw layout");
    if (m_playerTalentCtrl) {
        uintptr_t pcCls = UObjectLookup::FindClassByName("PlayerTalentControllerComponent");
        if (pcCls) UObjectLookup::DumpFunctionsOf(pcCls, 80);
        HexDump("PlayerTalentController bytes", m_playerTalentCtrl, 0x200);
    }
    if (m_soloTalentCtrl) {
        uintptr_t scCls = UObjectLookup::FindClassByName("SoloTalentControllerComponent");
        if (scCls) UObjectLookup::DumpFunctionsOf(scCls, 80);
        HexDump("SoloTalentController bytes", m_soloTalentCtrl, 0x200);
    }
    uintptr_t baseCls = UObjectLookup::FindClassByName("TalentControllerComponent");
    if (baseCls) {
        LOG_INFO("Base TalentControllerComponent UFunctions:");
        UObjectLookup::DumpFunctionsOf(baseCls, 80);
    }

    LOG_SECTION("TalentView widget classes (UMG)");
    UObjectLookup::DumpClassesContaining("TalentView", 20);
    UObjectLookup::DumpClassesContaining("TalentWidget", 20);
    UObjectLookup::DumpClassesContaining("TalentModel",  20);
    // If we can find a live widget, dump its class + bytes so we can
    // see the actual text/int bindings used by the UMG designer.
    for (const char* wname : {"UMG_TalentView_Solo_C", "UMG_TalentView_Player_C",
                              "UMG_TalentView_C", "UMG_PlayerTalents_C",
                              "UMG_SoloTalents_C"}) {
        void* w = FindFirstLiveInstance(wname);
        if (w) {
            LOG_OK("Live widget %s = 0x%p", wname, w);
            DumpClassProperties(wname);
            HexDump(wname, w, 0x200);
        }
    }

    LOG_SECTION("MetaResources");
    if (m_playerState) {
        DumpMetaResourceArray("ActiveCharacter.MetaResources",
            (uintptr_t)m_playerState + Off::PS_ActiveCharacter + Off::OPC_MetaResources);
        DumpMetaResourceArray("ActiveUserProfile.MetaResources",
            (uintptr_t)m_playerState + Off::PS_ActiveUserProfile + Off::OPU_MetaResources);
    }
}

void Trainer::FindPlayer() {
    // Find SetHealth and dump the actual bytes to see what we're patching
    if (!m_setHealthAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz, kSetHealthWriteAob);
        if (match) {
            // Dump bytes to verify what we're patching
            LOG_PATCH("Pattern found at 0x%p", (void*)match);
            LOG_PATCH("Bytes:");
            uint8_t* p = reinterpret_cast<uint8_t*>(match);
            for (int i = 0; i < 22; i++) printf("%02X ", p[i]);
            printf("\n");

            // Find the exact position of "89 XX D8 01 00 00" within the match
            for (int i = 0; i < 20; i++) {
                if (p[i] == 0x89 && p[i+2] == 0xD8 && p[i+3] == 0x01 && p[i+4] == 0x00 && p[i+5] == 0x00) {
                    m_setHealthAddr = match + i;
                    LOG_PATCH("Found health write at offset +%d -> 0x%p", i, (void*)m_setHealthAddr);
                    break;
                }
            }
            if (!m_setHealthAddr) {
                LOG_PATCH("Could not find 89 XX D8 01 00 00 in pattern!");
            }
        } else {
            LOG_PATCH("SetHealth AOB not found!");
        }
    }

    if (!Off::World_GameInstance || !Off::GI_LocalPlayers || !Off::Player_Controller ||
        !Off::Ctrl_Character || !Off::Char_ActorState || !Off::State_Health ||
        !Off::State_MaxHealth || !Off::State_Stamina || !Off::State_MaxStamina) {
        static bool loggedMissingCore = false;
        if (!loggedMissingCore) {
            loggedMissingCore = true;
            LOG_INFO("Core runtime offsets unresolved, player scan skipped");
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
            LOG_HOOK("Material gate candidate: GetResourceRecipeValidity=%p HasSufficientResource=%p", reinterpret_cast<void*>(s_getResourceRecipeValidityAddr),
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
            LOG_PATCH("ConsumeItem sub at 0x%p", (void*)m_removeItemAddr);
        } else {
            LOG_PATCH("ConsumeItem AOB not found");
        }
    }

    // (DIAG block removed — those addresses were UFunction thunks, not real
    // enclosing functions. We now resolve impls via UObjectLookup.)

    LOG_INFO("scanning for player...");

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

                // PlayerState is on the controller and may be null very
                // briefly after connect; we simply try to cache it here and
                // Tick() bails out of the meta-progression features when
                // m_playerState is still null.
                if (Off::Ctrl_PlayerState) {
                    m_playerState = ReadAt<void*>(pc, Off::Ctrl_PlayerState);
                }

                // Cache the live components we now know from the scan:
                //   - ExperienceComponent lives in the player's
                //     BlueprintCreatedComponents at index 20 in the live
                //     build, but we look it up by class name so it stays
                //     patch-resilient.
                //   - PlayerTalentControllerComponent and
                //     SoloTalentControllerComponent are NOT on the player
                //     character component lists; they are standalone
                //     UObjects in GObjects, so we walk GObjects for the
                //     first non-CDO instance of each.
                m_expComp          = FindCharacterComponentByClass(ch, "ExperienceComponent");
                // Pick the TalentController instance whose Model pointer
                // at +0xC8 is non-null — Icarus keeps several dormant
                // shells per class in GObjects and only the live
                // gameplay one has a wired Model.
                m_playerTalentCtrl = FindFirstInstanceWithNonNullProbe("PlayerTalentControllerComponent", 0xC8);
                m_soloTalentCtrl   = FindFirstInstanceWithNonNullProbe("SoloTalentControllerComponent",   0xC8);
                // Once we have the correct controllers, the live models
                // are simply *(ctrl + 0xC8). Anything else picked up by a
                // plain GObjects scan is a sibling for a different view.
                m_playerTalentModel = SafeReadPtrAt(m_playerTalentCtrl, 0xC8);
                m_soloTalentModel   = SafeReadPtrAt(m_soloTalentCtrl,   0xC8);

                // One-shot diagnostic dumps live in their own method so
                // FindPlayer itself stays free of C++ object unwinding
                // (it already uses __try around the gwPtr walk).
                RunOnceDiagnostics();

                LOG_OK("player resolved (candidate #%d)", candidates);
                LOG_DEBUG("  Character   = 0x%p", ch);
                LOG_DEBUG("  ActorState  = 0x%p", as);
                LOG_DEBUG("  PlayerState = 0x%p", m_playerState);
                LOG_INFO("Health %d/%d  Stamina %d/%d  Armor %d/%d",
                    hp, maxHp, sta, maxSta,
                    ReadAt<int>(as, Off::State_Armor),
                    ReadAt<int>(as, Off::State_MaxArmor));
                return;
            }

            if (candidates < 5) {
                LOG_DEBUG("candidate #%d rejected: HP=%d/%d STA=%d/%d",
                    candidates, hp, maxHp, sta, maxSta);
            }
        }
        __except (1) {}

        next:
        candidates++;
        scanPos = hit + 1;
        remain = (base + size) - scanPos;
    }

    LOG_INFO("Not found (%d scanned). Retry in 2s.", candidates);
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
                    LOG_PATCH("Health patch was restored by game! Re-patching...");
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
                LOG_DEBUG("First tick - cur=%d max=%d", curArmor, maxArmor);
                if (maxArmor <= 0) {
                    LOG_DEBUG("MaxArmor is 0 - equip armor first or it has nothing to fill");
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

        // Stable Temperature — writes BOTH InternalTemperature (raw, what
        // the sim mutates) AND ModifiedInternalTemperature (the
        // post-modifier HUD value). The game stores temperature in
        // centi-degrees (2395 = 23.95 C), so StableTempValue in degrees
        // is multiplied by 100 on write.
        if (StableTemperature) {
            const int target = StableTempValue * 100;
            static bool s_tempLogged = false;
            if (!s_tempLogged) {
                s_tempLogged = true;
                int bInt = Off::State_InternalTemp    ? ReadAt<int>(m_actorState, Off::State_InternalTemp)    : -9999;
                int bMod = Off::State_ModInternalTemp ? ReadAt<int>(m_actorState, Off::State_ModInternalTemp) : -9999;
                if (Off::State_InternalTemp)    WriteAt<int>(m_actorState, Off::State_InternalTemp,    target);
                if (Off::State_ModInternalTemp) WriteAt<int>(m_actorState, Off::State_ModInternalTemp, target);
                int aInt = Off::State_InternalTemp    ? ReadAt<int>(m_actorState, Off::State_InternalTemp)    : -9999;
                int aMod = Off::State_ModInternalTemp ? ReadAt<int>(m_actorState, Off::State_ModInternalTemp) : -9999;
                LOG_TEMP("user=%dC store=%d  Internal %d -> %d  ModInternal %d -> %d",
                         StableTempValue, target, bInt, aInt, bMod, aMod);
            } else {
                if (Off::State_InternalTemp)    WriteAt<int>(m_actorState, Off::State_InternalTemp,    target);
                if (Off::State_ModInternalTemp) WriteAt<int>(m_actorState, Off::State_ModInternalTemp, target);
            }
        }

        // Mega Exp — continuously grants +50 000 XP per tick so the XP
        // bar visibly fills and the game's native level-up path awards
        // levels + points normally. At 60 FPS that is +3 000 000 XP/sec,
        // cap at 9 999 999 to avoid int32 weirdness near INT_MAX. Also
        // mirrors Level = 60 as a belt-and-suspenders fallback in case
        // the client HUD is decoupled from the XP recompute. A periodic
        // status log (every ~2 seconds) lets us confirm persistence.
        if (MegaExp) {
            static bool s_megaLogged = false;
            static int  s_megaTick   = 0;
            if (!s_megaLogged) {
                s_megaLogged = true;
                int bXp  = Off::State_TotalExp ? ReadAt<int>(m_actorState, Off::State_TotalExp) : -1;
                int bLvl = Off::State_Level    ? ReadAt<int>(m_actorState, Off::State_Level)    : -1;
                LOG_MEGA("enabled: TotalExp=%d Level=%d  granting +50000 xp/tick", bXp, bLvl);
            }
            if (Off::State_TotalExp) {
                int cur = ReadAt<int>(m_actorState, Off::State_TotalExp);
                long long bumped = (long long)cur + 50000;
                if (bumped > 9999999) bumped = 9999999;
                WriteAt<int>(m_actorState, Off::State_TotalExp, (int)bumped);
            }
            if (Off::State_Level) {
                int curLvl = ReadAt<int>(m_actorState, Off::State_Level);
                if (curLvl < 60) WriteAt<int>(m_actorState, Off::State_Level, 60);
            }
            if (++s_megaTick >= 120) {
                s_megaTick = 0;
                int xp  = Off::State_TotalExp ? ReadAt<int>(m_actorState, Off::State_TotalExp) : -1;
                int lvl = Off::State_Level    ? ReadAt<int>(m_actorState, Off::State_Level)    : -1;
                LOG_MEGA("status: TotalExp=%d Level=%d", xp, lvl);
            }
        }

        // Max Talent / Tech / Solo points — the live Talent Models have
        // a handful of int32 slots in their state-cache block at +0x80,
        // +0x84, +0x90, +0x94, +0xB0, +0xB4, +0xC0, +0xD0, +0xD8. We
        // know +0xD8 matches the user's reported Solo points but
        // writing it alone doesn't move the UI, so the UMG widget is
        // reading through a different path (possibly a getter the
        // refresh hooks below feed). We flood every candidate each tick
        // **and** hook the four refresh UFunctions so that whichever
        // the widget ends up polling will see 99 999. Pointer / bitmask
        // slots (+0x88, +0x98, +0xB8, +0xC8, +0xE0) are deliberately
        // skipped to avoid corruption.
        if ((MaxTalentPoints || MaxTechPoints) && m_playerTalentCtrl) {
            ClampTalentModelAvailablePoints(m_playerTalentCtrl, 99999);
        }
        if (MaxSoloPoints && m_soloTalentCtrl) {
            ClampTalentModelAvailablePoints(m_soloTalentCtrl, 99999);
        }

        // Periodic status log — prints every int32 slot we're clamping
        // so we can watch which field the UI actually reacts to.
        static int s_talentStatusTick = 0;
        if (++s_talentStatusTick >= 120) {
            s_talentStatusTick = 0;
            auto printModel = [](const char* label, void* ctrl) {
                if (!ctrl) return;
                __try {
                    void* model = *(void**)((uintptr_t)ctrl + 0xC8);
                    if (!model) return;
                    int v80 = *(int*)((uintptr_t)model + 0x80);
                    int v84 = *(int*)((uintptr_t)model + 0x84);
                    int v90 = *(int*)((uintptr_t)model + 0x90);
                    int v94 = *(int*)((uintptr_t)model + 0x94);
                    int vB0 = *(int*)((uintptr_t)model + 0xB0);
                    int vB4 = *(int*)((uintptr_t)model + 0xB4);
                    int vC0 = *(int*)((uintptr_t)model + 0xC0);
                    int vD0 = *(int*)((uintptr_t)model + 0xD0);
                    int vD8 = *(int*)((uintptr_t)model + 0xD8);
                    LOG_DEBUG("%s model: 80=%d 84=%d 90=%d 94=%d B0=%d B4=%d C0=%d D0=%d D8=%d",
                              label, v80, v84, v90, v94, vB0, vB4, vC0, vD0, vD8);
                }
                __except (1) {}
            };
            printModel("Player", m_playerTalentCtrl);
            printModel("Solo",   m_soloTalentCtrl);
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
                LOG_DEBUG("TimeLock ENABLED, target=%.2f, m_gworldPtr=%p", LockedTime, m_gworldPtr);
            } else if (!TimeLock && wasTimeLocked) {
                wasTimeLocked = false;
                LOG_DEBUG("TimeLock DISABLED");
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
                    LOG_DEBUG("Real GWorld symbol resolved at %p (AOB hit=%p)", cachedRealGWorld, (void*)hit);
                } else {
                    LOG_DEBUG("ERROR: real GWorld AOB not found");
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
                                LOG_DEBUG("world=%p gs=%p spd=%d hour=%.2f -> %.2f", world, gameState, secondsPerDay, hour24, timeValue);
                            }
                        } else {
                            static bool loggedNullGS = false;
                            if (!loggedNullGS) {
                                loggedNullGS = true;
                                LOG_DEBUG("ERROR: GameState null at world+0x%llX (not in prospect?)", (unsigned long long)Off::World_GameState);
                            }
                        }
                    } else {
                        static bool loggedNullWorld = false;
                        if (!loggedNullWorld) {
                            loggedNullWorld = true;
                            LOG_DEBUG("ERROR: real GWorld is null");
                        }
                    }
                } __except(1) {
                    static bool loggedEx = false;
                    if (!loggedEx) {
                        loggedEx = true;
                        LOG_DEBUG("Exception in Time Lock tick");
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
            LOG_FC("%s", FreeCraft ? "enabled" : "disabled");
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
            // Processor kick removed: Trainer::Tick runs on our DLL worker
            // thread (see dllmain.cpp MainThread). Calling
            // OnServer_ActivateProcessor / OnServer_StartProcessing from here
            // crashes biofuel/processor crafts because those RPCs must run on
            // the UE game thread. The gate-permissive hooks are enough — the
            // game's own processing tick will pick up the queue naturally.
            //
            // Post-ARPC watcher: reads state (Q/A/PI/MJ) of the last queued
            // processor every tick, logs transitions until a 3s deadline.
            // Pure read, SEH-guarded — safe to run from the worker thread.
            PollArpcTrackedProcessor();
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
            LOG_PATCH("SetHealth bytes mismatch: %02X %02X %02X - skipping", check[0], check[1], check[2]);
            return;
        }
        // Save 6 original bytes and NOP the write instruction
        memcpy(m_setHealthBackup, reinterpret_cast<void*>(m_setHealthAddr), 6);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, PAGE_EXECUTE_READWRITE, &oldP);
        memset(reinterpret_cast<void*>(m_setHealthAddr), 0x90, 6); // 6x NOP
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, oldP, &oldP);
        m_setHealthPatched = true;
        LOG_PATCH("Health write NOPed (god mode ON)");
    }
    else if (!enable && m_setHealthPatched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(m_setHealthAddr), m_setHealthBackup, 6);
        VirtualProtect(reinterpret_cast<void*>(m_setHealthAddr), 6, oldP, &oldP);
        m_setHealthPatched = false;
        LOG_PATCH("Health write restored (god mode OFF)");
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
        LOG_PATCH("%s patched", name);
    } else if (!enable && patched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(addr), backup, size);
        VirtualProtect(reinterpret_cast<void*>(addr), size, oldP, &oldP);
        patched = false;
        LOG_PATCH("%s restored", name);
    }
}

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
    LOG_PIPE("Server started on \\\\.\\pipe\\ZeusModPipe");

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
                    else if (strcmp(cmd, "temp") == 0) self->StableTemperature = (v != 0);
                    else if (strcmp(cmd, "temp_val") == 0) self->StableTempValue = v;
                    else if (strcmp(cmd, "megaexp") == 0) self->MegaExp = (v != 0);
                    else if (strcmp(cmd, "talent") == 0) self->MaxTalentPoints = (v != 0);
                    else if (strcmp(cmd, "tech") == 0) self->MaxTechPoints = (v != 0);
                    else if (strcmp(cmd, "solo") == 0) self->MaxSoloPoints = (v != 0);

                    LOG_PIPE("%s = %s", cmd, val);
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
