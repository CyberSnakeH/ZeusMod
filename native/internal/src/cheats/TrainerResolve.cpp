#include "Trainer.h"
#include "UObjectLookup.h"
#include "Logger.h"
#include "TrainerInternal.h"
#include <cstdint>

// ============================================================================
// Trainer::ResolveAllOffsets — replaces every Off:: namespace constant with
// the value resolved at runtime via UObjectLookup. Each property is looked up
// by its UE reflection name, walking the parent class chain if needed.
// Unresolved properties stay at 0 and the corresponding feature path is
// skipped safely. This makes ALL field offsets patch-proof across game
// updates, the same way FindNativeFunction makes function addresses
// patch-proof.
// ============================================================================

void Trainer::ResolveAllOffsets() {
    auto resolve = [](uintptr_t& slot, const char* className, const char* propName) {
        int32_t off = UObjectLookup::FindPropertyOffset(className, propName);
        if (off >= 0) {
            slot = static_cast<uintptr_t>(off);
            Log::Resume::Ok("UPROPERTY offsets");
        } else {
            LOG_WARN("unresolved property %s::%s", className, propName);
            Log::Resume::Fail("UPROPERTY offsets", "%s::%s", className, propName);
        }
    };
    auto resolveStruct = [](uintptr_t& slot, const char* structName, const char* propName) {
        int32_t off = UObjectLookup::FindStructPropertyOffset(structName, propName);
        if (off >= 0) {
            slot = static_cast<uintptr_t>(off);
            Log::Resume::Ok("struct offsets");
        } else {
            Log::Resume::Fail("struct offsets", "%s::%s", structName, propName);
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

    // --- Player character-side weight cache (HUD reads this) ---
    {
        int32_t off = UObjectLookup::FindPropertyOffset(
            "BP_IcarusPlayerCharacterSurvival_C", "CurrentWeight");
        if (off < 0) off = UObjectLookup::FindPropertyOffset("IcarusPlayerCharacterSurvival", "CurrentWeight");
        if (off >= 0) { Off::Char_CurrentWeight = static_cast<uintptr_t>(off); Log::Resume::Ok("UPROPERTY offsets"); }
        else {
            LOG_WARN("unresolved <PlayerCharacter>::CurrentWeight (kept 0x%llX)",
                (unsigned long long)Off::Char_CurrentWeight);
            Log::Resume::Fail("UPROPERTY offsets", "Character::CurrentWeight");
        }
    }

    resolve(Off::InvComp_Inventories, "InventoryComponent", "Inventories");

    // --- Struct offsets (FInventorySlot / FItemData / FItemDynamicData) ---
    resolveStruct(Off::Slot_ItemData,   "InventorySlot",   "ItemData");
    resolveStruct(Off::Item_StaticData, "ItemData",        "ItemStaticData");
    resolveStruct(Off::Item_DynamicData,"ItemData",        "ItemDynamicData");
    resolveStruct(Off::Item_DatabaseGUID,"ItemData",       "DatabaseGUID");
    resolveStruct(Off::Dyn_PropertyType,"ItemDynamicData", "PropertyType");
    resolveStruct(Off::Dyn_Value,       "ItemDynamicData", "Value");

    // --- EDynamicItemProperties enum members (avoid hardcoded 6 / 7) ---
    {
        int32_t d = UObjectLookup::ResolveEnumValue("EDynamicItemProperties", "Durability");
        if (d >= 0 && d <= 255) { Off::DynProp_Durability = static_cast<uint8_t>(d);   Log::Resume::Ok("enum members"); }
        else { Log::Resume::Fail("enum members", "EDynamicItemProperties::Durability"); }
    }
    {
        int32_t s = UObjectLookup::ResolveEnumValue("EDynamicItemProperties", "ItemableStack");
        if (s >= 0 && s <= 255) { Off::DynProp_ItemableStack = static_cast<uint8_t>(s); Log::Resume::Ok("enum members"); }
        else { Log::Resume::Fail("enum members", "EDynamicItemProperties::ItemableStack"); }
    }

    LOG_DEBUG("Off Char_CurrentWeight=0x%llX InvComp_Inventories=0x%llX FastArray_Slots=0x%llX",
        (unsigned long long)Off::Char_CurrentWeight,
        (unsigned long long)Off::InvComp_Inventories,
        (unsigned long long)Off::FastArray_Slots);
    LOG_DEBUG("Off Slot_ItemData=0x%llX Item_StaticData=0x%llX Item_DynamicData=0x%llX",
        (unsigned long long)Off::Slot_ItemData,
        (unsigned long long)Off::Item_StaticData,
        (unsigned long long)Off::Item_DynamicData);
    LOG_DEBUG("Off Dyn_PropertyType=0x%llX Dyn_Value=0x%llX DynProp[stack=%d durability=%d]",
        (unsigned long long)Off::Dyn_PropertyType,
        (unsigned long long)Off::Dyn_Value,
        (int)Off::DynProp_ItemableStack,
        (int)Off::DynProp_Durability);

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
