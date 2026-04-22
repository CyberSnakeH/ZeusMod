// ============================================================================
// TrainerGiveItem - dynamic item injection via ProcessEvent
// ----------------------------------------------------------------------------
// Pipeline (validated via Phase 1-5 recon):
//
//   1. MakeItemTemplate(FName rowName)
//      -> FItemTemplateRowHandle  (0x18 bytes: DataTablePtr, RowName, DataTableName)
//
//   2. Zero a 0x1F0-byte FItemData buffer, memcpy the handle at +0x18
//      (that's the ItemStaticData field; FItemData lays it out as the first
//       "real" member after the base classes).
//
//   3. InventoryItemLibrary::CreateItem(FItemData& templ, UObject* world)
//      -> FItemData (0x1F0 bytes, fully populated with GUID/DynamicData/etc.)
//
//   4. Inventory::ManuallyForcePlaceItem(FItemData, int32 location, bool stacking)
//      -> bool (writes into the target inventory's Slots FastArray).
//
// FName construction from a C-string would require calling FName's ctor in
// the game's address space. We avoid that entirely by pre-enumerating every
// row in D_ItemTemplate at startup: call ItemTemplateLibrary::IntToStruct(i)
// for i in [0, NumRows), capture the returned RowHandle.RowName bytes, and
// index them by name-string for later lookup.
// ============================================================================

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include "Trainer.h"

// Captured at runtime by the ConsumeItem hook in TrainerFreeCraft.cpp.
extern "C" void* g_lastConsumeInventory;
#include "UObjectLookup.h"
#include "Logger.h"

namespace {

// --- Cached resolution state ------------------------------------------------
static void*      g_dItemTemplate        = nullptr; // D_ItemTemplate singleton UDataTable
static uintptr_t  g_fn_MakeItemTemplate  = 0;
static uintptr_t  g_fn_IntToStruct       = 0;
static uintptr_t  g_fn_NumRows           = 0;
static uintptr_t  g_fn_CreateItem        = 0;       // InventoryItemLibrary::CreateItem
static uintptr_t  g_fn_ForcePlaceItem    = 0;       // Inventory::ManuallyForcePlaceItem
// Icarus ships OnServer_AddItemCheat / OnServer_AddItem on IcarusController
// (dev cheat kept in shipping build). Using the game's own code path avoids
// FMemory canary crashes from manually-allocated dyn data buffers.
static uintptr_t  g_fn_AddItemCheat      = 0;       // IcarusController::OnServer_AddItemCheat
static uintptr_t  g_fn_AddItem           = 0;       // IcarusController::OnServer_AddItem
static uintptr_t  g_fn_ProcAddItem       = 0;       // ProcessingComponent::AddItem

// D_ItemsStatic is the RUNTIME table referenced by actual FItemData in
// player slots. D_ItemTemplate is the spawn-template library — items built
// from it have the wrong DataTablePtr and the game won't render them.
// We find D_ItemsStatic by walking GObjects once and storing:
//   - its weak ptr bytes {idx (int32), serial (int32)} = 8 bytes
//   - its FName comparison index for DTName
static uint8_t    g_itemsStaticWeakPtr[8] = {};     // to splice at +0x00 of ItemStaticData
static uint8_t    g_itemsStaticDTName[8]  = {};     // to splice at +0x10 of ItemStaticData
static bool       g_itemsStaticResolved   = false;

static void ResolveItemsStaticHandle() {
    if (g_itemsStaticResolved) return;
    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string name = UObjectLookup::GetObjectName(obj);
        if (name != "D_ItemsStatic") continue;
        if (std::string n2 = UObjectLookup::GetObjectName(obj); n2.rfind("Default__", 0) == 0) continue;
        int32_t serial = UObjectLookup::GetObjectSerialNumberByIndex(i);
        // FWeakObjectPtr = {int32 ObjectIndex, int32 ObjectSerialNumber}
        *reinterpret_cast<int32_t*>(g_itemsStaticWeakPtr + 0) = i;
        *reinterpret_cast<int32_t*>(g_itemsStaticWeakPtr + 4) = serial;
        // DTName FName = { ComparisonIndex, Number=0 }
        // NamePrivate is at UObject+0x18 (Off_UObject_Name) = FName at that address
        uint64_t raw = 0;
        memcpy(&raw, reinterpret_cast<void*>(obj + 0x18), 8);
        memcpy(g_itemsStaticDTName, &raw, 8);
        g_itemsStaticResolved = true;
        LOG_INFO("[GIVE] resolved D_ItemsStatic @ 0x%llX idx=%d serial=0x%X DTName=%08llX",
            (unsigned long long)obj, i, (unsigned)serial, (unsigned long long)raw);
        return;
    }
    LOG_INFO("[GIVE] D_ItemsStatic not found in GObjects — give will use D_ItemTemplate (fallback)");
}

