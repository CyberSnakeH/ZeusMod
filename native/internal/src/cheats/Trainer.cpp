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
#include <psapi.h>
#include <unordered_map>

// File-scope cache for InfiniteItems' per-row durability lock. Lives here
// rather than as a function-local static because MSVC refuses __try in a
// function whose scope introduces C++ unwind edges (C2712) — and the
// static-local guard for an unordered_map constructor counts.
static std::unordered_map<uint32_t, int32_t> g_conditionMaxByRow;

// ============================================================================
// Trainer lifecycle + per-tick dispatcher + core (non-FreeCraft) patches +
// named-pipe IPC server. Free-craft logic lives in TrainerFreeCraft.cpp;
// UProperty offset resolution in TrainerResolve.cpp; reflection/introspection
// helpers in TrainerDiagnostics.cpp. All three sister TUs call through this
// file's Trainer class and reuse AOBs + helpers from TrainerInternal.h.
// ============================================================================

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
        // ONE-SHOT recon: look for cheat/give/spawn/admin style functions
        // so we can implement a real GiveItem cheat. Dumps the full UFunction
        // table of each candidate class. We're looking for reflected names
        // like "GiveItem", "SpawnItem", "Cheat_GiveItem", "AddItemByRow",
        // "OnServer_GiveItem", etc. Output goes to the same [UOBJ] log tag
        // so you can grep for it. This is verbose but one-shot.
        printf("\n[RECON] ======= GiveItem candidate discovery =======\n");
        {
            const char* cheatClasses[] = {
                "CheatManager",                 // UE base
                "IcarusCheatManager",           // likely subclass
                "IcarusPlayerController",       // might carry server RPCs
                "IcarusPlayerState",
                "IcarusGameMode",
                "IcarusGameState",
                "IcarusPlayerCharacter",        // inventory lives here
                "Inventory",
                "InventoryComponent",
                "InventoryContainerComponent"
            };
            for (const char* cls : cheatClasses) {
                uintptr_t ca = UObjectLookup::FindClassByName(cls);
                if (ca) {
                    printf("[RECON] --- %s (0x%p) ---\n", cls, (void*)ca);
                    UObjectLookup::DumpFunctionsOf(ca, 300);
                } else {
                    printf("[RECON] %s NOT FOUND\n", cls);
                }
            }
            printf("[RECON] --- classes containing 'Cheat' ---\n");
            UObjectLookup::DumpClassesContaining("Cheat", 50);
            printf("[RECON] --- classes containing 'Give' ---\n");
            UObjectLookup::DumpClassesContaining("Give", 50);
            printf("[RECON] --- classes containing 'Admin' ---\n");
            UObjectLookup::DumpClassesContaining("Admin", 50);

            // === Phase 1 recon: function signatures + DataTable enumeration =====
            printf("[RECON] --- UFunction signatures (params we need to fill) ---\n");
            UObjectLookup::DumpUFunctionSignatureByName("InventoryItemLibrary", "CreateItem");
            UObjectLookup::DumpUFunctionSignatureByName("InventoryItemLibrary", "CreateCustomItem");
            UObjectLookup::DumpUFunctionSignatureByName("InventoryItemLibrary", "ConvertToItem");
            UObjectLookup::DumpUFunctionSignatureByName("InventoryItemLibrary", "FindOrAssignNewID");
            UObjectLookup::DumpUFunctionSignatureByName("Inventory", "ManuallyPlaceItem");
            UObjectLookup::DumpUFunctionSignatureByName("Inventory", "ManuallyForcePlaceItem");
            UObjectLookup::DumpUFunctionSignatureByName("Inventory", "ForceAddItems");
            UObjectLookup::DumpUFunctionSignatureByName("Inventory", "FindEmptyLocation");
            UObjectLookup::DumpUFunctionSignatureByName("Inventory", "SetItemDynamicProperty");

            printf("[RECON] --- DataTable instances (looking for item templates) ---\n");
            // Try likely substrings for the item data table name
            UObjectLookup::DumpDataTableInstances("Item", 80);
            printf("[RECON] --- DataTable instances matching 'Template' ---\n");
            UObjectLookup::DumpDataTableInstances("Template", 40);
            printf("[RECON] --- DataTable instances matching 'Resource' ---\n");
            UObjectLookup::DumpDataTableInstances("Resource", 40);
            printf("[RECON] --- all DataTable instances (first 40) ---\n");
            UObjectLookup::DumpDataTableInstances("", 40);

            // === Phase 2: struct layout + alternate item storage ===============
            printf("[RECON] --- ScriptStruct 'ItemData' layout ---\n");
            DumpClassProperties("ItemData");
            printf("[RECON] --- ScriptStruct 'MetaItem' layout ---\n");
            DumpClassProperties("MetaItem");
            printf("[RECON] --- ScriptStruct 'ItemDynamicData' layout ---\n");
            DumpClassProperties("ItemDynamicData");
            printf("[RECON] --- ScriptStruct 'ItemTemplateRowHandle' layout ---\n");
            DumpClassProperties("ItemTemplateRowHandle");

            printf("[RECON] --- classes/structs containing 'ItemTemplate' ---\n");
            UObjectLookup::DumpClassesContaining("ItemTemplate", 40);
            printf("[RECON] --- classes/structs containing 'ItemStatic' ---\n");
            UObjectLookup::DumpClassesContaining("ItemStatic", 40);
            printf("[RECON] --- classes/structs containing 'ItemType' ---\n");
            UObjectLookup::DumpClassesContaining("ItemType", 40);
            printf("[RECON] --- classes/structs containing 'AssetManager' ---\n");
            UObjectLookup::DumpClassesContaining("AssetManager", 10);
            printf("[RECON] --- classes/structs containing 'PrimaryAsset' ---\n");
            UObjectLookup::DumpClassesContaining("PrimaryAsset", 20);
            printf("[RECON] --- classes/structs containing 'DataAsset' (alt storage) ---\n");
            UObjectLookup::DumpClassesContaining("DataAsset", 20);
            printf("[RECON] --- classes/structs containing 'ResourceItem' ---\n");
            UObjectLookup::DumpClassesContaining("ResourceItem", 20);

            // === Phase 3: targeted dumps of the item template storage =========
            // Now that we know the storage is ItemTemplateLibrary + ItemTemplateTable
            // (Icarus-custom types, not UDataTable), dump their functions to
            // find a "GetRow" / "GetAllRows" helper we can call via ProcessEvent.
            printf("[RECON] --- ItemTemplateLibrary functions ---\n");
            {
                uintptr_t cls = UObjectLookup::FindClassByName("ItemTemplateLibrary");
                if (cls) UObjectLookup::DumpFunctionsOf(cls, 200);
                else     printf("[RECON] ItemTemplateLibrary NOT FOUND\n");
            }
            printf("[RECON] --- ItemTemplateTable functions ---\n");
            {
                uintptr_t cls = UObjectLookup::FindClassByName("ItemTemplateTable");
                if (cls) UObjectLookup::DumpFunctionsOf(cls, 200);
                else     printf("[RECON] ItemTemplateTable NOT FOUND\n");
            }
            printf("[RECON] --- ItemTemplateTable properties ---\n");
            DumpClassProperties("ItemTemplateTable");
            printf("[RECON] --- ItemTemplateLibrary properties ---\n");
            DumpClassProperties("ItemTemplateLibrary");

            // With fixed FindStructByName these now work for ScriptStructs.
            printf("[RECON] --- FItemData struct layout ---\n");
            DumpClassProperties("ItemData");
            printf("[RECON] --- FMetaItem struct layout ---\n");
            DumpClassProperties("MetaItem");
            printf("[RECON] --- FItemDynamicData struct layout ---\n");
            DumpClassProperties("ItemDynamicData");
            printf("[RECON] --- FItemTemplateRowHandle struct layout ---\n");
            DumpClassProperties("ItemTemplateRowHandle");
            printf("[RECON] --- FItemStaticData struct layout ---\n");
            DumpClassProperties("ItemStaticData");
            printf("[RECON] --- FItemStaticRow struct layout ---\n");
            DumpClassProperties("ItemStaticRow");

            // Live instances of ItemTemplateTable — there should be one global
            // holding all item templates. Its raw bytes at small offsets will
            // tell us where the row map / data lives.
            printf("[RECON] --- live ItemTemplateTable instances ---\n");
            EnumerateLiveInstancesWithProbe("ItemTemplateTable", 0x30, 20);

            // === Phase 4: final signature dumps for GiveItem pipeline =========
            printf("[RECON] --- key ItemTemplateLibrary signatures ---\n");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "NameToStruct");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "IntToStruct");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "NumRows");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "IsValidName");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "MakeLiteralItemTemplate");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "MakeItemTemplate");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "RowHandleToStruct");
            UObjectLookup::DumpUFunctionSignatureByName("ItemTemplateLibrary", "StructToRowHandle");

            // Dump the raw memory of the D_ItemTemplate singleton at the offsets
            // where DataTable stores RowStruct and RowMap. That gives us the
            // final bits needed: the map's data pointer to iterate/enumerate rows.
            printf("[RECON] --- D_ItemTemplate raw layout (first 0x100 bytes) ---\n");
            void* dItemTemplate = FindFirstLiveInstance("ItemTemplateTable");
            if (dItemTemplate) {
                printf("[RECON] D_ItemTemplate @ %p\n", dItemTemplate);
                HexDump("D_ItemTemplate", dItemTemplate, 0x100);
            } else {
                printf("[RECON] D_ItemTemplate NOT FOUND\n");
            }

            // === Phase 5: validate ProcessEvent resolution =====================
            // Use D_ItemTemplate as the 'self' to call ItemTemplateLibrary::NumRows
            // via ProcessEvent. If it returns 3054 (matching the TMap count read
            // from memory at +0x38) we know our ProcessEvent pointer is correct
            // and we can proceed to the full GiveItem pipeline.
            printf("[RECON] --- ProcessEvent validation ---\n");
            // Initialize the GiveItem subsystem (cache UFunction pointers + PE).
            Trainer_GiveItem_Init();
            Trainer_GiveItem_LogSamples();

            // Diagnostic: dump the GetInventory signature + walk the
            // InventoryComponent UClass super chain so we know how to find
            // the real UInventory used by the placement pipeline.
            UObjectLookup::DumpUFunctionSignatureByName("InventoryComponent", "GetInventory");
            {
                uintptr_t cls = UObjectLookup::FindClassByName("InventoryComponent");
                int hops = 0;
                while (cls && hops < 8) {
                    std::string n = UObjectLookup::GetObjectName(cls);
                    printf("[RECON] InventoryComponent super[%d] = %s\n", hops, n.c_str());
                    // UStruct class objects are permanent, raw read is safe.
                    uintptr_t super = *reinterpret_cast<uintptr_t*>(cls + 0x40);
                    if (!super || super == cls) break;
                    cls = super;
                    ++hops;
                }
            }

            if (dItemTemplate) {
                auto pe = UObjectLookup::ResolveProcessEvent(dItemTemplate);
                if (pe) {
                    printf("[RECON] ProcessEvent resolved at %p\n", (void*)pe);
                    // Independent confirmation: call NumRows via our helper
                    uintptr_t libCls = UObjectLookup::FindClassByName("ItemTemplateLibrary");
                    uintptr_t numRowsFn = libCls ?
                        UObjectLookup::FindFunctionInClass(libCls, "NumRows") : 0;
                    if (numRowsFn) {
                        int32_t rows = -1;
                        char buf[16]{};
                        if (UObjectLookup::CallUFunction(dItemTemplate, numRowsFn, buf)) {
                            rows = *reinterpret_cast<int32_t*>(buf);
                            printf("[RECON] CallUFunction NumRows() -> %d  (expect ~3054)\n", rows);
                        } else {
                            printf("[RECON] CallUFunction failed\n");
                        }
                    }
                } else {
                    printf("[RECON] ProcessEvent resolution FAILED\n");
                }
            }
        }
        printf("[RECON] ============================================\n\n");
        // Discover the subsystem that owns the per-frame processor tick.
        // Outputs the full class hierarchy, properties and functions of
        // any class matching tick/deployable/processor subsystem naming.
        DiscoverTickSubsystem();
        // Cache the live DeployableTickSubsystem instance and validate
        // the ProcessingComponent list at +0x60. If the prospect isn't
        // loaded yet this is a no-op and we retry lazily on the first
        // ARPC hit.
        Trainer_ResolveAndValidateTickSubsystem();
        // Install the source-hook that swallows Overburdened at creation
        // time when NoWeight is on. Relies on FindNativeFunction, so must
        // run AFTER UObjectLookup::Initialize succeeded.
        InstallWeightHook();
    } else {
        LOG_INFO("UObjectLookup failed — falling back to hardcoded offsets");
        Log::Resume::Note('E', "UObjectLookup failed to initialize — hardcoded offset fallback");
    }

    FindPlayer();

    // Surface everything worth summarising: item library size, recipes,
    // native function resolution status, etc. These come from other TUs
    // that track them during boot; we publish the final values here so
    // the Resume box has a single authoritative render point.
    Log::Resume::Set("Icarus module",   "%s",   "Icarus-Win64-Shipping.exe");
    if (m_character) Log::Resume::Set("player character", "0x%p", m_character);
    if (Off::Player_InventoryComp) Log::Resume::Set("InventoryComp offset", "+0x%llX",
        (unsigned long long)Off::Player_InventoryComp);
    Log::Resume::Set("Char_CurrentWeight",  "+0x%llX", (unsigned long long)Off::Char_CurrentWeight);
    Log::Resume::Set("FastArray_Slots",     "+0x%llX", (unsigned long long)Off::FastArray_Slots);
    Log::Resume::Set("DynProp Durability",  "%u",      (unsigned)Off::DynProp_Durability);
    Log::Resume::Set("DynProp ItemableStack","%u",     (unsigned)Off::DynProp_ItemableStack);

    Log::PrintResume("ZeusMod initialisation");
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

    // =================================================================
    // Live InventoryComponent layout dump — finds the REAL offset of
    // the player's slot array. Walks 0x800 bytes at 8-byte alignment
    // and flags every qword that parses as a plausible TArray head
    // {ptr, num, max} or a pointer to a known UObject class.
    // =================================================================
    LOG_SECTION("Player InventoryComponent layout");
    if (m_character && Off::Player_InventoryComp) {
        void* invComp = ReadAt<void*>(m_character, Off::Player_InventoryComp);
        if (invComp) {
            std::string icls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(invComp));
            LOG_INFO("[LAYT] Player InventoryComponent @ %p  class=%s", invComp, icls.c_str());
            HexDump("InventoryComponent", invComp, 0x400);

            // Scan for TArray<?>{data,num,max} shapes: a valid pointer
            // followed by num (>0 && < 512) and max (>= num && <= 1024).
            printf("[LAYT] Scanning InventoryComponent for TArray-shaped fields:\n");
            for (int off = 0x30; off < 0x400; off += 8) {
                uint8_t* p = reinterpret_cast<uint8_t*>(invComp) + off;
                void*   ptr = *reinterpret_cast<void**>(p);
                int32_t num = *reinterpret_cast<int32_t*>(p + 8);
                int32_t mx  = *reinterpret_cast<int32_t*>(p + 12);
                if (!ptr) continue;
                if ((uintptr_t)ptr < 0x10000 || (uintptr_t)ptr > 0x00007FFFFFFFFFFFULL) continue;
                if (num <= 0 || num > 512) continue;
                if (mx  < num || mx  > 4096) continue;
                printf("[LAYT]   +0x%03X TArray  data=%p num=%d max=%d", off, ptr, num, mx);
                // Probe element 0 at a common slot stride (0x240). Read 8 bytes
                // and check if it looks like a valid FInventorySlot (stable
                // memory). Not authoritative but a helpful signal.
                uint64_t elem0 = *reinterpret_cast<uint64_t*>(ptr);
                printf("  elem0_head=0x%016llX\n", (unsigned long long)elem0);
            }

            // Also scan for UObject pointers to known inventory-related
            // classes so we can learn where the real UInventory lives.
            printf("[LAYT] Scanning InventoryComponent for UObject* pointers:\n");
            void* firstInv = nullptr;
            void* secondInv = nullptr;
            for (int off = 0; off < 0x400; off += 8) {
                uint8_t* p = reinterpret_cast<uint8_t*>(invComp) + off;
                void* candidate = *reinterpret_cast<void**>(p);
                if (!candidate) continue;
                if ((uintptr_t)candidate < 0x10000 || (uintptr_t)candidate > 0x00007FFFFFFFFFFFULL) continue;
                std::string cls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(candidate));
                if (cls.empty() || cls == "?") continue;
                std::string lc = cls; for (auto& c : lc) c = (char)tolower((unsigned char)c);
                if (lc.find("invent") == std::string::npos &&
                    lc.find("item")   == std::string::npos &&
                    lc.find("slot")   == std::string::npos &&
                    lc.find("bag")    == std::string::npos) continue;
                printf("[LAYT]   +0x%03X -> %p  class=%s\n", off, candidate, cls.c_str());
                if (cls == "Inventory") {
                    if (!firstInv) firstInv = candidate;
                    else if (!secondInv) secondInv = candidate;
                }
            }

            // =============================================================
            // Dump the actual UInventory objects we just found, same
            // TArray-shape scan. The real TArray<FInventorySlot> lives
            // inside UInventory somewhere between its Slots UPROPERTY
            // offset (0xF0) and a few hundred bytes beyond.
            // =============================================================
            for (int which = 0; which < 2; ++which) {
                void* inv = which == 0 ? firstInv : secondInv;
                if (!inv) continue;
                printf("[LAYT] ---- UInventory #%d @ %p dump ----\n", which + 1, inv);
                HexDump("UInventory", inv, 0x400);
                printf("[LAYT] Scanning UInventory #%d for TArray shapes (relaxed):\n", which + 1);
                // Relaxed filter: accept num>=0 (empty TArrays count), and
                // accept num==max for small fixed-size arrays. Scan the full
                // object range from 0x30 to 0x400.
                for (int off = 0x30; off < 0x400; off += 8) {
                    uint8_t* p = reinterpret_cast<uint8_t*>(inv) + off;
                    void*   ptr = *reinterpret_cast<void**>(p);
                    int32_t num = *reinterpret_cast<int32_t*>(p + 8);
                    int32_t mx  = *reinterpret_cast<int32_t*>(p + 12);
                    if (!ptr) continue;
                    if ((uintptr_t)ptr < 0x10000 || (uintptr_t)ptr > 0x00007FFFFFFFFFFFULL) continue;
                    if (num < 0 || num > 512) continue;
                    if (mx  < num || mx  > 4096) continue;
                    uint64_t elem0 = *reinterpret_cast<uint64_t*>(ptr);
                    uint64_t elem0_mid = *reinterpret_cast<uint64_t*>((uint8_t*)ptr + 0x10);
                    printf("[LAYT]   +0x%03X TArray  data=%p num=%d max=%d  elem0=0x%016llX  elem0+0x10=0x%016llX\n",
                        off, ptr, num, mx,
                        (unsigned long long)elem0,
                        (unsigned long long)elem0_mid);
                }
                printf("[LAYT] --- end UInventory #%d scan ---\n", which + 1);
            }
        }
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

        // GetResourceRecipeValidity / HasSufficientResource are resolved
        // again inside Trainer_InstallCraftValidationHooks below, which is
        // where they are actually hooked — no need to pre-resolve here.

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

        Trainer_InstallCraftValidationHooks(b, sz);
        Trainer_StartArpcFastWatcher();
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
            Trainer_ClampTalentModelAvailablePoints(m_playerTalentCtrl, 99999);
        }
        if (MaxSoloPoints && m_soloTalentCtrl) {
            Trainer_ClampTalentModelAvailablePoints(m_soloTalentCtrl, 99999);
        }

        // Periodic talent-model status log was used during MaxTalentPoints
        // reverse-engineering to find which int32 slot the widget polled.
        // Silenced in release to de-noise the trainer log — re-enable by
        // flipping kLogTalentModelPeriodic if the point-clamping regresses.
        constexpr bool kLogTalentModelPeriodic = false;
        if constexpr (kLogTalentModelPeriodic) {
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

        // No weight: DISABLED the byte-patch on GetTotalWeight —
        // experimentation showed it blows up either the encumbrance UI
        // ("POIDS 1538538.8 / 147 KG" screenshot) or the physics tick
        // (PhysX access-violation on TickPhysScene), depending on whether
        // AOB/reflection resolves the impl or a bystander function. Byte-
        // patching a freestanding float-returning C++ function without a
        // verified signature is too fragile in this binary. We now rely
        // exclusively on per-tick zeroing of every player bag's
        // Inventory.CurrentWeight (done right below, in the next block).
        // The game recomputes display weight from inventory contents via
        // the OnWeightUpdated event, which reads that field — so keeping
        // it clamped to zero is sufficient for NoWeight in practice.

        // Infinite Items / Infinite Durability: every tick, walk every
        // slot in every player inventory and pin the "Durability" dynamic
        // property (EDynamicItemProperties::Durability = type 6, verified
        // against the live UEnum) to a high value.
        //
        // Confirmed mapping (live dump of EDynamicItemProperties):
        //   0 AssociatedItemInventoryId   3 GunCurrentMagSize
        //   1 AssociatedItemInventorySlot 4 CurrentAmmoType
        //   2 DynamicState                5 BuildingVariation
        //   6 Durability                  7 ItemableStack
        //   8 MillijoulesRemaining        9 TransmutableUnits
        //   10 Fillable_StoredUnits       11 Fillable_Type
        //   12 Decayable_CurrentSpoilTime 13 InventoryContainer_LinkedInv
        //
        // Strategy: pin to max(cached, 999999, current). The per-row cache
        // still exists (so we don't nuke a natural max that happens to be
        // *higher* than 999999 for some exotic tool), but the floor is a
        // generous 999999 so an already-damaged tool immediately jumps
        // back to a healthy value instead of locking at whatever value it
        // had when InfiniteItems was first toggled on.
        //
        // Cache key = RowName FName comparison index, read at
        //   slot + Slot_ItemData (0x10) + Item_StaticData (0x18) + 0x08
        // i.e. the 4-byte ComparisonIndex inside the FItemTemplateRowHandle.
        // Zero key → empty slot, skip.
        constexpr int32_t kDurabilityFloor = 999999;
        if (InfiniteItems && m_character && Off::Player_InventoryComp) {
            __try {
                void* invComp = ReadAt<void*>(m_character, Off::Player_InventoryComp);
                if (invComp) {
                    uint8_t* comp = reinterpret_cast<uint8_t*>(invComp);
                    void*   bags = *reinterpret_cast<void**>(comp + Off::InvComp_Inventories);
                    int32_t bagsN = *reinterpret_cast<int32_t*>(comp + Off::InvComp_Inventories + 0x08);
                    if (bags && bagsN > 0 && bagsN <= 32) {
                        uint8_t* entries = reinterpret_cast<uint8_t*>(bags);
                        for (int32_t b = 0; b < bagsN; ++b) {
                            void* bag = *reinterpret_cast<void**>(
                                entries + b * Off::InvEntry_Stride + Off::InvEntry_Ptr);
                            if (!bag) continue;
                            uint8_t* inv = reinterpret_cast<uint8_t*>(bag);
                            void*   slotsData = *reinterpret_cast<void**>(inv + Off::FastArray_Slots);
                            int32_t slotCount = *reinterpret_cast<int32_t*>(inv + Off::FastArray_Slots + 0x08);
                            if (!slotsData || slotCount <= 0 || slotCount > 128) continue;
                            for (int i = 0; i < slotCount; ++i) {
                                uintptr_t slot = reinterpret_cast<uintptr_t>(slotsData) + i * Off::Slot_Size;
                                uintptr_t item = slot + Off::Slot_ItemData;
                                // Identify the item row. Key = FName comparison
                                // index at item+0x18+0x08. Zero key = empty slot.
                                uint32_t rowKey = *reinterpret_cast<uint32_t*>(
                                    item + Off::Item_StaticData + 0x08);
                                if (rowKey == 0) continue;
                                uintptr_t dynTArray = item + Off::Item_DynamicData;
                                void*   dynData  = *reinterpret_cast<void**>(dynTArray);
                                int32_t dynCount = *reinterpret_cast<int32_t*>(dynTArray + 0x08);
                                if (!dynData || dynCount <= 0 || dynCount > 32) continue;
                                for (int j = 0; j < dynCount; ++j) {
                                    uintptr_t d = reinterpret_cast<uintptr_t>(dynData) + j * Off::Dyn_Size;
                                    uint8_t  type = *reinterpret_cast<uint8_t*>(d + Off::Dyn_PropertyType);
                                    int32_t* pVal =  reinterpret_cast<int32_t*>(d + Off::Dyn_Value);
                                    if (type != Off::DynProp_Durability) continue;  // reflection-resolved enum value
                                    // Target = max(kDurabilityFloor, cached, current).
                                    // Cached grows if the game ever hands us a
                                    // value above the floor (exotic legendary
                                    // tools). We never shrink, never write 0.
                                    int32_t target = kDurabilityFloor;
                                    auto it = g_conditionMaxByRow.find(rowKey);
                                    if (it != g_conditionMaxByRow.end() && it->second > target) target = it->second;
                                    if (*pVal > target) target = *pVal;
                                    if (it == g_conditionMaxByRow.end()) {
                                        g_conditionMaxByRow.emplace(rowKey, target);
                                    } else if (target > it->second) {
                                        it->second = target;
                                    }
                                    if (*pVal != target) *pVal = target;
                                }
                            }
                        }
                    }
                }
            }
            __except (1) {}
        }

        // Force CurrentWeight to zero on every player-owned UInventory when
        // NoWeight or InfiniteItems is active. The engine otherwise
        // recalculates weight from ItemDynamicData counts and gates
        // crafting / carrying when the player is overloaded.
        // CurrentWeight is a float at Inv+0xE8 (UE4 reflection).
        if ((NoWeight || InfiniteItems) && m_character && Off::Player_InventoryComp) {
            __try {
                void* invComp = ReadAt<void*>(m_character, Off::Player_InventoryComp);
                if (invComp) {
                    uint8_t* compBytes = reinterpret_cast<uint8_t*>(invComp);
                    void*   data = *reinterpret_cast<void**>(compBytes + Off::InvComp_Inventories);
                    int32_t num  = *reinterpret_cast<int32_t*>(compBytes + Off::InvComp_Inventories + 0x08);
                    if (data && num > 0 && num <= 32) {
                        uint8_t* entries = reinterpret_cast<uint8_t*>(data);
                        for (int32_t i = 0; i < num; ++i) {
                            void* bag = *reinterpret_cast<void**>(
                                entries + i * Off::InvEntry_Stride + Off::InvEntry_Ptr);
                            if (!bag) continue;
                            float* cw = reinterpret_cast<float*>(
                                reinterpret_cast<uint8_t*>(bag) + Off::Inv_CurrentWeight);
                            if (*cw > 0.0f) *cw = 0.0f;
                        }
                    }
                }
                // ALSO zero the character's own cached weight total.
                // Off::Char_CurrentWeight is resolved at boot from
                //   BP_IcarusPlayerCharacterSurvival_C::CurrentWeight
                // (with a fallback to IcarusPlayerCharacterSurvival) so if
                // the UE layout drifts in a future Icarus patch we still
                // find the right field without needing a mod rebuild.
                if (Off::Char_CurrentWeight) {
                    float* charCw = reinterpret_cast<float*>(
                        reinterpret_cast<uint8_t*>(m_character) + Off::Char_CurrentWeight);
                    if (*charCw > 0.0f) *charCw = 0.0f;
                }
                // UI layer refresh — the UMG_EncumbranceBar_C widget caches
                // PlayerWeight (ratio) + renders the KG text from it. When
                // we zero the model fields, the widget still shows the
                // previous numbers until its `NeedsUpdate` flag flips,
                // which normally only happens on inventory change events.
                // Force the flip every tick so the HUD reflects 0 kg
                // within one frame of enabling NoWeight. The widget class
                // lookup walks GObjects once per tick (cheap), the field
                // offset within the widget is reflection-resolved lazily.
                ForceRefreshEncumbranceWidget();
            }
            __except (1) {}
        }

        if (FreeCraft != m_prevFreeCraft) {
            Trainer_ResetFreeCraftTelemetry();
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
            Trainer_PollArpcTrackedProcessor();
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

// Chase a chain of `FF 25 rel32` indirect jumps down to the first byte
// that looks like a real function prologue. UE4.27 + Icarus has several
// such trampolines between the UFunction native-thunk resolver and the
// actual impl body (IAT-style or hot-patch sleds). Returns 0 if we can't
// find a prologue within `maxHops` jumps.
static uintptr_t FollowIndirectJumps(uintptr_t addr, int maxHops = 4) {
    for (int i = 0; i < maxHops; ++i) {
        if (!addr) return 0;
        uint8_t b0 = 0, b1 = 0;
        __try { b0 = *reinterpret_cast<uint8_t*>(addr); b1 = *reinterpret_cast<uint8_t*>(addr + 1); }
        __except (1) { return 0; }
        if (b0 == 0x40 || b0 == 0x48 || b0 == 0x4C || b0 == 0x55 || b0 == 0x56) return addr;
        if (b0 == 0xFF && b1 == 0x25) {
            // jmp qword ptr [rip + rel32]
            int32_t rel = 0;
            __try { rel = *reinterpret_cast<int32_t*>(addr + 2); } __except (1) { return 0; }
            uintptr_t slot = addr + 6 + (intptr_t)rel;
            uintptr_t target = 0;
            __try { target = *reinterpret_cast<uintptr_t*>(slot); } __except (1) { return 0; }
            addr = target;
            continue;
        }
        if (b0 == 0xE9) {
            // jmp rel32 direct
            int32_t rel = 0;
            __try { rel = *reinterpret_cast<int32_t*>(addr + 1); } __except (1) { return 0; }
            addr = addr + 5 + (intptr_t)rel;
            continue;
        }
        return 0;  // unknown opening byte, give up
    }
    return 0;
}

void Trainer::PatchBytes(uintptr_t addr, const uint8_t* patch, uint8_t* backup, int size, bool enable, bool& patched, const char* name) {
    if (!addr) return;
    if (enable && !patched) {
        // Safety: refuse to patch if the first byte doesn't look like a
        // Windows x64 function prologue. Callers are expected to have
        // already chased jmp sleds (via FollowIndirectJumps) so that by
        // this point `addr` should point at a real prologue. If not, we
        // log once and set `patched=true` to prevent per-frame spam.
        //
        // Accepted first bytes: 40/48/4C REX prefixes, 55 push-rbp,
        // 56 push-rsi — any other start is refused.
        uint8_t firstByte = *reinterpret_cast<uint8_t*>(addr);
        if (firstByte != 0x40 && firstByte != 0x48 && firstByte != 0x4C &&
            firstByte != 0x55 && firstByte != 0x56) {
            LOG_PATCH("%s: disabling — first byte 0x%02X @0x%llX not a prologue",
                name, firstByte, (unsigned long long)addr);
            patched = true;
            return;
        }
        memcpy(backup, reinterpret_cast<void*>(addr), size);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(addr), patch, size);
        VirtualProtect(reinterpret_cast<void*>(addr), size, oldP, &oldP);
        patched = true;
        LOG_PATCH("%s patched @0x%llX (size=%d)", name, (unsigned long long)addr, size);
    } else if (!enable && patched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(addr), backup, size);
        VirtualProtect(reinterpret_cast<void*>(addr), size, oldP, &oldP);
        patched = false;
        LOG_PATCH("%s restored", name);
    }
}

