#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstdio>

// ============================================================================
// Raw offset reader - no struct alignment issues
// ============================================================================
template<typename T>
inline T ReadAt(void* base, uintptr_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

template<typename T>
inline void WriteAt(void* base, uintptr_t offset, T value) {
    *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset) = value;
}

template<typename T>
inline T* PtrAt(void* base, uintptr_t offset) {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + offset);
}

// ============================================================================
// Runtime-resolved offsets populated by Trainer::ResolveAllOffsets().
// Zero means unresolved for the current build.
// ============================================================================
namespace Off {
    // UWorld / UGameInstance / UPlayer / AController
    inline uintptr_t World_GameInstance = 0;

    // UGameInstance -> TArray<ULocalPlayer*>
    inline uintptr_t GI_LocalPlayers = 0; // TArray: data at +0, count at +8

    // UPlayer
    inline uintptr_t Player_Controller = 0;

    // AController
    inline uintptr_t Ctrl_Character = 0;

    // AIcarusCharacter
    inline uintptr_t Char_ActorState = 0;
    inline uintptr_t Char_InstanceComponents = 0;   // TArray
    inline uintptr_t Char_BlueprintComponents = 0;  // TArray

    // UActorState
    inline uintptr_t State_Health = 0;
    inline uintptr_t State_MaxHealth = 0;
    inline uintptr_t State_Armor = 0;
    inline uintptr_t State_MaxArmor = 0;
    inline uintptr_t State_AliveState = 0;

    // UCharacterState (extends UActorState)
    inline uintptr_t State_Stamina = 0;
    inline uintptr_t State_MaxStamina = 0;

    // USurvivalCharacterState (extends UCharacterState)
    // Note: TotalExperience, Level and InternalTemperature also live on
    // SurvivalCharacterState per the SDK dump, not on a PlayerCharacterState
    // subclass. Using SurvivalCharacterState for lookup is the safe choice.
    inline uintptr_t State_Oxygen = 0;
    inline uintptr_t State_Water = 0;
    inline uintptr_t State_Food = 0;
    inline uintptr_t State_MaxOxygen = 0;
    inline uintptr_t State_MaxWater = 0;
    inline uintptr_t State_MaxFood = 0;
    inline uintptr_t State_ModInternalTemp = 0; // ModifiedInternalTemperature (HUD)
    inline uintptr_t State_InternalTemp = 0;    // InternalTemperature (raw, mutated by sim)
    inline uintptr_t State_TotalExp = 0;        // TotalExperience (int32)
    inline uintptr_t State_Level = 0;           // Level (int32)

    // AController -> APlayerState (IcarusPlayerState)
    inline uintptr_t Ctrl_PlayerState = 0;

    // AIcarusPlayerState -> embedded profile structs
    // Fallback defaults match the current SDK dump; ResolveAllOffsets
    // replaces them with reflection-walked values when the UPROPERTY is
    // discoverable on the live class.
    inline uintptr_t PS_ActiveUserProfile = 0x358; // struct OnlineProfileUser
    inline uintptr_t PS_ActiveCharacter   = 0x3A0; // struct OnlineProfileCharacter

    // Struct-internal offsets (OnlineProfileCharacter / OnlineProfileUser /
    // MetaResource). These are not UClass properties so they can't be
    // resolved through FindPropertyOffset; they are stable in the current
    // UE 4.27 build and come from the SDK dump.
    constexpr uintptr_t OPC_MetaResources = 0x48; // TArray<MetaResource>
    constexpr uintptr_t OPU_MetaResources = 0x10; // TArray<MetaResource>
    constexpr uintptr_t MR_MetaRow        = 0x00; // FString { wchar_t*, len, max }
    constexpr uintptr_t MR_Count          = 0x10; // int32
    constexpr uintptr_t MR_Size           = 0x18; // sizeof(MetaResource)

    // ACharacter -> CharacterMovement
    inline uintptr_t Char_MovementComp = 0;