// FName storage: raw 8-byte FName value, indexed by lowercase row name so the
// UI lookup is case-insensitive. Built lazily on first GiveItem call.
struct ItemEntry { uint8_t fname[8]; };
static std::unordered_map<std::string, ItemEntry> g_itemLibrary;
static bool g_libraryBuilt = false;

static std::string LowerCopy(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(tolower((unsigned char)c)));
    return r;
}

static void* FindFirstItemTemplateInstance() {
    // Defined in TrainerDiagnostics.cpp — forward-declared via TrainerInternal.h,
    // but we want a local copy here to keep this TU standalone.
    int32_t total = UObjectLookup::GetObjectCount();
    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
        if (!obj) continue;
        std::string cls = UObjectLookup::GetObjectClassName(obj);
        if (cls != "ItemTemplateTable") continue;
        std::string name = UObjectLookup::GetObjectName(obj);
        // Skip CDO (Default__...)
        if (name.rfind("Default__", 0) == 0) continue;
        return reinterpret_cast<void*>(obj);
    }
    return nullptr;
}

} // anonymous namespace

// ============================================================================
// Initialization — called once during Trainer::Initialize AFTER UObjectLookup.
// Resolves all the UFunctions and caches the D_ItemTemplate instance.
// ============================================================================
void Trainer_GiveItem_Init() {
    if (!UObjectLookup::IsInitialized()) {
        LOG_INFO("[GIVE] UObjectLookup not ready — GiveItem disabled");
        return;
    }

    g_dItemTemplate = FindFirstItemTemplateInstance();
    if (!g_dItemTemplate) {
        LOG_INFO("[GIVE] D_ItemTemplate instance not found — GiveItem disabled");
        return;
    }

    uintptr_t libClsItem = UObjectLookup::FindClassByName("ItemTemplateLibrary");
    uintptr_t libClsInv  = UObjectLookup::FindClassByName("InventoryItemLibrary");
    uintptr_t clsInv     = UObjectLookup::FindClassByName("Inventory");

    if (libClsItem) {
        g_fn_MakeItemTemplate = UObjectLookup::FindFunctionInClass(libClsItem, "MakeItemTemplate");
        g_fn_IntToStruct      = UObjectLookup::FindFunctionInClass(libClsItem, "IntToStruct");
        g_fn_NumRows          = UObjectLookup::FindFunctionInClass(libClsItem, "NumRows");
    }
    if (libClsInv) {
        g_fn_CreateItem       = UObjectLookup::FindFunctionInClass(libClsInv, "CreateItem");
    }
    if (clsInv) {
        g_fn_ForcePlaceItem   = UObjectLookup::FindFunctionInClass(clsInv, "ManuallyForcePlaceItem");
    }
    uintptr_t clsCtrl = UObjectLookup::FindClassByName("IcarusController");
    if (clsCtrl) {
        g_fn_AddItemCheat = UObjectLookup::FindFunctionInClass(clsCtrl, "OnServer_AddItemCheat");
        g_fn_AddItem      = UObjectLookup::FindFunctionInClass(clsCtrl, "OnServer_AddItem");
    }
    uintptr_t clsProc = UObjectLookup::FindClassByName("ProcessingComponent");
    if (clsProc) {
        g_fn_ProcAddItem = UObjectLookup::FindFunctionInClass(clsProc, "AddItem");
    }

    // Warm up ProcessEvent on D_ItemTemplate — validates the whole pipeline.
    auto pe = UObjectLookup::ResolveProcessEvent(g_dItemTemplate);

    // Resolve D_ItemsStatic once. Used to re-target ItemStaticData after
    // CreateItem so the game accepts the item as a real inventory object.
    ResolveItemsStaticHandle();

    LOG_INFO("[GIVE] init: dt=%p  make=%p  int=%p  num=%p  create=%p  place=%p  pe=%p",
        g_dItemTemplate,
        (void*)g_fn_MakeItemTemplate, (void*)g_fn_IntToStruct, (void*)g_fn_NumRows,
        (void*)g_fn_CreateItem, (void*)g_fn_ForcePlaceItem, (void*)pe);
}