// Flip the UMG_EncumbranceBar_C widget's `NeedsUpdate` byte so the HUD
// re-reads PlayerWeight / CurrentEncumbrance on its next tick. Used when
// NoWeight / InfiniteItems just zeroed the underlying model fields —
// otherwise the widget keeps its previously-cached display until the
// player picks up or drops something. Also writes 0 to PlayerWeight /
// CurrentEncumbrance directly so the very next frame is clean.
//
// Offsets are discovered lazily the first time we find a live widget
// instance (via FindPropertyOffsetInClass on the UMG class pointer), so
// future Icarus patches that move PlayerWeight or NeedsUpdate work
// without a rebuild.
// Forward decl of the SEH-safe raw-write helper (definition below).
static bool ApplyWidgetZeroAndRefresh(void* widget, uintptr_t offPW, uintptr_t offCE, uintptr_t offNU);


void Trainer::ForceRefreshEncumbranceWidget() {
    static uintptr_t  s_offPlayerW   = 0;     // PlayerWeight (float)
    static uintptr_t  s_offCurEnc    = 0;     // CurrentEncumbrance (float)
    static uintptr_t  s_offNeedsUpd  = 0;     // NeedsUpdate (u8)
    static bool       s_offResolved  = false;

    // Resolve widget field offsets once via reflection.
    if (!s_offResolved) {
        s_offResolved = true;
        uintptr_t cls = UObjectLookup::FindClassByName("UMG_EncumbranceBar_C");
        if (cls) {
            int32_t pw = UObjectLookup::FindPropertyOffsetInClass(cls, "PlayerWeight");
            int32_t ce = UObjectLookup::FindPropertyOffsetInClass(cls, "CurrentEncumbrance");
            int32_t nu = UObjectLookup::FindPropertyOffsetInClass(cls, "NeedsUpdate");
            if (pw >= 0) s_offPlayerW  = static_cast<uintptr_t>(pw);
            if (ce >= 0) s_offCurEnc   = static_cast<uintptr_t>(ce);
            if (nu >= 0) s_offNeedsUpd = static_cast<uintptr_t>(nu);
        }
    }
    if (!s_offPlayerW) return;  // reflection failed — nothing safe to poke

    // Icarus spawns SEVERAL encumbrance widget instances simultaneously
    // (main HUD + inventory-panel variant, possibly more on mount / rig).
    // Caching only the first one left the others stuck at overencumbered.
    //
    // We cache up to 6 widget pointers and refresh them every tick.
    // Every ~60 ticks (~2s at 33Hz) we re-scan GObjects to catch newly
    // spawned widgets (e.g. after opening the inventory screen). This
    // keeps per-tick work at ~6 memory writes instead of a 200k-entry
    // GObjects walk.
    static void*    s_widgets[8] = {};
    static int      s_widgetN    = 0;
    static int      s_scanTick   = 0;
    if (++s_scanTick >= 60 || s_widgetN == 0) {
        s_scanTick = 0;
        s_widgetN  = 0;
        int32_t total = UObjectLookup::GetObjectCount();
        for (int32_t i = 0; i < total && s_widgetN < 8; ++i) {
            uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
            if (!obj) continue;
            std::string cls = UObjectLookup::GetObjectClassName(obj);
            if (cls != "UMG_EncumbranceBar_C") continue;
            std::string name = UObjectLookup::GetObjectName(obj);
            if (name.rfind("Default__", 0) == 0) continue;
            s_widgets[s_widgetN++] = reinterpret_cast<void*>(obj);
        }
    }
    for (int k = 0; k < s_widgetN; ++k) {
        if (!ApplyWidgetZeroAndRefresh(s_widgets[k], s_offPlayerW, s_offCurEnc, s_offNeedsUpd)) {
            s_widgets[k] = nullptr;      // invalidated — will be repopulated next rescan
        }
    }
}

