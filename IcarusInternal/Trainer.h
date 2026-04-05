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
// Confirmed offsets from UE4SS SDK dump
// ============================================================================
namespace Off {
    // UWorld
    constexpr uintptr_t World_GameInstance = 0x0D28;

    // UGameInstance -> TArray<ULocalPlayer*>
    constexpr uintptr_t GI_LocalPlayers = 0x38; // TArray: data at +0, count at +8

    // UPlayer
    constexpr uintptr_t Player_Controller = 0x30;

    // AController
    constexpr uintptr_t Ctrl_Character = 0x260;

    // AIcarusCharacter
    constexpr uintptr_t Char_ActorState = 0x5A8;
    constexpr uintptr_t Char_InstanceComponents = 0x1F0;   // TArray
    constexpr uintptr_t Char_BlueprintComponents = 0x200;  // TArray

    // UActorState
    constexpr uintptr_t State_Health = 0x1D8;
    constexpr uintptr_t State_MaxHealth = 0x1DC;
    constexpr uintptr_t State_Armor = 0x1E0;
    constexpr uintptr_t State_MaxArmor = 0x1E4;
    constexpr uintptr_t State_AliveState = 0x210;

    // UCharacterState (extends UActorState)
    constexpr uintptr_t State_Stamina = 0x278;
    constexpr uintptr_t State_MaxStamina = 0x27C;

    // USurvivalCharacterState (extends UCharacterState)
    constexpr uintptr_t State_Oxygen = 0x328;
    constexpr uintptr_t State_Water = 0x32C;
    constexpr uintptr_t State_Food = 0x330;
    constexpr uintptr_t State_MaxOxygen = 0x338;
    constexpr uintptr_t State_MaxWater = 0x340;
    constexpr uintptr_t State_MaxFood = 0x348;

    // ACharacter -> CharacterMovement
    constexpr uintptr_t Char_MovementComp = 0x288;

    // UCharacterMovementComponent
    constexpr uintptr_t CMC_MaxWalkSpeed = 0x18C;
    constexpr uintptr_t CMC_MaxWalkSpeedCrouched = 0x190;
    constexpr uintptr_t CMC_MaxSwimSpeed = 0x194;
    constexpr uintptr_t CMC_MaxFlySpeed = 0x198;

    // AActor
    constexpr uintptr_t Actor_CustomTimeDilation = 0x98;

    // AIcarusPlayerCharacter
    constexpr uintptr_t Player_InventoryComp = 0x758;

    // UInventory
    constexpr uintptr_t Inv_Slots = 0xF0;  // FInventorySlotsFastArray

    // FInventorySlotsFastArray -> TArray<FInventorySlot>
    constexpr uintptr_t FastArray_Slots = 0x108;

    // FInventorySlot
    constexpr uintptr_t Slot_ItemData = 0x10;  // FItemData
    constexpr uintptr_t Slot_Size = 0x240;

    // FItemData
    constexpr uintptr_t Item_DynamicData = 0x30;  // TArray<FItemDynamicData>

    // FItemDynamicData
    constexpr uintptr_t Dyn_PropertyType = 0x00;  // EDynamicItemProperties (uint8)
    constexpr uintptr_t Dyn_Value = 0x04;          // int32
    constexpr uintptr_t Dyn_Size = 0x08;
    constexpr uint8_t DynProp_ItemableStack = 7;

    // UModifierStateComponent
    constexpr uintptr_t Mod_Lifetime = 0x108;
    constexpr uintptr_t Mod_Remaining = 0x110;
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

    bool IsReady() const { return m_actorState != nullptr; }

    // Fast god mode - called from dedicated thread at 1ms intervals
    void TickGodModefast();

    // For overlay status display
    int GetHealth() const;
    int GetMaxHealth() const;
    int GetStamina() const;
    int GetMaxStamina() const;
    int GetArmor() const;
    int GetMaxArmor() const;

    struct FuncPatch {
        uintptr_t addr = 0;
        uint8_t backup[4] = {};
        int patchSize = 3;
        bool patched = false;
        bool floatReturn = false;
    };

private:
    Trainer() = default;
    void FindPlayer();
    void RemoveDebuffs();
    void PatchSetHealth(bool enable);
    void PatchFunc(FuncPatch& p, bool enable, const char* name);
    void PatchRemoveItem(bool enable);
    void ZeroRecipeCosts();

    // RemoveItem write patch
    uintptr_t m_removeItemAddr = 0;
    uint8_t m_removeItemBackup[3] = {};
    bool m_removeItemPatched = false;
    bool m_recipesZeroed = false;

public:
    float m_originalWalkSpeed = 0.0f;
    float m_origSpeeds[5] = {};
private:

    void** m_gworldPtr = nullptr;
    void* m_character = nullptr;
    void* m_actorState = nullptr;
    int m_retryTimer = 0;
    FILE* m_con = nullptr;

    // SetHealth write instruction patch (6 bytes NOP)
    uintptr_t m_setHealthAddr = 0;
    uint8_t m_setHealthBackup[6] = {};
    bool m_setHealthPatched = false;

    // Free craft patches
    FuncPatch m_scaledInputCount;    // GetScaledRecipeInputCount -> return 0
    FuncPatch m_scaledResourceCount; // GetScaledRecipeResourceItemCount -> return 0
    FuncPatch m_costMultiplier;      // GetStatBasedResourceCostMultiplier -> return 0.0f
};