    // UCharacterMovementComponent
    inline uintptr_t CMC_MaxWalkSpeed = 0;
    inline uintptr_t CMC_MaxWalkSpeedCrouched = 0;
    inline uintptr_t CMC_MaxSwimSpeed = 0;
    inline uintptr_t CMC_MaxFlySpeed = 0;

    // AActor
    inline uintptr_t Actor_CustomTimeDilation = 0;

    // UWorld
    inline uintptr_t World_GameState = 0;

    // AIcarusGameStateSurvival
    inline uintptr_t GS_TimeOfDay = 0;
    inline uintptr_t GS_SecondsPerGameDay = 0;
    inline uintptr_t GS_ReplicatedLastSessionProspectGameTime = 0;

    // AIcarusPlayerCharacter / BP_IcarusPlayerCharacterSurvival_C
    inline uintptr_t Player_InventoryComp = 0;
    // Cached character-side weight total the encumbrance HUD reads.
    // Resolved at boot via FindPropertyOffset — defaults to 0x1010 as a
    // fallback from the build used while writing this mod.
    inline uintptr_t Char_CurrentWeight = 0x1010;

    // UInventory (all offsets resolved via UE4 reflection at boot;
    // hardcoded defaults are kept as a last-resort fallback if the
    // reflection lookup fails — e.g. in an edge-case build where the
    // property name changed but the layout didn't).
    inline uintptr_t Inv_CurrentWeight = 0xE8;   // float
    inline uintptr_t Inv_Slots         = 0xF0;   // FInventorySlotsFastArray

    // Offset from Inventory* to the Items TArray header INSIDE the
    // FFastArraySerializer struct. Computed at boot as
    //   Inv_Slots + FastArrayItems_Off
    // where FastArrayItems_Off is discovered by scanning the
    // FInventorySlotsFastArray struct's ChildProperties via reflection.
    inline uintptr_t FastArray_Slots   = 0x1F8;

    // UInventoryComponent — Inventories is a TArray of 0x20-byte entries
    // {vtable, FName, UInventory*, weakPtr}. Names observed in live game:
    // Quickbar, Backpack, Equipment, Suit, Upgrade.
    inline uintptr_t InvComp_Inventories = 0xE8;
    // Layout inside each entry — these are ABI-stable for the engine's
    // TScriptInterface / native TArray<FSomethingEntry> pattern. Exposed
    // here so we can re-point them if a future UE update changes the
    // pattern (unlikely but cheap to accommodate).
    inline uintptr_t InvEntry_Stride     = 0x20;
    inline uintptr_t InvEntry_FName      = 0x08;
    inline uintptr_t InvEntry_Ptr        = 0x10;

    // FInventorySlot — resolved at boot via findstruct + propswalk.
    inline uintptr_t Slot_ItemData = 0x10;
    inline uintptr_t Slot_Size     = 0x240;

    // FItemData — same story, resolved at boot.
    inline uintptr_t Item_StaticData   = 0x18;   // FItemTemplateRowHandle
    inline uintptr_t Item_DynamicData  = 0x30;   // TArray<FItemDynamicData>
    inline uintptr_t Item_DatabaseGUID = 0x1B8;

    // FItemDynamicData layout — PropertyType uint8 at +0, Value int32 at +4,
    // stride 8 bytes. Resolved via findstruct ItemDynamicData + propswalk.
    inline uintptr_t Dyn_PropertyType = 0x00;
    inline uintptr_t Dyn_Value        = 0x04;
    inline uintptr_t Dyn_Size         = 0x08;

    // EDynamicItemProperties members — resolved at boot via ResolveEnumValue
    // so a future enum reorder doesn't silently break InfiniteItems.
    inline uint8_t DynProp_ItemableStack = 7;
    inline uint8_t DynProp_Durability    = 6;