// SEH-only helper (no C++ objects in this frame — MSVC C2712). Writes
// zeros to PlayerWeight / CurrentEncumbrance and sets NeedsUpdate=1.
// Returns false if any access faulted (caller will re-probe the widget).
static bool ApplyWidgetZeroAndRefresh(void* widget, uintptr_t offPW, uintptr_t offCE, uintptr_t offNU) {
    if (!widget) return false;
    __try {
        uint8_t* base = reinterpret_cast<uint8_t*>(widget);
        if (offPW) {
            float* p = reinterpret_cast<float*>(base + offPW);
            if (*p > 0.0f) *p = 0.0f;
        }
        if (offCE) {
            float* p = reinterpret_cast<float*>(base + offCE);
            if (*p > 0.0f) *p = 0.0f;
        }
        if (offNU) {
            *reinterpret_cast<uint8_t*>(base + offNU) = 1;
        }
        return true;
    }
    __except (1) { return false; }
}

// ============================================================================
// NoWeight — Source-of-truth hook on IcarusFunctionLibrary::AddModifierState.
//
// Overburdened (the modifier that cuts MaxWalkSpeed, locks sprint, triggers
// the "sluggish" anim BP) is applied through a single static library call
// every time the weight system re-evaluates. Signature (propswalk confirmed):
//
//   bool UIcarusFunctionLibrary::AddModifierState(
//       UObject*                 Parent,
//       FModifierStateRowHandle  InModifier,    // 24 bytes, [+0x08] = RowName FName
//       UObject*                 Causer,
//       UObject*                 Instigator,
//       float                    Effectiveness);
//
// MSVC x64 passes the struct by implicit pointer (RDX). We read RowName at
// +0x08 inside that buffer, and when NoWeight is on AND the row is
// "Overburdened" we short-circuit to `return false` — the modifier is never
// created, no attached component, no component to remove later, no anim-BP
// "heavy" state. Clean at the source.
//
// Hook is installed once via MinHook after UObjectLookup is ready; toggling
// NoWeight on/off flips behaviour at runtime without uninstalling.
// ============================================================================
using FnAddModifierState = bool(__fastcall*)(void* parent, void* inModifier,
                                             void* causer, void* instigator,
                                             float effectiveness);