// ============================================================================
// Build the item library. Iterates IntToStruct(0..NumRows-1), captures the
// FName of every row, stores raw FName bytes keyed by lowercase string.
// ============================================================================
static bool BuildItemLibrary_Internal() {
    if (g_libraryBuilt) return true;
    if (!g_dItemTemplate || !g_fn_IntToStruct || !g_fn_NumRows) return false;

    // Get row count first.
    int32_t numRows = 0;
    {
        char buf[4]{};
        if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_NumRows, buf)) return false;
        numRows = *reinterpret_cast<int32_t*>(buf);
    }
    if (numRows <= 0 || numRows > 50000) return false;

    LOG_INFO("[GIVE] enumerating %d rows from D_ItemTemplate...", numRows);

    // IntToStruct params (from Phase 4 recon):
    //   ParmsSize = 0x18
    //   [0] IntValue (IntProperty)    off=+0x00 size=0x4
    //   [1] Return   (StructProperty) off=+0x08 size=0x10  <- FRowHandle base (DataTablePtr+RowName)
    // We only need the RowName (8 bytes) at +0x08+0x08 = +0x10.
    constexpr size_t kIntToStructBuf = 0x18;
    constexpr size_t kRowNameOff     = 0x10;

    int captured = 0;
    for (int32_t i = 0; i < numRows; ++i) {
        char buf[kIntToStructBuf]{};
        *reinterpret_cast<int32_t*>(buf) = i;
        if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_IntToStruct, buf)) continue;

        // Read the 8-byte FName value from the return struct
        uintptr_t fnameAddr = reinterpret_cast<uintptr_t>(buf) + kRowNameOff;
        std::string name = UObjectLookup::ReadFNameAt(fnameAddr);
        if (name.empty() || name == "None") continue;

        ItemEntry entry{};
        memcpy(entry.fname, buf + kRowNameOff, 8);
        g_itemLibrary[LowerCopy(name)] = entry;
        ++captured;
    }

    g_libraryBuilt = true;
    LOG_INFO("[GIVE] item library built: %d entries", captured);
    // Log a few sample entries for confirmation
    int printed = 0;
    for (const auto& kv : g_itemLibrary) {
        if (printed++ >= 10) break;
        LOG_INFO("[GIVE]   sample: '%s'", kv.first.c_str());
    }
    return true;
}