    // UModifierStateComponent
    inline uintptr_t Mod_Lifetime = 0;
    inline uintptr_t Mod_Remaining = 0;
}

class Trainer {
public:
    static Trainer& Get() { static Trainer t; return t; }

    void Initialize();
    void Shutdown();
    void Tick();

    bool GodMode = false;
    bool InfiniteStamina = false;
    bool InfiniteArmor = false;
    bool InfiniteOxygen = false;
    bool InfiniteFood = false;
    bool InfiniteWater = false;
    bool SpeedHack = false;
    float SpeedMultiplier = 2.0f;
    bool FreeCraft = false;  // Infinite item stacks
    bool InfiniteItems = false; // No-op UInventory::ConsumeItem so food, torches, arrows,
                                // fuel etc never decrement. Also per-tick: pin every tool
                                // & armor piece's Condition (dyn type 6) to 999999, so
                                // durability never drops during use.
    bool NoWeight = false;   // Zero inventory weight
    bool TimeLock = false;   // Lock time of day
    float LockedTime = 12.0f; // Default: noon (0-24)

    // SurvivalCharacterState::ModifiedInternalTemperature clamp
    bool StableTemperature = false;
    int  StableTempValue = 20;   // degrees celsius, user configurable

    // PlayerCharacterState::TotalExperience pump
    bool MegaExp = false;

    // IcarusPlayerState::ActiveCharacter::MetaResources[MetaRow] clamps
    bool MaxTalentPoints = false;
    bool MaxTechPoints   = false;
    // IcarusPlayerState::ActiveUserProfile::MetaResources[MetaRow] clamp
    bool MaxSoloPoints   = false;

    bool IsReady() const { return m_actorState != nullptr; }

    // Fast god mode - called from dedicated thread at 1ms intervals
    void TickGodModefast();

    // Raw character / actor pointers (used by GiveItem and other helpers
    // that live in separate TUs and need read access). Kept as plain
    // getters rather than friends so we don't extend Trainer's private API.
    void* GetCharacter() const { return m_character; }
    void* GetPlayerState() const { return m_playerState; }
    // Resolves the player's Backpack UInventory* by walking:
    //   character + Player_InventoryComp -> UInventoryComponent
    //   UInventoryComponent + InvComp_Inventories -> TArray<Entry>
    //   Entry with FName == "Backpack" -> UInventory*
    // Returns nullptr if anything is missing. Used by InfiniteItems
    // and GiveItem to find the authoritative slot TArray.
    void* ResolvePlayerInventoryByName(const char* inventoryName) const;
    void* ResolvePlayerBackpack() const { return ResolvePlayerInventoryByName("Backpack"); }
    void* ResolvePlayerQuickbar() const { return ResolvePlayerInventoryByName("Quickbar"); }

    // For overlay status display
    int GetHealth() const;
    int GetMaxHealth() const;
    int GetStamina() const;
    int GetMaxStamina() const;
    int GetArmor() const;
    int GetMaxArmor() const;

private:
    Trainer() = default;
    void FindPlayer();
    void RemoveDebuffs();
    void PatchSetHealth(bool enable);
    void StartPipeServer();
    static DWORD WINAPI PipeServerThread(LPVOID param);
    void PatchRemoveItem(bool enable);
    void PatchFreeCraftItems(bool enable);
    void PatchFreeCraftProcessorGates(bool enable);
    void PatchWeight(bool enable);
    void ForceRefreshEncumbranceWidget();
    // NoWeight root-cause hook: short-circuits
    // IcarusFunctionLibrary::AddModifierState when the incoming row is
    // "Overburdened" and NoWeight is enabled. Installed once after
    // UObjectLookup is ready; toggling the cheat flips behaviour at
    // runtime without re-hooking.
    void InstallWeightHook();

    // Resolve active UPROPERTY offsets in the Off:: namespace via
    // UObjectLookup. Called once at trainer init after UObjectLookup::Initialize.
    void ResolveAllOffsets();