static FnAddModifierState g_origAddModifierState = nullptr;

// SEH-only helper: read the FName ComparisonIndex out of the InModifier
// struct. Cannot live inside HookAddModifierState because __try is
// incompatible with C++ unwinding from std::string (MSVC C2712).
static int32_t SafeReadModifierRowIdx(void* inModifier) {
    if (!inModifier) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(
            reinterpret_cast<uint8_t*>(inModifier) + 0x08);
    }
    __except (1) { return 0; }
}

bool __fastcall HookAddModifierState(void* parent, void* inModifier,
                                     void* causer, void* instigator,
                                     float effectiveness) {
    if (Trainer::Get().NoWeight) {
        int32_t rowIdx = SafeReadModifierRowIdx(inModifier);
        if (rowIdx) {
            std::string row = UObjectLookup::ResolveFNameByIndex(rowIdx);
            if (row == "Overburdened") {
                // Silently swallow the call. Game proceeds as if the
                // modifier was never applied — no attached component,
                // no anim-BP "heavy" state, no speed multiplier.
                return false;
            }
        }
    }
    return g_origAddModifierState(parent, inModifier, causer, instigator, effectiveness);
}

void Trainer::InstallWeightHook() {
    if (g_origAddModifierState) return;  // already installed

    uintptr_t impl = UObjectLookup::FindNativeFunction(
        "IcarusFunctionLibrary", "AddModifierState");
    if (!impl) {
        LOG_WARN("NoWeight hook: AddModifierState native impl not resolved");
        Log::Resume::Fail("hooks", "AddModifierState (native impl)");
        return;
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_HOOK("MH_Initialize failed for NoWeight hook: %d", (int)init);
        Log::Resume::Fail("hooks", "MH_Initialize");
        return;
    }
    MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(impl),
                                reinterpret_cast<void*>(&HookAddModifierState),
                                reinterpret_cast<void**>(&g_origAddModifierState));
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        LOG_HOOK("AddModifierState MH_CreateHook failed: %d", (int)s);
        Log::Resume::Fail("hooks", "AddModifierState create");
        return;
    }
    s = MH_EnableHook(reinterpret_cast<void*>(impl));
    if (s != MH_OK && s != MH_ERROR_ENABLED) {
        LOG_HOOK("AddModifierState MH_EnableHook failed: %d", (int)s);
        Log::Resume::Fail("hooks", "AddModifierState enable");
        return;
    }
    LOG_HOOK("NoWeight source-hook installed on AddModifierState @ 0x%p",
        reinterpret_cast<void*>(impl));
    Log::Resume::Ok("hooks");
}

void Trainer::PatchWeight(bool enable) {
    // GetTotalWeight / GetCurrentInventoryWeight return a float — the ABI
    // hands it back in XMM0, not EAX. The old `xor eax,eax; ret` (31 C0 C3)
    // left XMM0 with whatever garbage the prologue had, so the UI still saw
    // non-zero weight for a few ticks. We patch with
    //   xorps xmm0, xmm0 ; ret   → 0F 57 C0 C3
    // which zeroes the entire 128-bit XMM0 register. Guaranteed float = 0.0f.
    uint8_t retZeroF[4] = { 0x0F, 0x57, 0xC0, 0xC3 };

    if (!m_weightAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        m_weightAddr = ResolveNativeOrAob("InventoryComponent", "GetTotalWeight",
                                          b, sz, kGetTotalWeightAob, "GetTotalWeight");
        if (!m_weightAddr) {
            printf("[WEIGHT] GetTotalWeight runtime resolution failed\n");
        }
    }
    PatchBytes(m_weightAddr, retZeroF, m_weightBackup, 4, enable, m_weightPatched, "GetTotalWeight");

    // NOTE: an earlier iteration also tried to byte-patch
    // IcarusPlayerCharacterSurvival::GetCurrentInventoryWeight. That was
    // removed after a PhysX access-violation crash — the reflection
    // resolver pointed at an `FF 25 rel32` IAT-style trampoline whose
    // target, even after sled-walking, landed on a prologue that
    // happened to look valid but wasn't the right function. Patching
    // that bystander corrupted unrelated game state (physics tick
    // dereferencing poisoned return values).
    //
    // The UI-side "Encumbrance" widget reads its weight via the UFunction
    // thunk on InventoryComponent::GetTotalWeight anyway (verified via
    // `outer 0x... OnRep_CachedWeightValue` and the delegate chain), so
    // the single-point patch above is sufficient. Combined with the
    // per-bag CurrentWeight zero-out that runs every tick in Trainer::Tick
    // while NoWeight / InfiniteItems is on, the user never reads a
    // non-zero weight anywhere.
}

// ============================================================================
// Named Pipe Server — receives commands from Electron app
// ============================================================================

void Trainer::StartPipeServer() {
    CreateThread(nullptr, 0, PipeServerThread, this, 0, nullptr);
}