// ============================================================================
// Public entry point. Places a freshly created stack of `count` items named
// `rawName` (case-insensitive) into the player's inventory.
//
// Returns true on success, false on any failure (unknown item, call chain
// fault, placement rejected, etc.).
// ============================================================================
bool Trainer_GiveItem(const char* rawName, int count) {
    if (!rawName || !*rawName) return false;
    if (!g_dItemTemplate || !g_fn_MakeItemTemplate ||
        !g_fn_CreateItem || !g_fn_ForcePlaceItem) {
        LOG_INFO("[GIVE] not initialized — aborting");
        return false;
    }
    if (!g_libraryBuilt) BuildItemLibrary_Internal();

    // Find the item by lowercase name lookup.
    std::string key = LowerCopy(rawName);
    auto it = g_itemLibrary.find(key);
    if (it == g_itemLibrary.end()) {
        LOG_INFO("[GIVE] unknown item: '%s' (library has %zu entries)", rawName, g_itemLibrary.size());
        return false;
    }
    const ItemEntry& entry = it->second;

    // ---- Step 1: MakeItemTemplate(FName) -> FItemTemplateRowHandle (0x18) --
    // Params: [0] RowName FName @+0x00 (8B),  [1] Return @+0x08 size=0x18
    constexpr size_t kMakeBuf = 0x20;
    char makeBuf[kMakeBuf]{};
    memcpy(makeBuf + 0x00, entry.fname, 8);
    if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_MakeItemTemplate, makeBuf)) {
        LOG_INFO("[GIVE] MakeItemTemplate call failed");
        return false;
    }
    uint8_t handle[0x18];
    memcpy(handle, makeBuf + 0x08, 0x18);

    // Sanity: DataTablePtr at +0x00 must be non-null (should be D_ItemTemplate).
    if (*reinterpret_cast<void**>(handle) == nullptr) {
        LOG_INFO("[GIVE] handle has null DataTablePtr — item row not valid");
        return false;
    }
    {
        void* dtp = *reinterpret_cast<void**>(handle);
        std::string dtcls = UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(dtp));
        LOG_INFO("[GIVE]   handle: DataTablePtr=%p(%s)  RowName=%08llX_%08llX  DTName=%08llX_%08llX",
            dtp, dtcls.c_str(),
            *(uint64_t*)(handle + 0x08) >> 32, *(uint64_t*)(handle + 0x08) & 0xFFFFFFFF,
            *(uint64_t*)(handle + 0x10) >> 32, *(uint64_t*)(handle + 0x10) & 0xFFFFFFFF);
    }

    // ---- Step 2: build FItemData template buffer --------------------------
    // Layout: zero 0x1F0 bytes, memcpy handle at +0x18 (ItemStaticData field).
    constexpr size_t kItemDataSize = 0x1F0;
    uint8_t itemTemplate[kItemDataSize]{};
    memcpy(itemTemplate + 0x18, handle, 0x18);

    // ---- Step 3: CreateItem(template, world) -> FItemData -----------------
    // Params: [0] ItemData@+0x00 size=0x1F0 (ref/const), [1] World@+0x1F0 Object ptr,
    //         [2] Return@+0x1F8 size=0x1F0
    constexpr size_t kCreateBuf = 0x3E8;
    uint8_t createBuf[kCreateBuf]{};
    memcpy(createBuf + 0x00,  itemTemplate, kItemDataSize);
    *reinterpret_cast<void**>(createBuf + 0x1F0) = g_dItemTemplate; // any UObject works for WorldContext
    if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_CreateItem, createBuf)) {
        LOG_INFO("[GIVE] CreateItem call failed");
        return false;
    }
    uint8_t finalItem[kItemDataSize];
    memcpy(finalItem, createBuf + 0x1F8, kItemDataSize);

    // ---- Step 3b: Re-target ItemStaticData to D_ItemsStatic ---------------
    // MakeItemTemplate returns a handle into D_ItemTemplate, but actual
    // player-side FItemData references D_ItemsStatic. We swap the 8-byte
    // DataTablePtr weak-obj-ptr and the 8-byte DTName FName so the game's
    // lookups land on the right table. RowName stays as-is.
    if (!g_itemsStaticResolved) ResolveItemsStaticHandle();
    if (g_itemsStaticResolved) {
        memcpy(finalItem + 0x18 + 0x00, g_itemsStaticWeakPtr, 8);  // DataTablePtr
        memcpy(finalItem + 0x18 + 0x10, g_itemsStaticDTName,  8);  // DTName
    }

    // ---- Step 3c: Leave ItemDynamicData empty (null TArray) ---------------
    // We MUST NOT point the TArray at DLL-static memory. Icarus calls
    // FItemData::operator= from UInventory::UpdateInventorySlots every
    // game tick, and the destructor calls FMemory::Free on the TArray
    // buffer. Buffers not allocated via GMalloc trigger:
    //   "FMallocBinned2 Attempt to realloc an unrecognized block"  → fatal.
    // Explicit null TArray (ptr=0, num=0, max=0) is safely freed (no-op).
    // The game's OnServer_AddItem populates dyn data (stack count, weight,
    // durability, etc.) server-side when the item lands in the bag.
    uint8_t* dynTArray = finalItem + Off::Item_DynamicData;
    memset(dynTArray, 0, 16);  // {ptr=null, num=0, max=0}

    // Dump the first 0x40 bytes of the patched item for diagnostic.
    {
        char hex[0x40 * 3 + 1]; int hi = 0;
        for (int b = 0; b < 0x40; ++b)
            hi += snprintf(hex + hi, sizeof(hex) - hi, "%02X ", finalItem[b]);
        LOG_INFO("[GIVE]   finalItem[0..3F] (patched): %s", hex);
    }

    // ---- Step 4: Force-place into player BACKPACK -------------------------
    // Authoritative resolution (confirmed via UE4 reflection + live memory):
    //   character + 0x758 -> UInventoryComponent
    //     component + 0xE8 (Inventories) -> TArray of 0x20-byte entries
    //       each entry = {vtable, FName name, UInventory* bag, weakPtr}
    //       names observed: Quickbar, Backpack, Equipment, Suit, Upgrade
    // Dropping an item into Backpack's Slots TArray makes it appear in
    // the player's main bag. Quickbar is an alternative target.
    void* character = Trainer::Get().GetCharacter();
    if (!character || !Off::Player_InventoryComp) {
        LOG_INFO("[GIVE] player inventory not available");
        return false;
    }

    std::vector<void*> candidateInventories;
    // Preferred: the real Backpack UInventory resolved by name.
    void* backpack = Trainer::Get().ResolvePlayerBackpack();
    if (backpack) {
        LOG_INFO("[GIVE] resolved Backpack @ %p  class=%s",
            backpack,
            UObjectLookup::GetObjectClassName(reinterpret_cast<uintptr_t>(backpack)).c_str());
        candidateInventories.push_back(backpack);
    }
    // Fallback: Quickbar (smaller but still a real UInventory).
    void* quickbar = Trainer::Get().ResolvePlayerQuickbar();
    if (quickbar && quickbar != backpack) {
        LOG_INFO("[GIVE] also trying Quickbar @ %p", quickbar);
        candidateInventories.push_back(quickbar);
    }
    // Last-resort: the most recent UInventory the game passed to our
    // ConsumeItem hook (if any crafting/consumption happened this session).
    if (g_lastConsumeInventory) {
        std::string ccls = UObjectLookup::GetObjectClassName(
            reinterpret_cast<uintptr_t>(g_lastConsumeInventory));
        if (ccls == "Inventory" && g_lastConsumeInventory != backpack
            && g_lastConsumeInventory != quickbar) {
            LOG_INFO("[GIVE] fallback candidate g_lastConsumeInventory @ %p",
                g_lastConsumeInventory);
            candidateInventories.push_back(g_lastConsumeInventory);
        }
    }

    if (candidateInventories.empty()) {
        LOG_INFO("[GIVE] no player bag found — InvComp.Inventories walk returned nothing");
        return false;
    }

    // =================================================================
    // PREFERRED: Route through IcarusController::OnServer_AddItemCheat.
    // This is a dev-cheat UFunction shipped in the binary. It takes
    // (SourceInventory*, FItemData) and internally:
    //   - Validates the item against D_ItemsStatic
    //   - Allocates ItemDynamicData via UE's FMemory (no canary crash)
    //   - Adds to the player's bag with correct replication
    //
    // Fallback: if the cheat function isn't resolved (unlikely), do a
    // direct memory write into the first free slot — accepting that
    // stack count may be zero until the server re-syncs.
    // =================================================================
    bool ok = false;

    // Find the PLAYER's IcarusController (not the CDO). Each session it
    // shows up as BP_IcarusPlayerControllerSurvival_C (blueprint subclass).
    void* playerCtrl = nullptr;
    {
        int32_t total = UObjectLookup::GetObjectCount();
        for (int32_t i = 0; i < total; ++i) {
            uintptr_t obj = UObjectLookup::GetObjectByIndex(i);
            if (!obj) continue;
            std::string cls = UObjectLookup::GetObjectClassName(obj);
            if (cls.find("IcarusPlayerController") == std::string::npos
                && cls != "IcarusController") continue;
            std::string name = UObjectLookup::GetObjectName(obj);
            if (name.rfind("Default__", 0) == 0) continue;
            playerCtrl = reinterpret_cast<void*>(obj);
            LOG_INFO("[GIVE] player controller @ %p  class=%s  name=%s",
                playerCtrl, cls.c_str(), name.c_str());
            break;
        }
    }

    // For stackable items the game adds 1 per call (the stack size it
    // chooses seems to come from the item's max-stack config, so looping
    // count times delivers count units overall). Cap loop to avoid runaway.
    int toGive = (count > 0 && count <= 999) ? count : 1;
    int delivered = 0;

    // ── BEST PATH: clone a live valid FItemData as the template body,
    // patch RowName, and fire it into OnServer_AddItem on the player's
    // IcarusController. This works because the game deep-copies the
    // FItemData server-side (allocating fresh dyn data via FMemory) and
    // validates the ItemStaticData against D_ItemsStatic.
    //
    // The template carrier is any currently-occupied slot in any of the
    // player's bags — its ItemStaticData has the correct weak-obj-ptr
    // for D_ItemsStatic and its ItemDynamicData TArray points to a real
    // UE-heap allocation (so the operator= copy inside the RPC frees it
    // cleanly). We only swap the 8-byte RowName at +0x20.
    auto findTemplateFItemData = [&](uint8_t* out0x1F0) -> bool {
        void* bags[] = { backpack, quickbar };
        for (void* bag : bags) {
            if (!bag) continue;
            uint8_t* bb = reinterpret_cast<uint8_t*>(bag);
            void*   data = *reinterpret_cast<void**>(bb + Off::FastArray_Slots);
            int32_t snum = *reinterpret_cast<int32_t*>(bb + Off::FastArray_Slots + 0x08);
            if (!data || snum <= 0 || snum > 128) continue;
            uint8_t* slots = reinterpret_cast<uint8_t*>(data);
            for (int32_t i = 0; i < snum; ++i) {
                uint8_t* slot = slots + i * Off::Slot_Size;
                uint8_t* item = slot + Off::Slot_ItemData;
                void* dt = *reinterpret_cast<void**>(item + Off::Item_StaticData);
                if (!dt) continue;    // empty slot
                memcpy(out0x1F0, item, kItemDataSize);
                return true;
            }
        }
        return false;
    };

    if (playerCtrl && g_fn_AddItem) {
        uint8_t templateItem[kItemDataSize];
        bool haveTemplate = findTemplateFItemData(templateItem);
        if (!haveTemplate) {
            LOG_INFO("[GIVE] no populated slot to use as FItemData template — bag must be empty");
        }
        // Patch RowName to target item (fname bytes already cached in g_itemLibrary).
        if (haveTemplate) {
            memcpy(templateItem + Off::Item_StaticData + 0x08, entry.fname, 8);
        }
        for (int k = 0; k < toGive && haveTemplate; ++k) {
            uint8_t params[0x1F0]{};
            memcpy(params, templateItem, kItemDataSize);
            bool cok = UObjectLookup::CallUFunction(playerCtrl, g_fn_AddItem, params);
            if (k == 0) LOG_INFO("[GIVE]   OnServer_AddItem(row=%s) call=%d", rawName, (int)cok);
            if (!cok) break;
            ++delivered;
        }
        ok = (delivered > 0);
    } else if (playerCtrl && g_fn_AddItemCheat) {
        // Fallback: same body via OnServer_AddItemCheat (takes an extra
        // SourceInventory param). Tried second because OnServer_AddItem
        // proved reliable in live testing, AddItemCheat sometimes returns
        // call=1 but silently discards the item.
        uint8_t templateItem[kItemDataSize];
        bool haveTemplate = findTemplateFItemData(templateItem);
        if (haveTemplate) {
            memcpy(templateItem + Off::Item_StaticData + 0x08, entry.fname, 8);
        }
        void* targetBag = candidateInventories.front();
        for (int k = 0; k < toGive && haveTemplate; ++k) {
            uint8_t params[0x1F8]{};
            *reinterpret_cast<void**>(params + 0x000) = targetBag;
            memcpy(params + 0x008, templateItem, kItemDataSize);
            bool cok = UObjectLookup::CallUFunction(playerCtrl, g_fn_AddItemCheat, params);
            if (k == 0) LOG_INFO("[GIVE]   OnServer_AddItemCheat(bag=%p, row=%s) call=%d",
                targetBag, rawName, (int)cok);
            if (!cok) break;
            ++delivered;
        }
        ok = (delivered > 0);
    } else {
        LOG_INFO("[GIVE] controller / cheat UFunction not resolved — skipping");
    }

    LOG_INFO("[GIVE] '%s' delivered=%d/%d placed=%d", rawName, delivered, toGive, (int)ok);
    return ok;
}