    // One-shot diagnostic dump printed the first time a live player is
    // located. Lives in its own method (no SEH) because __try/__except
    // can't coexist with the std::string objects from UObjectLookup.
    void RunOnceDiagnostics();

    // GetTotalWeight patch (return 0)
    uintptr_t m_weightAddr = 0;
    uint8_t m_weightBackup[4] = {};
    bool m_weightPatched = false;

    void PatchBytes(uintptr_t addr, const uint8_t* patch, uint8_t* backup, int size, bool enable, bool& patched, const char* name);

    // GetScaledRecipeInputCount patch (return 1 - production bails on 0)
    uintptr_t m_scaledInputAddr = 0;
    uint8_t m_scaledInputBackup[6] = {};
    bool m_scaledInputPatched = false;

    // GetScaledRecipeResourceItemCount patch (return 1)
    uintptr_t m_scaledResourceAddr = 0;
    uint8_t m_scaledResourceBackup[6] = {};
    bool m_scaledResourcePatched = false;

    // FindItemCountByType patch (return 9999 = always have items)
    uintptr_t m_findItemCountAddr = 0;
    uint8_t m_findItemCountBackup[6] = {};
    bool m_findItemCountPatched = false;

    // CanSatisfyRecipeQueryInput patch (B0 01 C3 = mov al,1; ret).
    // Byte-patch, not a hook: this is a Blueprint UFunction and a __fastcall
    // detour crashes on it, but overwriting the prologue with "return true"
    // is safe. Removes the "need at least 1 of each material" limitation.
    uintptr_t m_canSatisfyQueryAddr = 0;
    uint8_t m_canSatisfyQueryBackup[3] = {};
    bool m_canSatisfyQueryPatched = false;

    // GetItemCount patch (return 9999). DIFFERENT from FindItemCountByType:
    // this one walks the inventory slot array and sums counts for a given
    // item type. The craft UI uses this for the "X/Y materials" display
    // and the craft button enable check.
    uintptr_t m_getItemCountAddr = 0;
    uint8_t m_getItemCountBackup[6] = {};
    bool m_getItemCountPatched = false;

    // ConsumeItem sub patch (4 bytes: 44 29 66 04)
    uintptr_t m_removeItemAddr = 0;
    uint8_t m_removeItemBackup[4] = {};
    bool m_removeItemPatched = false;

    // Non-material processor gates. Keep these separate from item-cost
    // bypasses so we can enable "craft anyway" without touching tick paths.
    uintptr_t m_shelterRequirementsAddr = 0;
    uint8_t m_shelterRequirementsBackup[3] = {};
    bool m_shelterRequirementsPatched = false;

    uintptr_t m_canStartProcessingAddr = 0;
    uint8_t m_canStartProcessingBackup[3] = {};
    bool m_canStartProcessingPatched = false;

public:
    float m_originalWalkSpeed = 0.0f;
    float m_origSpeeds[5] = {};
private:

    void** m_gworldPtr = nullptr;
    void* m_character = nullptr;
    void* m_actorState = nullptr;
    void* m_playerState = nullptr;  // AIcarusPlayerState, reached via controller
    void* m_expComp = nullptr;      // ExperienceComponent on the player char
    void* m_playerTalentCtrl = nullptr;  // PlayerTalentControllerComponent instance
    void* m_soloTalentCtrl = nullptr;    // SoloTalentControllerComponent instance
    void* m_playerTalentModel = nullptr; // BlueprintTalentModel live instance
    void* m_soloTalentModel = nullptr;   // SoloTalentModel live instance
    int m_retryTimer = 0;
    FILE* m_con = nullptr;
    bool m_prevFreeCraft = false;

    // SetHealth write instruction patch (6 bytes NOP)
    uintptr_t m_setHealthAddr = 0;
    uint8_t m_setHealthBackup[6] = {};
    bool m_setHealthPatched = false;

    // FreeCraft item-path patches
};