// Safe memory helpers for the pipe debug commands. Wrapped in SEH so a
// bad pointer from the client doesn't take the whole DLL down.
static bool TryReadBytes(uintptr_t addr, void* dst, size_t n) {
    __try { memcpy(dst, reinterpret_cast<void*>(addr), n); return true; }
    __except (1) { return false; }
}
// Writer counterpart. Pages are auto-RW'd and restored — works for both
// heap objects and code sections. The Python client passes hex strings
// which we decode before calling this, so it's purely byte-level.
static bool TryWriteBytes(uintptr_t addr, const void* src, size_t n) {
    DWORD oldP = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &oldP))
        return false;
    bool ok = false;
    __try { memcpy(reinterpret_cast<void*>(addr), src, n); ok = true; }
    __except (1) { ok = false; }
    VirtualProtect(reinterpret_cast<void*>(addr), n, oldP, &oldP);
    return ok;
}
// Parse a compact hex string (no 0x, no spaces) into a byte buffer.
// "DEADBEEF" -> {0xDE, 0xAD, 0xBE, 0xEF}. Returns bytes written or -1.
static int HexStringToBytes(const char* hex, uint8_t* out, int cap) {
    int len = 0; while (hex[len]) ++len;
    if (len & 1) return -1;
    int nbytes = len / 2;
    if (nbytes > cap) return -1;
    for (int i = 0; i < nbytes; ++i) {
        auto nib = [](char c)->int{
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        int hi = nib(hex[i*2]), lo = nib(hex[i*2+1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return nbytes;
}
// Inner raw helpers kept free of std::string objects so MSVC accepts __try
// without C2712 (SEH cannot coexist with C++ object unwinding).
static int HandleDbgRaw(const char* cmd, const char* args, char* out, size_t outCap);

// Entry point for all "dbg:*" pipe commands. Keeps the std::string / vector
// usage in this function and forwards raw char buffers into HandleDbgRaw.
static int HandleDbgCommand(const char* cmd, const char* args, char* out, size_t outCap) {
    // Dispatch to commands that can safely use C++ objects. These fetch
    // a string result (class name, etc.) and copy it into `out`.
    if (!strcmp(cmd, "classof")) {
        uintptr_t a = strtoull(args, nullptr, 16);
        std::string s = UObjectLookup::GetObjectClassName(a);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK %s", s.c_str());
    }
    if (!strcmp(cmd, "nameof")) {
        uintptr_t a = strtoull(args, nullptr, 16);
        std::string s = UObjectLookup::GetObjectName(a);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK %s", s.c_str());
    }
    if (!strcmp(cmd, "findcls")) {
        uintptr_t a = UObjectLookup::FindClassByName(args);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%llX", (unsigned long long)a);
    }
    if (!strcmp(cmd, "findstruct")) {
        uintptr_t a = UObjectLookup::FindStructByName(args);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%llX", (unsigned long long)a);
    }
    if (!strcmp(cmd, "getbyindex")) {
        // getbyindex:<idx>  — return GObjects[idx] pointer + class + name.
        // Idx can be hex (0x…) or decimal. The serial number isn't validated
        // here; use this with an FWeakObjectPtr's idx to resolve to a live
        // UObject when you know the target is current.
        int32_t idx = (int32_t)strtoll(args, nullptr, 16);
        uintptr_t obj = UObjectLookup::GetObjectByIndex(idx);
        if (!obj) return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x0 (index %d empty)", idx);
        std::string cls  = UObjectLookup::GetObjectClassName(obj);
        std::string name = UObjectLookup::GetObjectName(obj);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%llX %s / %s",
            (unsigned long long)obj, cls.c_str(), name.c_str());
    }
    if (!strcmp(cmd, "findobj")) {
        // Iterate GObjects and return the first live instance (non-CDO) of args.
        int32_t total = UObjectLookup::GetObjectCount();
        for (int32_t i = 0; i < total; ++i) {
            uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
            if (!obj) continue;
            std::string cls = UObjectLookup::GetObjectClassName(obj);
            if (cls != args) continue;
            std::string name = UObjectLookup::GetObjectName(obj);
            if (name.rfind("Default__", 0) == 0) continue;
            return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%llX %s", (unsigned long long)obj, name.c_str());
        }
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x0");
    }
    if (!strcmp(cmd, "listobj")) {
        // listobj:<className>:<maxN> — but args is already past the first ':'
        // so we parse another colon here.
        const char* colon = strchr(args, ':');
        std::string className = colon ? std::string(args, colon - args) : std::string(args);
        int maxN = colon ? atoi(colon + 1) : 20;
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK");
        int total = UObjectLookup::GetObjectCount();
        int found = 0;
        for (int32_t i = 0; i < total && found < maxN; ++i) {
            uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
            if (!obj) continue;
            std::string cls = UObjectLookup::GetObjectClassName(obj);
            if (cls != className) continue;
            std::string name = UObjectLookup::GetObjectName(obj);
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n0x%llX %s", (unsigned long long)obj, name.c_str());
            if (n <= 0) break;
            written += n;
            ++found;
        }
        return written;
    }
    if (!strcmp(cmd, "propoff")) {
        // propoff:<className>:<propName> — look up a single property's offset
        // (walks parent classes up to 16 hops). Returns "OK 0x<offset>".
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage propoff:<class>:<prop>");
        std::string className(args, colon - args);
        std::string propName(colon + 1);
        int32_t off = UObjectLookup::FindPropertyOffset(className.c_str(), propName.c_str());
        if (off < 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR %s::%s not found", className.c_str(), propName.c_str());
        return _snprintf_s(out, outCap, _TRUNCATE, "OK %s::%s = 0x%X", className.c_str(), propName.c_str(), (unsigned)off);
    }
    if (!strcmp(cmd, "props") || !strcmp(cmd, "propsall")) {
        // props:<className>         — walk only the class's own ChildProperties
        // propsall:<className>      — walk up through Super classes too
        // Works for both UClass and UScriptStruct (they share UStruct layout).
        // Returns: each property on its own line "  +0x<off> <name>"
        bool walkSupers = !strcmp(cmd, "propsall");
        std::string className(args);
        uintptr_t cls = UObjectLookup::FindClassByName(className.c_str());
        if (!cls) cls = UObjectLookup::FindStructByName(className.c_str());
        if (!cls) return _snprintf_s(out, outCap, _TRUNCATE, "ERR class/struct '%s' not found", className.c_str());
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK props %s (cls=0x%llX)",
            className.c_str(), (unsigned long long)cls);
        // Constants mirrored from UObjectLookup.cpp
        constexpr int OFF_USTRUCT_SUPER = 0x40;
        constexpr int OFF_USTRUCT_CHILDPROPS = 0x50;
        constexpr int OFF_FFIELD_NEXT = 0x20;
        constexpr int OFF_FFIELD_NAME = 0x28;
        constexpr int OFF_FPROPERTY_OFFSET_INT = 0x4C;
        // FProperty layout beyond Offset_Internal has not been validated for
        // this build — only the offset is trusted. Dim/size display is elided.
        uintptr_t curCls = cls;
        int hops = 0;
        while (curCls && hops < 16) {
            std::string clsName = UObjectLookup::GetObjectName(curCls);
            if (hops > 0) {
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n-- super[%d] %s --", hops, clsName.c_str());
                if (n > 0) written += n;
            }
            uintptr_t field = 0;
            uint64_t childHead = 0;
            if (TryReadBytes(curCls + OFF_USTRUCT_CHILDPROPS, &childHead, 8)) field = (uintptr_t)childHead;
            int safety = 4096;
            while (field && safety-- > 0) {
                std::string pname = UObjectLookup::ReadFNameAt(field + OFF_FFIELD_NAME);
                int32_t poff = -1;
                TryReadBytes(field + OFF_FPROPERTY_OFFSET_INT, &poff, 4);
                // Also read the FFieldClass pointer so we can show the property type
                // (e.g. BoolProperty, FloatProperty, ArrayProperty, StructProperty).
                uint64_t fieldCls = 0;
                std::string typeName;
                if (TryReadBytes(field + 0x00, &fieldCls, 8) && fieldCls) {
                    // FFieldClass has FName at +0x00 (first field)
                    typeName = UObjectLookup::ReadFNameAt((uintptr_t)fieldCls);
                }
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  +0x%03X  %-40s  %s",
                    (unsigned)poff, pname.c_str(), typeName.c_str());
                if (n <= 0) break;
                written += n;
                uint64_t next = 0;
                if (!TryReadBytes(field + OFF_FFIELD_NEXT, &next, 8)) break;
                field = (uintptr_t)next;
            }
            if (!walkSupers) break;
            uint64_t super = 0;
            if (!TryReadBytes(curCls + OFF_USTRUCT_SUPER, &super, 8) || !super) break;
            curCls = (uintptr_t)super;
            hops++;
        }
        return written;
    }
    // ──── FName helpers ───────────────────────────────────────────────
    if (!strcmp(cmd, "fname")) {
        // fname:<comparisonIndex>    (hex or decimal, e.g. "1DD5F" or "0x1DD5F")
        int32_t idx = (int32_t)strtoll(args, nullptr, 16);
        std::string s = UObjectLookup::ResolveFNameByIndex(idx);
        if (s.empty()) return _snprintf_s(out, outCap, _TRUNCATE, "ERR fname idx 0x%X not found", idx);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK fname[0x%X] = %s", idx, s.c_str());
    }
    if (!strcmp(cmd, "fnameof")) {
        // fnameof:<addr>  — resolve the FName whose bytes start AT addr
        // (unlike `nameof` which reads at addr+0x18 for UObjects).
        uintptr_t a = strtoull(args, nullptr, 16);
        std::string s = UObjectLookup::ReadFNameAt(a);
        if (s.empty()) return _snprintf_s(out, outCap, _TRUNCATE, "ERR fnameof @0x%llX failed", (unsigned long long)a);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK fname@0x%llX = %s", (unsigned long long)a, s.c_str());
    }
    // ──── UObject inheritance / outer ─────────────────────────────────
    if (!strcmp(cmd, "outer")) {
        // outer:<addr>  — dump full outer chain (up to 8 levels)
        uintptr_t a = strtoull(args, nullptr, 16);
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK outer chain 0x%llX", (unsigned long long)a);
        uintptr_t cur = a;
        for (int lvl = 0; lvl < 8; ++lvl) {
            std::string cls  = UObjectLookup::GetObjectClassName(cur);
            std::string name = UObjectLookup::GetObjectName(cur);
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  [%d] 0x%llX  %s / %s", lvl, (unsigned long long)cur,
                cls.c_str(), name.c_str());
            if (n > 0) written += n;
            uintptr_t nxt = 0;
            if (!TryReadBytes(cur + 0x20, &nxt, 8) || !nxt || nxt == cur) break;
            cur = nxt;
        }
        return written;
    }
    if (!strcmp(cmd, "isa")) {
        // isa:<addr>:<className>  — walk ClassPrivate -> Super chain
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage isa:<addr>:<className>");
        uintptr_t a = strtoull(args, nullptr, 16);
        std::string target(colon + 1);
        uint64_t cls = 0;
        if (!TryReadBytes(a + 0x10, &cls, 8) || !cls)
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR no class on @0x%llX", (unsigned long long)a);
        for (int hops = 0; hops < 16; ++hops) {
            std::string name = UObjectLookup::GetObjectName((uintptr_t)cls);
            if (name == target)
                return _snprintf_s(out, outCap, _TRUNCATE, "OK isa yes (matched at depth %d)", hops);
            uint64_t super = 0;
            if (!TryReadBytes((uintptr_t)cls + 0x40, &super, 8) || !super) break;
            cls = super;
        }
        return _snprintf_s(out, outCap, _TRUNCATE, "OK isa no");
    }
    // ──── Function lookup / invocation ────────────────────────────────
    if (!strcmp(cmd, "funcoff") || !strcmp(cmd, "funcof")) {
        // funcoff:<className>:<funcName>  — returns UFunction address
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage funcoff:<class>:<func>");
        std::string className(args, colon - args);
        std::string funcName(colon + 1);
        uintptr_t cls = UObjectLookup::FindClassByName(className.c_str());
        if (!cls) return _snprintf_s(out, outCap, _TRUNCATE, "ERR class '%s' not found", className.c_str());
        uintptr_t fn = UObjectLookup::FindFunctionInClass(cls, funcName.c_str());
        if (!fn) return _snprintf_s(out, outCap, _TRUNCATE, "ERR %s::%s not found", className.c_str(), funcName.c_str());
        return _snprintf_s(out, outCap, _TRUNCATE, "OK %s::%s = 0x%llX",
            className.c_str(), funcName.c_str(), (unsigned long long)fn);
    }
    if (!strcmp(cmd, "callfn")) {
        // callfn:<selfObj>:<className>:<funcName>[:<hexparams>]
        // params buffer is written verbatim; its layout must match the
        // UFunction's expected params struct (inspect with `props` of the
        // UFunction's UStruct). Up to 256 bytes.
        const char* c1 = strchr(args, ':');
        if (!c1) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage callfn:<obj>:<class>:<func>[:<hex>]");
        const char* c2 = strchr(c1 + 1, ':');
        if (!c2) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage callfn:<obj>:<class>:<func>[:<hex>]");
        const char* c3 = strchr(c2 + 1, ':');  // optional
        uintptr_t selfAddr = strtoull(args, nullptr, 16);
        std::string className(c1 + 1, c2 - (c1 + 1));
        std::string funcName(c3 ? std::string(c2 + 1, c3 - (c2 + 1)) : std::string(c2 + 1));
        uintptr_t cls = UObjectLookup::FindClassByName(className.c_str());
        if (!cls) return _snprintf_s(out, outCap, _TRUNCATE, "ERR class '%s' not found", className.c_str());
        uintptr_t fn = UObjectLookup::FindFunctionInClass(cls, funcName.c_str());
        if (!fn) return _snprintf_s(out, outCap, _TRUNCATE, "ERR %s::%s not found", className.c_str(), funcName.c_str());
        // 0x400 (1024) covers UFunctions with ParmsSize up to the largest in
        // Icarus binary (OnServer_AddItemCheat = 0x1F8). Stack storage,
        // zero-initialised, handed to ProcessEvent.
        uint8_t params[0x400]{};
        int psize = 0;
        if (c3 && c3[1]) {
            psize = HexStringToBytes(c3 + 1, params, sizeof(params));
            if (psize < 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad hex params");
        }
        bool ok = UObjectLookup::CallUFunction(
            reinterpret_cast<void*>(selfAddr), fn, params);
        // Dump the first 16 bytes of the params (covers the return value
        // for small scalar returns) so the client can decode it.
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK call=%d paramsIn=%d paramsOut=", (int)ok, psize);
        for (int i = 0; i < 16; ++i) {
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE, "%02X", params[i]);
            if (n > 0) written += n;
        }
        return written;
    }
    if (!strcmp(cmd, "listfuncs")) {
        // listfuncs:<className>  — enumerate UFunction Children
        std::string className(args);
        uintptr_t cls = UObjectLookup::FindClassByName(className.c_str());
        if (!cls) cls = UObjectLookup::FindStructByName(className.c_str());
        if (!cls) return _snprintf_s(out, outCap, _TRUNCATE, "ERR class/struct '%s' not found", className.c_str());
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK funcs %s", className.c_str());
        uint64_t child = 0;
        TryReadBytes(cls + 0x48, &child, 8);
        int safety = 2048;
        while (child && safety-- > 0) {
            std::string name = UObjectLookup::GetObjectName((uintptr_t)child);
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  0x%llX  %s", (unsigned long long)child, name.c_str());
            if (n > 0) written += n;
            uint64_t next = 0;
            if (!TryReadBytes((uintptr_t)child + 0x20, &next, 8)) break;
            child = next;
        }
        return written;
    }
    // ──── Typed property read / write (via reflection) ────────────────
    if (!strcmp(cmd, "propget")) {
        // propget:<objAddr>:<className>:<fieldName>[:<byteCountHex>]
        // Resolves offset via FindPropertyOffset then dumps N bytes (default 8).
        const char* c1 = strchr(args, ':');
        if (!c1) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage propget:<obj>:<class>:<field>[:<bytes>]");
        const char* c2 = strchr(c1 + 1, ':');
        if (!c2) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage propget:<obj>:<class>:<field>[:<bytes>]");
        const char* c3 = strchr(c2 + 1, ':');
        uintptr_t obj = strtoull(args, nullptr, 16);
        std::string className(c1 + 1, c2 - (c1 + 1));
        std::string field(c3 ? std::string(c2 + 1, c3 - (c2 + 1)) : std::string(c2 + 1));
        int nBytes = c3 ? (int)strtoull(c3 + 1, nullptr, 16) : 8;
        if (nBytes < 1 || nBytes > 64) nBytes = 8;
        int32_t off = UObjectLookup::FindPropertyOffset(className.c_str(), field.c_str());
        if (off < 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR %s::%s not found", className.c_str(), field.c_str());
        uint8_t buf[64]{};
        if (!TryReadBytes(obj + off, buf, nBytes))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR read failed @0x%llX+0x%X", (unsigned long long)obj, off);
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK %s::%s @0x%llX+0x%X = ",
            className.c_str(), field.c_str(), (unsigned long long)obj, off);
        for (int i = 0; i < nBytes; ++i) {
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE, "%02X", buf[i]);
            if (n > 0) written += n;
        }
        // Best-effort typed interpretation (hint only).
        if (nBytes == 4) {
            int32_t iv; memcpy(&iv, buf, 4);
            float fv;   memcpy(&fv, buf, 4);
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                " (i32=%d, f32=%g)", iv, fv);
            if (n > 0) written += n;
        } else if (nBytes == 8) {
            uint64_t uv; memcpy(&uv, buf, 8);
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                " (u64=0x%llX)", (unsigned long long)uv);
            if (n > 0) written += n;
        }
        return written;
    }
    if (!strcmp(cmd, "propset")) {
        // propset:<objAddr>:<className>:<fieldName>:<hexValue>
        const char* c1 = strchr(args, ':');
        const char* c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
        const char* c3 = c2 ? strchr(c2 + 1, ':') : nullptr;
        if (!c3) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage propset:<obj>:<class>:<field>:<hex>");
        uintptr_t obj = strtoull(args, nullptr, 16);
        std::string className(c1 + 1, c2 - (c1 + 1));
        std::string field(c2 + 1, c3 - (c2 + 1));
        int32_t off = UObjectLookup::FindPropertyOffset(className.c_str(), field.c_str());
        if (off < 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR %s::%s not found", className.c_str(), field.c_str());
        uint8_t buf[64]{};
        int nb = HexStringToBytes(c3 + 1, buf, sizeof(buf));
        if (nb <= 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad hex value");
        if (!TryWriteBytes(obj + off, buf, nb))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR write failed");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK %s::%s @0x%llX+0x%X wrote %d bytes",
            className.c_str(), field.c_str(), (unsigned long long)obj, off, nb);
    }
    // ──── Player bag introspection ────────────────────────────────────
    if (!strcmp(cmd, "inv")) {
        // inv  — list the player's Quickbar/Backpack/Equipment/Suit/Upgrade
        void* c = Trainer::Get().GetCharacter();
        if (!c || !Off::Player_InventoryComp)
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR no character");
        void* comp = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(c) + Off::Player_InventoryComp);
        if (!comp) return _snprintf_s(out, outCap, _TRUNCATE, "ERR no InvComp");
        uint8_t* compBytes = reinterpret_cast<uint8_t*>(comp);
        void* data = *reinterpret_cast<void**>(compBytes + Off::InvComp_Inventories);
        int32_t num = *reinterpret_cast<int32_t*>(compBytes + Off::InvComp_Inventories + 0x08);
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK inv character=0x%llX comp=0x%llX count=%d",
            (unsigned long long)c, (unsigned long long)comp, num);
        if (!data || num <= 0 || num > 32) return written;
        uint8_t* entries = reinterpret_cast<uint8_t*>(data);
        for (int32_t i = 0; i < num; ++i) {
            uint8_t* e = entries + i * Off::InvEntry_Stride;
            std::string name = UObjectLookup::ReadFNameAt(
                reinterpret_cast<uintptr_t>(e + Off::InvEntry_FName));
            void* bag = *reinterpret_cast<void**>(e + Off::InvEntry_Ptr);
            int32_t slotNum = 0, slotMax = 0;
            if (bag) {
                uint8_t* b = reinterpret_cast<uint8_t*>(bag);
                slotNum = *reinterpret_cast<int32_t*>(b + Off::FastArray_Slots + 0x08);
                slotMax = *reinterpret_cast<int32_t*>(b + Off::FastArray_Slots + 0x0C);
            }
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  [%d] %-12s  bag=0x%llX  %d/%d",
                i, name.c_str(), (unsigned long long)bag, slotNum, slotMax);
            if (n > 0) written += n;
        }
        return written;
    }
    if (!strcmp(cmd, "listitems")) {
        // listitems:<bagAddr>  — enumerate all occupied slots: RowName, stack count
        uintptr_t bag = strtoull(args, nullptr, 16);
        if (!bag) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage listitems:<bagAddr>");
        uint8_t* b = reinterpret_cast<uint8_t*>(bag);
        void*   data = *reinterpret_cast<void**>(b + Off::FastArray_Slots);
        int32_t snum = *reinterpret_cast<int32_t*>(b + Off::FastArray_Slots + 0x08);
        int32_t smax = *reinterpret_cast<int32_t*>(b + Off::FastArray_Slots + 0x0C);
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK listitems bag=0x%llX %d/%d",
            (unsigned long long)bag, snum, smax);
        if (!data || snum <= 0) return written;
        uint8_t* slots = reinterpret_cast<uint8_t*>(data);
        for (int32_t i = 0; i < snum; ++i) {
            uint8_t* slot = slots + i * Off::Slot_Size;
            uint8_t* item = slot + Off::Slot_ItemData;
            void* dt = *reinterpret_cast<void**>(item + Off::Item_StaticData);
            if (!dt) { /* empty slot */
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  [%02d] (empty)", i);
                if (n > 0) written += n;
                continue;
            }
            std::string row = UObjectLookup::ReadFNameAt(
                reinterpret_cast<uintptr_t>(item + Off::Item_StaticData + 0x08));
            // Stack count = sum of ItemableStack dynamic property values
            int32_t stack = 0;
            void* dynData = *reinterpret_cast<void**>(item + Off::Item_DynamicData);
            int32_t dynNum = *reinterpret_cast<int32_t*>(item + Off::Item_DynamicData + 0x08);
            if (dynData && dynNum > 0 && dynNum < 32) {
                for (int32_t j = 0; j < dynNum; ++j) {
                    uint8_t* d = reinterpret_cast<uint8_t*>(dynData) + j * Off::Dyn_Size;
                    if (d[Off::Dyn_PropertyType] == Off::DynProp_ItemableStack) {
                        stack += *reinterpret_cast<int32_t*>(d + Off::Dyn_Value);
                        break;
                    }
                }
            }
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  [%02d] %-32s x%d", i, row.c_str(), stack);
            if (n > 0) written += n;
        }
        return written;
    }
    if (!strcmp(cmd, "dumpslot")) {
        // dumpslot:<bagAddr>:<slotIndex>  — full FItemData dump for a slot
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage dumpslot:<bag>:<idx>");
        uintptr_t bag = strtoull(args, nullptr, 16);
        int idx = (int)strtoll(colon + 1, nullptr, 10);
        uint8_t* b = reinterpret_cast<uint8_t*>(bag);
        void* data = *reinterpret_cast<void**>(b + Off::FastArray_Slots);
        int32_t snum = *reinterpret_cast<int32_t*>(b + Off::FastArray_Slots + 0x08);
        if (!data || idx < 0 || idx >= snum)
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR slot %d out of range (%d)", idx, snum);
        uint8_t* slot = reinterpret_cast<uint8_t*>(data) + idx * Off::Slot_Size;
        uint8_t* item = slot + Off::Slot_ItemData;
        std::string row = UObjectLookup::ReadFNameAt(
            reinterpret_cast<uintptr_t>(item + Off::Item_StaticData + 0x08));
        std::string dtn = UObjectLookup::ReadFNameAt(
            reinterpret_cast<uintptr_t>(item + Off::Item_StaticData + 0x10));
        int32_t slotIndex = *reinterpret_cast<int32_t*>(slot + 0x238);
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK slot[%d] @0x%llX\n  Index=%d  Row=%s  DT=%s",
            idx, (unsigned long long)slot, slotIndex, row.c_str(), dtn.c_str());
        // Dynamic properties
        void* dynData = *reinterpret_cast<void**>(item + Off::Item_DynamicData);
        int32_t dynNum = *reinterpret_cast<int32_t*>(item + Off::Item_DynamicData + 0x08);
        if (dynData && dynNum > 0 && dynNum < 32) {
            for (int32_t j = 0; j < dynNum; ++j) {
                uint8_t* d = reinterpret_cast<uint8_t*>(dynData) + j * Off::Dyn_Size;
                uint8_t type = d[Off::Dyn_PropertyType];
                int32_t val  = *reinterpret_cast<int32_t*>(d + Off::Dyn_Value);
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  dyn[%d] type=%u value=%d", j, (unsigned)type, val);
                if (n > 0) written += n;
            }
        }
        return written;
    }
    // ──── Bulk UObject discovery ──────────────────────────────────────
    if (!strcmp(cmd, "findname") || !strcmp(cmd, "findnamed")) {
        // findname:<className>:<substring>  — first live instance whose
        // Name contains substring, case-insensitive.
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage findname:<class>:<substr>");
        std::string className(args, colon - args);
        std::string substr(colon + 1);
        for (auto& c : substr) c = (char)tolower((unsigned char)c);
        int total = UObjectLookup::GetObjectCount();
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK findname");
        int found = 0;
        for (int32_t i = 0; i < total && found < 20; ++i) {
            uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
            if (!obj) continue;
            std::string cls = UObjectLookup::GetObjectClassName(obj);
            if (cls != className) continue;
            std::string name = UObjectLookup::GetObjectName(obj);
            std::string lname = name;
            for (auto& c : lname) c = (char)tolower((unsigned char)c);
            if (lname.find(substr) == std::string::npos) continue;
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  0x%llX %s", (unsigned long long)obj, name.c_str());
            if (n > 0) written += n;
            ++found;
        }
        return written;
    }
    // Everything below is raw memory — no C++ objects, safe for SEH.
    return HandleDbgRaw(cmd, args, out, outCap);
}