// ============================================================================
// Trainer_AddItemToProcessor — deliver `count` units of `rawName` into the
// OUTPUT slot of a specific UProcessingComponent (furnace, biofuel generator,
// breeding station, etc.). Used by FreeCraft v2 when the recipe was queued on
// a workbench / station so the output lands in the station's own bag instead
// of the player's backpack.
//
// Uses the game's UProcessingComponent::AddItem(FItemData) UFunction — same
// body Trainer_GiveItem would use, but different target object so the bag
// routing matches what a legit craft would have produced.
// ============================================================================
bool Trainer_AddItemToProcessor(void* procComp, const char* rawName, int count) {
    if (!procComp || !rawName || !*rawName) return false;
    if (!g_dItemTemplate || !g_fn_MakeItemTemplate ||
        !g_fn_CreateItem || !g_fn_ProcAddItem) {
        LOG_INFO("[GIVE-PROC] not initialized — aborting");
        return false;
    }
    if (!g_libraryBuilt) BuildItemLibrary_Internal();

    std::string key = LowerCopy(rawName);
    auto it = g_itemLibrary.find(key);
    if (it == g_itemLibrary.end()) {
        LOG_INFO("[GIVE-PROC] unknown item: '%s'", rawName);
        return false;
    }
    const ItemEntry& entry = it->second;

    // Build finalItem via the same MakeItemTemplate → CreateItem → D_ItemsStatic
    // re-target pipeline Trainer_GiveItem uses.
    char makeBuf[0x20]{};
    memcpy(makeBuf + 0x00, entry.fname, 8);
    if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_MakeItemTemplate, makeBuf)) return false;
    uint8_t handle[0x18]; memcpy(handle, makeBuf + 0x08, 0x18);
    if (*reinterpret_cast<void**>(handle) == nullptr) return false;

    constexpr size_t kItemDataSize = 0x1F0;
    uint8_t itemTemplate[kItemDataSize]{};
    memcpy(itemTemplate + 0x18, handle, 0x18);

    uint8_t createBuf[0x3E8]{};
    memcpy(createBuf + 0x00, itemTemplate, kItemDataSize);
    *reinterpret_cast<void**>(createBuf + 0x1F0) = g_dItemTemplate;
    if (!UObjectLookup::CallUFunction(g_dItemTemplate, g_fn_CreateItem, createBuf)) return false;
    uint8_t finalItem[kItemDataSize];
    memcpy(finalItem, createBuf + 0x1F8, kItemDataSize);

    if (!g_itemsStaticResolved) ResolveItemsStaticHandle();
    if (g_itemsStaticResolved) {
        memcpy(finalItem + 0x18 + 0x00, g_itemsStaticWeakPtr, 8);
        memcpy(finalItem + 0x18 + 0x10, g_itemsStaticDTName,  8);
    }
    // Null dyn TArray — game will populate it when it processes the AddItem.
    memset(finalItem + 0x30, 0, 16);

    int toGive = (count > 0 && count <= 999) ? count : 1;
    int delivered = 0;
    for (int k = 0; k < toGive; ++k) {
        uint8_t params[kItemDataSize]{};
        memcpy(params, finalItem, kItemDataSize);
        bool ok = UObjectLookup::CallUFunction(procComp, g_fn_ProcAddItem, params);
        if (k == 0) LOG_INFO("[GIVE-PROC] AddItem(proc=%p row=%s) call=%d",
            procComp, rawName, (int)ok);
        if (!ok) break;
        ++delivered;
    }
    LOG_INFO("[GIVE-PROC] '%s' delivered=%d/%d", rawName, delivered, toGive);
    return delivered > 0;
}

