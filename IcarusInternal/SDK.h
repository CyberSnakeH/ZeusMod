#pragma once
// ============================================================================
// IcarusMod SDK - Minimal structs from UE4SS CXXHeaderDump
// All offsets verified against Icarus April 2026 build
// ============================================================================

#include <cstdint>

namespace SDK {

// ============================================================================
// UE4 Core Types
// ============================================================================

template<typename T>
struct TArray {
    T* Data;
    int32_t Count;
    int32_t Max;

    T& operator[](int32_t i) { return Data[i]; }
    const T& operator[](int32_t i) const { return Data[i]; }
    bool IsValid() const { return Data != nullptr && Count > 0; }
};

// ============================================================================
// UObject base (Size: 0x28)
// ============================================================================
struct UObject {
    void** VTable;           // 0x00
    char pad_0008[0x20];     // flags, index, class, name, outer
};

// ============================================================================
// AActor (partial, Size: 0x228+)
// Offsets from Engine.hpp dump
// ============================================================================
struct UActorComponent;

struct AActor : UObject {
    char pad_0028[0x1C8];                          // 0x28 -> 0x1F0
    TArray<UActorComponent*> InstanceComponents;   // 0x1F0
    TArray<UActorComponent*> BlueprintCreatedComponents; // 0x200
};

// ============================================================================
// UActorComponent base (Size: 0xB0+)
// ============================================================================
struct UActorComponent : UObject {
    char pad_0028[0x88]; // up to 0xB0
};

// ============================================================================
// UModifierStateComponent (Size: 0x3B8)
// From Icarus.hpp line 24405
// ============================================================================
struct UModifierStateComponent : UActorComponent {
    char pad_00B0[0x04];                    // 0xB0 -> 0xB4
    char DataRowHandleNew[0x18];            // 0xB4 (FModifierStatesRowHandle)
    char pad_00CC[0x04];                    // padding
    void* Instigator;                       // 0xD0 (AController*)
    void* Causer;                           // 0xD8 (AActor*)
    void* ReplicatedOwner;                  // 0xE0 (AActor*)
    char OwningModifiableInterface[0x10];   // 0xE8 (TScriptInterface)
    int32_t ModifierUID;                    // 0xF8
    char pad_00FC[0x0C];                    // 0xFC -> 0x108
    float ModifierLifetime;                 // 0x108
    float CachedLifetimeModifier;           // 0x10C
    float RemainingTime;                    // 0x110
    int32_t InitialEffectiveness;           // 0x114
    int32_t CachedFinalEffectiveness;       // 0x118
};

// ============================================================================
// UActorState (Size: 0x270)
// From Icarus.hpp line 13147
// Health/Armor/Temperature system
// ============================================================================
struct UActorState : UActorComponent {
    // 0xB0 -> 0x1D8: LastDamagePacket, RecentDamagePackets, delegates
    char pad_00B0[0x128];

    int32_t Health;              // 0x1D8
    int32_t MaxHealth;           // 0x1DC
    int32_t Armor;               // 0x1E0
    int32_t MaxArmor;            // 0x1E4
    float Shelter;               // 0x1E8
    int32_t ExternalTemperature; // 0x1EC
    int32_t ModifiedExtTemp;     // 0x1F0
    int32_t ModifiedIntTemp;     // 0x1F4
    char CurrentBiome[0x18];     // 0x1F8 (FBiomesRowHandle)
    uint8_t CurrentAliveState;   // 0x210 (0 = Alive)
    char pad_0211[0x5F];         // 0x211 -> 0x270
};

// ============================================================================
// UCharacterState (Size: 0x320, extends UActorState)
// From Icarus.hpp line 15287
// Adds Stamina/XP/Level
// ============================================================================
struct UCharacterState : UActorState {
    // 0x270 -> 0x278: OnStaminaUpdated delegates etc
    char pad_0270[0x08];

    int32_t Stamina;             // 0x278
    int32_t MaxStamina;          // 0x27C
    int32_t TotalExperience;     // 0x280
    char GrowthRowHandle[0x18];  // 0x284 (FCharacterGrowthRowHandle)
    int32_t Level;               // 0x29C
    char pad_02A0[0x80];         // 0x2A0 -> 0x320
};

// ============================================================================
// AIcarusCharacter (Size: 0x750)
// From Icarus.hpp line 9710
// ============================================================================
struct AIcarusCharacter : AActor {
    // 0x210 -> 0x5A0: lots of inherited fields from ACharacter/APawn
    char pad_0210[0x390];

    void* StatContainer;              // 0x5A0 (UIcarusStatContainer*)
    UCharacterState* ActorState;      // 0x5A8
    char pad_05B0[0x1A0];             // rest up to 0x750
};

// ============================================================================
// UE4 World Hierarchy
// ============================================================================

// UWorld (partial)
struct UWorld : UObject {
    char pad_0028[0x0D00];       // 0x28 -> 0xD28
    void* GameInstance;          // 0x0D28 (UGameInstance*)
};

// UGameInstance (partial)
struct UGameInstance : UObject {
    char pad_0028[0x10];         // 0x28 -> 0x38
    TArray<void*> LocalPlayers;  // 0x38 (TArray<ULocalPlayer*>)
};

// UPlayer (partial)
struct UPlayer : UObject {
    char pad_0028[0x08];         // 0x28 -> 0x30
    void* PlayerController;      // 0x30 (APlayerController*)
};

// AController (partial)
struct AController : AActor {
    char pad_0210[0x40];         // 0x210 -> 0x250
    void* Pawn;                  // 0x250 (APawn*)
    char pad_0258[0x08];         // 0x258 -> 0x260
    AIcarusCharacter* Character; // 0x260
};

} // namespace SDK