static int HandleDbgRaw(const char* cmd, const char* args, char* out, size_t outCap) {
    if (!strcmp(cmd, "read64")) {
        uintptr_t a = strtoull(args, nullptr, 16);
        uint64_t v = 0; bool ok = TryReadBytes(a, &v, 8);
        if (!ok) return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%016llX", (unsigned long long)v);
    }
    if (!strcmp(cmd, "read32")) {
        uintptr_t a = strtoull(args, nullptr, 16);
        uint32_t v = 0; bool ok = TryReadBytes(a, &v, 4);
        if (!ok) return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%08X (%d)", v, (int)v);
    }
    if (!strcmp(cmd, "read8")) {
        uintptr_t a = strtoull(args, nullptr, 16);
        uint8_t v = 0; bool ok = TryReadBytes(a, &v, 1);
        if (!ok) return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK 0x%02X (%d)", v, (int)v);
    }
    if (!strcmp(cmd, "dump")) {
        // dump:<addr>:<size>  (hex, both args in hex without 0x prefix ok)
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage dump:<hexaddr>:<hexsize>");
        uintptr_t a = strtoull(args, nullptr, 16);
        uintptr_t sz = strtoull(colon + 1, nullptr, 16);
        if (sz == 0 || sz > 0xC000) sz = 0x100;
        uint8_t buf[0xC000];
        if (sz > sizeof(buf)) sz = sizeof(buf);
        if (!TryReadBytes(a, buf, sz))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK");
        for (size_t i = 0; i < sz; ++i) {
            if ((i & 0xF) == 0) {
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE, "\n+0x%03zX ", i);
                if (n <= 0) break;
                written += n;
            }
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE, "%02X ", buf[i]);
            if (n <= 0) break;
            written += n;
        }
        return written;
    }
    if (!strcmp(cmd, "scan")) {
        // scan:<addr>:<range>  — find TArray-shaped fields {data, num, max}
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage scan:<hexaddr>:<hexrange>");
        uintptr_t a = strtoull(args, nullptr, 16);
        uintptr_t range = strtoull(colon + 1, nullptr, 16);
        if (range == 0 || range > 0x4000) range = 0x400;
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK");
        uint8_t buf[0x4000];
        if (range > sizeof(buf)) range = sizeof(buf);
        if (!TryReadBytes(a, buf, range)) return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        for (size_t off = 0; off + 16 <= range; off += 8) {
            uint64_t ptr = *reinterpret_cast<uint64_t*>(buf + off);
            int32_t  num = *reinterpret_cast<int32_t*>(buf + off + 8);
            int32_t  mx  = *reinterpret_cast<int32_t*>(buf + off + 12);
            if (!ptr) continue;
            if (ptr < 0x10000ULL || ptr > 0x00007FFFFFFFFFFFULL) continue;
            if (num < 0 || num > 4096) continue;
            if (mx  < num || mx  > 8192) continue;
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n+0x%03zX TArray 0x%llX num=%d max=%d", off, (unsigned long long)ptr, num, mx);
            if (n <= 0) break;
            written += n;
        }
        return written;
    }
    // ──── Write primitives ────────────────────────────────────────────
    if (!strcmp(cmd, "write8") || !strcmp(cmd, "write32") || !strcmp(cmd, "write64")) {
        // writeN:<addr>:<value>  — value in hex
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage writeN:<addr>:<hexvalue>");
        uintptr_t a = strtoull(args, nullptr, 16);
        uint64_t  v = strtoull(colon + 1, nullptr, 16);
        size_t n = (cmd[5] == '8') ? 1 : (cmd[5] == '3') ? 4 : 8;
        if (!TryWriteBytes(a, &v, n))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR write failed");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK wrote %zu bytes @0x%llX = 0x%llX",
            n, (unsigned long long)a, (unsigned long long)v);
    }
    if (!strcmp(cmd, "wbytes")) {
        // wbytes:<addr>:<hexblob>   — "DEADBEEFCAFE" writes 6 bytes
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage wbytes:<addr>:<hex>");
        uintptr_t a = strtoull(args, nullptr, 16);
        uint8_t tmp[0x2000];
        int nb = HexStringToBytes(colon + 1, tmp, sizeof(tmp));
        if (nb < 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad hex blob");
        if (!TryWriteBytes(a, tmp, nb))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR write failed");
        return _snprintf_s(out, outCap, _TRUNCATE, "OK wrote %d bytes @0x%llX",
            nb, (unsigned long long)a);
    }
    // ──── Vtable inspection ───────────────────────────────────────────
    if (!strcmp(cmd, "vtable")) {
        // vtable:<addr>:<idx>  — read uobject's vtable[idx]
        const char* colon = strchr(args, ':');
        if (!colon) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage vtable:<addr>:<idx>");
        uintptr_t a = strtoull(args, nullptr, 16);
        int idx = (int)strtoll(colon + 1, nullptr, 10);
        void** vt = nullptr;
        if (!TryReadBytes(a, &vt, 8) || !vt)
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad object or vtable");
        void* entry = nullptr;
        if (!TryReadBytes(reinterpret_cast<uintptr_t>(vt) + idx * 8, &entry, 8))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad vtable[%d]", idx);
        return _snprintf_s(out, outCap, _TRUNCATE, "OK vtable@0x%llX[%d]=0x%llX",
            (unsigned long long)vt, idx, (unsigned long long)entry);
    }
    // ──── Byte-pattern search ─────────────────────────────────────────
    if (!strcmp(cmd, "pattern")) {
        // pattern:<addr>:<rangeHex>:<hexPattern>
        const char* c1 = strchr(args, ':');
        if (!c1) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage pattern:<addr>:<range>:<hex>");
        const char* c2 = strchr(c1 + 1, ':');
        if (!c2) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage pattern:<addr>:<range>:<hex>");
        uintptr_t a = strtoull(args, nullptr, 16);
        uintptr_t range = strtoull(c1 + 1, nullptr, 16);
        if (range == 0 || range > 0x100000) range = 0x10000;
        uint8_t pat[64];
        int plen = HexStringToBytes(c2 + 1, pat, sizeof(pat));
        if (plen <= 0) return _snprintf_s(out, outCap, _TRUNCATE, "ERR bad pattern");
        static uint8_t buf[0x100000];
        if (range > sizeof(buf)) range = sizeof(buf);
        if (!TryReadBytes(a, buf, range))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK pattern matches");
        int hits = 0;
        for (size_t i = 0; i + plen <= range; ++i) {
            if (memcmp(buf + i, pat, plen) != 0) continue;
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  +0x%zX  @0x%llX", i, (unsigned long long)(a + i));
            if (n <= 0) break;
            written += n;
            if (++hits >= 64) break;
        }
        if (hits == 0) {
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE, "\n  (no matches)");
            if (n > 0) written += n;
        }
        return written;
    }
    // ──── Module info ─────────────────────────────────────────────────
    if (!strcmp(cmd, "module")) {
        // module:<name>  — returns base and size
        HMODULE h = GetModuleHandleA(args[0] ? args : nullptr);
        if (!h) return _snprintf_s(out, outCap, _TRUNCATE, "ERR module '%s' not found", args);
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi));
        return _snprintf_s(out, outCap, _TRUNCATE, "OK module '%s' base=0x%llX size=0x%X",
            args[0] ? args : "(exe)", (unsigned long long)mi.lpBaseOfDll, mi.SizeOfImage);
    }
    // ──── memmap ───────────────────────────────────────────────────────
    // VirtualQuery-walker. Lists the game's memory regions with their
    // base, size, protect flags and type. `memmap` alone walks from 0
    // up to 0x7FFF'FFFFFFFF (user-mode cap). `memmap:<start>:<range>`
    // limits the walk to a window (useful to zoom onto a specific heap
    // or module). Capped at 256 regions to keep the pipe buffer sane.
    if (!strcmp(cmd, "memmap")) {
        uintptr_t start = 0, range = 0x00007FFFFFFFFFFFULL;
        if (args[0]) {
            start = strtoull(args, nullptr, 16);
            const char* colon = strchr(args, ':');
            if (colon) range = strtoull(colon + 1, nullptr, 16);
            if (range == 0) range = 0x00007FFFFFFFFFFFULL;
        }
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK memmap start=0x%llX range=0x%llX",
            (unsigned long long)start, (unsigned long long)range);
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t addr = start;
        int count = 0;
        while (addr < start + range && count < 256) {
            SIZE_T q = VirtualQuery((void*)addr, &mbi, sizeof(mbi));
            if (q == 0) break;
            if (mbi.State != MEM_FREE) {
                const char* stateStr =
                    mbi.State == MEM_COMMIT  ? "COMMIT " :
                    mbi.State == MEM_RESERVE ? "RESERVE" : "FREE   ";
                const char* typeStr =
                    mbi.Type == MEM_IMAGE   ? "IMAGE " :
                    mbi.Type == MEM_MAPPED  ? "MAPPED" :
                    mbi.Type == MEM_PRIVATE ? "PRIVAT" : "------";
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  0x%016llX  size=0x%-10llX %s %s prot=0x%X",
                    (unsigned long long)mbi.BaseAddress,
                    (unsigned long long)mbi.RegionSize,
                    stateStr, typeStr, mbi.Protect);
                if (n <= 0) break;
                written += n;
                ++count;
            }
            addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        }
        return written;
    }
    // ──── modules ─────────────────────────────────────────────────────
    // Enumerate every loaded DLL/EXE in the target process. Lists name,
    // base, size. Capped at 128.
    if (!strcmp(cmd, "modules")) {
        HMODULE mods[512];
        DWORD needed = 0;
        HANDLE hProc = GetCurrentProcess();
        if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR EnumProcessModules failed");
        DWORD n = needed / sizeof(HMODULE);
        int written = _snprintf_s(out, outCap, _TRUNCATE, "OK modules count=%u", (unsigned)n);
        for (DWORD i = 0; i < n && i < 128; ++i) {
            MODULEINFO mi{};
            GetModuleInformation(hProc, mods[i], &mi, sizeof(mi));
            char path[MAX_PATH] = {};
            GetModuleFileNameExA(hProc, mods[i], path, sizeof(path));
            const char* leaf = strrchr(path, '\\');
            leaf = leaf ? leaf + 1 : path;
            int w = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  0x%016llX  size=0x%08X  %s",
                (unsigned long long)mi.lpBaseOfDll, mi.SizeOfImage, leaf);
            if (w <= 0) break;
            written += w;
        }
        return written;
    }
    // ──── strings ─────────────────────────────────────────────────────
    // strings:<addr>:<range>[:<minLen>]  — scan a memory region for
    // printable ASCII and UTF-16LE sequences ≥ minLen chars. Great for
    // finding item row names, log tags, etc. without inspecting blobs
    // by hand.
    if (!strcmp(cmd, "strings")) {
        const char* c1 = strchr(args, ':');
        if (!c1) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage strings:<addr>:<range>[:<minLen>]");
        uintptr_t addr  = strtoull(args, nullptr, 16);
        uintptr_t range = strtoull(c1 + 1, nullptr, 16);
        const char* c2 = strchr(c1 + 1, ':');
        int minLen = c2 ? atoi(c2 + 1) : 6;
        if (minLen < 3)      minLen = 3;
        if (minLen > 128)    minLen = 128;
        if (range == 0)      range = 0x10000;
        if (range > 0x100000) range = 0x100000;   // 1 MB cap
        static uint8_t buf[0x100000];
        if (!TryReadBytes(addr, buf, range))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK strings @0x%llX range=0x%llX minLen=%d",
            (unsigned long long)addr, (unsigned long long)range, minLen);
        int hits = 0;
        // ASCII scan
        for (size_t i = 0; i + (size_t)minLen < range && hits < 128; ) {
            size_t j = i;
            while (j < range && buf[j] >= 0x20 && buf[j] < 0x7F) ++j;
            if (j - i >= (size_t)minLen) {
                int len = (int)(j - i > 80 ? 80 : j - i);
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  0x%llX  ascii(%d)  \"%.*s\"",
                    (unsigned long long)(addr + i), (int)(j - i), len, (const char*)(buf + i));
                if (n <= 0) break;
                written += n; ++hits;
            }
            i = (j == i) ? i + 1 : j + 1;
        }
        // UTF-16LE scan (ASCII range only — avoids false positives on
        // random 2-byte pairs happening to be printable).
        for (size_t i = 0; i + (size_t)(minLen * 2) < range && hits < 256; ) {
            size_t j = i;
            while (j + 1 < range && buf[j+1] == 0 && buf[j] >= 0x20 && buf[j] < 0x7F) j += 2;
            if ((j - i) / 2 >= (size_t)minLen) {
                // Copy ASCII characters out
                char tmp[96]; int t = 0;
                for (size_t k = i; k < j && t < 80; k += 2) tmp[t++] = (char)buf[k];
                tmp[t] = 0;
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  0x%llX  utf16(%zu) \"%s\"",
                    (unsigned long long)(addr + i), (j - i) / 2, tmp);
                if (n <= 0) break;
                written += n; ++hits;
            }
            i = (j == i) ? i + 2 : j + 2;
        }
        return written;
    }
    // ──── refs ────────────────────────────────────────────────────────
    // refs:<target>:<scanStart>:<range>  — scan range on 8-byte boundary
    // for u64 values ∈ [target, target+0x8000]. Finds every pointer that
    // lands inside a struct — the "Find references" of x64dbg.
    if (!strcmp(cmd, "refs")) {
        const char* c1 = strchr(args, ':');
        const char* c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
        if (!c2) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage refs:<target>:<scanStart>:<range>");
        uintptr_t target = strtoull(args, nullptr, 16);
        uintptr_t start  = strtoull(c1 + 1, nullptr, 16);
        uintptr_t range  = strtoull(c2 + 1, nullptr, 16);
        if (range == 0 || range > 0x200000) range = 0x10000;
        static uint8_t buf[0x200000];
        if (!TryReadBytes(start, buf, range))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK refs target=0x%llX start=0x%llX range=0x%llX",
            (unsigned long long)target, (unsigned long long)start, (unsigned long long)range);
        int hits = 0;
        for (size_t off = 0; off + 8 <= range && hits < 64; off += 8) {
            uint64_t v = *reinterpret_cast<uint64_t*>(buf + off);
            if (v >= (uint64_t)target && v < (uint64_t)target + 0x8000) {
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  0x%llX  ->  0x%llX  (+0x%llX)",
                    (unsigned long long)(start + off), (unsigned long long)v,
                    (unsigned long long)(v - target));
                if (n <= 0) break;
                written += n; ++hits;
            }
        }
        return written;
    }
    // ──── search ──────────────────────────────────────────────────────
    // search:<addr>:<range>:<type>:<value>
    //   type ∈ u32 | u64 | f32 | f64 | ascii | utf16
    //   value: hex (for integers) OR decimal float (f32/f64) OR literal string
    // Scans byte-by-byte for matches; returns up to 64 offsets.
    if (!strcmp(cmd, "search")) {
        const char* c1 = strchr(args, ':');
        const char* c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
        const char* c3 = c2 ? strchr(c2 + 1, ':') : nullptr;
        if (!c3) return _snprintf_s(out, outCap, _TRUNCATE, "ERR usage search:<addr>:<range>:<type>:<value>");
        uintptr_t addr  = strtoull(args, nullptr, 16);
        uintptr_t range = strtoull(c1 + 1, nullptr, 16);
        std::string type(c2 + 1, c3 - (c2 + 1));
        const char* val = c3 + 1;
        if (range == 0 || range > 0x200000) range = 0x10000;
        static uint8_t buf[0x200000];
        if (!TryReadBytes(addr, buf, range))
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR badaddr");

        uint8_t needle[64]; int nlen = 0;
        if (type == "u32") {
            uint32_t v = (uint32_t)strtoull(val, nullptr, 16);
            memcpy(needle, &v, 4); nlen = 4;
        } else if (type == "u64") {
            uint64_t v = strtoull(val, nullptr, 16);
            memcpy(needle, &v, 8); nlen = 8;
        } else if (type == "f32") {
            float v = (float)atof(val); memcpy(needle, &v, 4); nlen = 4;
        } else if (type == "f64") {
            double v = atof(val); memcpy(needle, &v, 8); nlen = 8;
        } else if (type == "ascii") {
            nlen = (int)strlen(val);
            if (nlen > 63) nlen = 63;
            memcpy(needle, val, nlen);
        } else if (type == "utf16") {
            int src = (int)strlen(val);
            if (src > 31) src = 31;
            for (int i = 0; i < src; ++i) { needle[i*2] = (uint8_t)val[i]; needle[i*2+1] = 0; }
            nlen = src * 2;
        } else {
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR unknown type '%s' — use u32|u64|f32|f64|ascii|utf16", type.c_str());
        }
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK search %s=%s  range=0x%llX", type.c_str(), val, (unsigned long long)range);
        int hits = 0;
        for (size_t i = 0; i + (size_t)nlen <= range && hits < 64; ++i) {
            if (memcmp(buf + i, needle, nlen) == 0) {
                int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                    "\n  0x%llX", (unsigned long long)(addr + i));
                if (n <= 0) break;
                written += n; ++hits;
            }
        }
        return written;
    }
    if (!strcmp(cmd, "character")) {
        void* c = Trainer::Get().GetCharacter();
        return _snprintf_s(out, outCap, _TRUNCATE, "OK character=0x%llX Off::Player_InventoryComp=0x%llX",
            (unsigned long long)c, (unsigned long long)Off::Player_InventoryComp);
    }
    if (!strcmp(cmd, "playerinv")) {
        // Walk: character -> InventoryComponent -> scan for child UInventory
        // pointers -> for each, report class + slot-count candidates.
        void* c = Trainer::Get().GetCharacter();
        if (!c || !Off::Player_InventoryComp)
            return _snprintf_s(out, outCap, _TRUNCATE, "ERR no character");
        void* comp = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(c) + Off::Player_InventoryComp);
        int written = _snprintf_s(out, outCap, _TRUNCATE,
            "OK character=0x%llX comp=0x%llX", (unsigned long long)c, (unsigned long long)comp);
        if (!comp) return written;
        for (int off = 0; off < 0x200; off += 8) {
            uint8_t* p = reinterpret_cast<uint8_t*>(comp) + off;
            void* candidate = *reinterpret_cast<void**>(p);
            if (!candidate) continue;
            if ((uintptr_t)candidate < 0x10000 || (uintptr_t)candidate > 0x00007FFFFFFFFFFFULL) continue;
            // Check it's a UInventory by reading its +0x10 ClassPrivate
            void* cls = 0;
            if (!TryReadBytes((uintptr_t)candidate + 0x10, &cls, 8) || !cls) continue;
            // Read class's name FName
            uint64_t fname = 0;
            if (!TryReadBytes((uintptr_t)cls + 0x18, &fname, 8)) continue;
            // Read Outer (+0x20) to filter to inventories belonging to the comp
            void* outer = 0;
            if (!TryReadBytes((uintptr_t)candidate + 0x20, &outer, 8)) continue;
            // We don't know exact class name from raw read; use the full lookup
            // instead (it handles FName resolution) — but we need to call it
            // safely. Since it allocates std::string, delegate to the outer
            // C++ function on the critical path only.
            int n = _snprintf_s(out + written, outCap - written, _TRUNCATE,
                "\n  +0x%03X  obj=0x%llX  outer=0x%llX",
                off, (unsigned long long)candidate, (unsigned long long)outer);
            if (n <= 0) break;
            written += n;
        }
        return written;
    }
    return _snprintf_s(out, outCap, _TRUNCATE,
        "ERR unknown cmd '%s'. Available: "
        "classof nameof findcls findstruct findobj findname listobj "
        "props propsall propoff listfuncs funcoff callfn "
        "propget propset isa outer fname fnameof vtable module pattern "
        "read8 read32 read64 write8 write32 write64 wbytes dump scan "
        "character playerinv inv listitems dumpslot", cmd);
}