// Expose a simple "test" entry called once from Trainer::Initialize so we
// can see the library build + verify the pipeline end-to-end. Disabled at
// runtime by Trainer.cpp unless explicitly requested.
// Sorted alphabetical list of all known item row names (lowercase). Built
// lazily on first call so the cost is paid at UI-open time, not at inject.
const std::vector<std::string>& Trainer_GiveItem_GetAllNames() {
    static std::vector<std::string> s_sorted;
    static bool s_built = false;
    if (!s_built) {
        if (!g_libraryBuilt) BuildItemLibrary_Internal();
        s_sorted.clear();
        s_sorted.reserve(g_itemLibrary.size());
        for (const auto& kv : g_itemLibrary) s_sorted.push_back(kv.first);
        std::sort(s_sorted.begin(), s_sorted.end());
        s_built = true;
    }
    return s_sorted;
}

void Trainer_GiveItem_LogSamples() {
    if (!g_dItemTemplate) return;
    BuildItemLibrary_Internal();
    LOG_INFO("[GIVE] library size=%zu", g_itemLibrary.size());
    // Check some expected keys so we can confirm IDs are usable
    for (const char* probe : {"Wood", "Stone", "Stick", "Thatch", "Fiber", "Iron_Ore"}) {
        auto it = g_itemLibrary.find(LowerCopy(probe));
        LOG_INFO("[GIVE]   '%s' %s", probe, it == g_itemLibrary.end() ? "NOT FOUND" : "OK");
    }
}