DWORD WINAPI Trainer::PipeServerThread(LPVOID param) {
    Trainer* self = static_cast<Trainer*>(param);
    LOG_PIPE("Server started on \\\\.\\pipe\\ZeusModPipe");

    while (true) {
        HANDLE pipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\ZeusModPipe",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0x10000, 0x10000, 0, nullptr);  // bump buffer for large dumps

        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }

        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            // Big enough to hold `callfn` with a fully-expanded FItemData
            // hex-encoded (0x400 bytes → 0x800 hex chars + cmd prefix).
            char buf[0x1000];
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

                    // Debug-inspector commands (dbg:<cmd>:<args>): route to
                    // the handler and write the full response back. Using
                    // dbg: prefix keeps the original cheat-toggle namespace
                    // untouched.
                    if (!strcmp(cmd, "dbg")) {
                        char* sep2 = strchr(const_cast<char*>(val), ':');
                        const char* innerCmd = val;
                        const char* innerArgs = "";
                        if (sep2) { *sep2 = 0; innerArgs = sep2 + 1; }
                        static char respBuf[0x10000];
                        int n = HandleDbgCommand(innerCmd, innerArgs, respBuf, sizeof(respBuf));
                        DWORD written = 0;
                        if (n > 0)
                            WriteFile(pipe, respBuf, (DWORD)n, &written, nullptr);
                        else {
                            const char* err = "ERR response";
                            WriteFile(pipe, err, 12, &written, nullptr);
                        }
                        continue;  // don't fall through to cheat toggles
                    }

                    if (strcmp(cmd, "godmode") == 0) self->GodMode = (v != 0);
                    else if (strcmp(cmd, "stamina") == 0) self->InfiniteStamina = (v != 0);
                    else if (strcmp(cmd, "armor") == 0) self->InfiniteArmor = (v != 0);
                    else if (strcmp(cmd, "oxygen") == 0) self->InfiniteOxygen = (v != 0);
                    else if (strcmp(cmd, "food") == 0) self->InfiniteFood = (v != 0);
                    else if (strcmp(cmd, "water") == 0) self->InfiniteWater = (v != 0);
                    else if (strcmp(cmd, "craft") == 0) self->FreeCraft = (v != 0);
                    else if (strcmp(cmd, "items") == 0) self->InfiniteItems = (v != 0);
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
                    else if (strcmp(cmd, "give") == 0) {
                        // "give" command value format: "ItemName,Count"
                        // The `val` pointer points past the ':' separator to the
                        // comma-delimited payload. Parse here and invoke.
                        const char* comma = strchr(val, ',');
                        int gcount = 1;
                        std::string itemName;
                        if (comma) {
                            itemName.assign(val, comma - val);
                            gcount = atoi(comma + 1);
                        } else {
                            itemName = val;
                        }
                        if (!itemName.empty()) {
                            Trainer_GiveItem(itemName.c_str(), gcount > 0 ? gcount : 1);
                        }
                    }

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

// ──────────────────────────────────────────────────────────────────────
// Player inventory resolver
// ──────────────────────────────────────────────────────────────────────
// UInventoryComponent stores a TArray<Entry> at +0xE8 where each entry is
// 0x20 bytes: {vtable, FName, UInventory*, weakPtr}. The FName identifies
// which bag (Quickbar / Backpack / Equipment / Suit / Upgrade) and the
// pointer at +0x10 is the actual UInventory containing the player's
// slots. Confirmed via UE4 reflection walk in scripts/inspect.py on a
// live session (slots 0..29 in a num=30/max=56 backpack contained
// Stone, Sulfur, Oxite, Refined_Metal etc).
// SEH-only worker: reads the TArray header fields raw (no std::string, no
// C++ unwind) so MSVC accepts __try (the std::string comparison lives in
// the outer C++ function below). Returns the entries buffer + count.
static bool SafeReadInventoriesTArray(void* invComp, void** outData, int32_t* outNum) {
    if (!invComp) return false;
    __try {
        uint8_t* compBytes = reinterpret_cast<uint8_t*>(invComp);
        *outData = *reinterpret_cast<void**>(compBytes + Off::InvComp_Inventories);
        *outNum  = *reinterpret_cast<int32_t*>(compBytes + Off::InvComp_Inventories + 0x08);
        return true;
    }
    __except (1) { return false; }
}
static bool SafeReadEntryPtr(uint8_t* entry, void** outBag) {
    __try {
        *outBag = *reinterpret_cast<void**>(entry + Off::InvEntry_Ptr);
        return true;
    }
    __except (1) { return false; }
}

void* Trainer::ResolvePlayerInventoryByName(const char* inventoryName) const {
    if (!inventoryName || !m_character || !Off::Player_InventoryComp) return nullptr;
    void* invComp = nullptr;
    // Raw pointer read to get the InventoryComponent — use a local SEH helper.
    struct Reader { static bool Read(void* c, uintptr_t off, void** out) {
        __try { *out = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(c) + off); return true; }
        __except (1) { return false; }
    }};
    if (!Reader::Read(m_character, Off::Player_InventoryComp, &invComp) || !invComp)
        return nullptr;

    void*   data = nullptr;
    int32_t num  = 0;
    if (!SafeReadInventoriesTArray(invComp, &data, &num)) return nullptr;
    if (!data || num <= 0 || num > 32) return nullptr;

    uint8_t* entries = reinterpret_cast<uint8_t*>(data);
    for (int32_t i = 0; i < num; ++i) {
        uint8_t* entry = entries + i * Off::InvEntry_Stride;
        // ReadFNameAt (std::string) is OK here — this function isn't __try-guarded.
        std::string name = UObjectLookup::ReadFNameAt(
            reinterpret_cast<uintptr_t>(entry + Off::InvEntry_FName));
        if (name == inventoryName) {
            void* bag = nullptr;
            if (!SafeReadEntryPtr(entry, &bag)) return nullptr;
            return bag;
        }
    }
    return nullptr;
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
