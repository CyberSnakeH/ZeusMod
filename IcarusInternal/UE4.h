#pragma once
// ============================================================================
// UE4 Helper - GWorld finder and player character resolution
// ============================================================================

#include "SDK.h"
#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace UE4 {

// Pattern scan within current process (internal)
inline uintptr_t PatternScan(uintptr_t start, size_t size, const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask;
    const char* p = pattern;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') {
            bytes.push_back(0); mask.push_back(false);
            p++; if (*p == '?') p++;
        } else {
            char h[3] = {p[0], p[1], 0};
            bytes.push_back((uint8_t)strtoul(h, nullptr, 16));
            mask.push_back(true);
            p += 2;
        }
    }
    auto* data = reinterpret_cast<const uint8_t*>(start);
    for (size_t i = 0; i + bytes.size() <= size; i++) {
        bool found = true;
        for (size_t j = 0; j < bytes.size(); j++) {
            if (mask[j] && data[i + j] != bytes[j]) { found = false; break; }
        }
        if (found) return start + i;
    }
    return 0;
}

// Resolve RIP-relative address
inline uintptr_t ResolveRIP(uintptr_t instrAddr, int opcodeLen = 3, int instrLen = 7) {
    int32_t rel = *reinterpret_cast<int32_t*>(instrAddr + opcodeLen);
    return instrAddr + instrLen + rel;
}

// Get module base and size
inline void GetModuleInfo(uintptr_t& base, size_t& size) {
    HMODULE mod = GetModuleHandleW(nullptr);
    base = reinterpret_cast<uintptr_t>(mod);
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info));
    size = info.SizeOfImage;
}

// ============================================================================
// Find GWorld pointer (scans all candidates, picks valid one)
// ============================================================================
inline SDK::UWorld** FindGWorld() {
    uintptr_t base; size_t size;
    GetModuleInfo(base, size);

    const char* pattern = "48 8B 1D ?? ?? ?? ?? 48 85 DB 74";

    uintptr_t scanPos = base;
    size_t remain = size;

    while (remain > 64) {
        uintptr_t hit = PatternScan(scanPos, remain, pattern);
        if (!hit) break;

        uintptr_t gworldPtr = ResolveRIP(hit);
        auto** gworld = reinterpret_cast<SDK::UWorld**>(gworldPtr);

        __try {
            SDK::UWorld* world = *gworld;
            if (world && reinterpret_cast<uintptr_t>(world) > 0x10000) {
                void* gi = world->GameInstance;
                if (gi && reinterpret_cast<uintptr_t>(gi) > 0x10000) {
                    return gworld;
                }
            }
        } __except(1) {}

        scanPos = hit + 1;
        remain = (base + size) - scanPos;
    }
    return nullptr;
}

// ============================================================================
// Walk the UE4 hierarchy to find the local player character
// ============================================================================
inline SDK::AIcarusCharacter* GetLocalCharacter(SDK::UWorld** gworldPtr) {
    if (!gworldPtr) return nullptr;

    __try {
        SDK::UWorld* world = *gworldPtr;
        if (!world) return nullptr;

        auto* gi = reinterpret_cast<SDK::UGameInstance*>(world->GameInstance);
        if (!gi) return nullptr;

        if (!gi->LocalPlayers.IsValid()) return nullptr;

        auto* localPlayer = reinterpret_cast<SDK::UPlayer*>(gi->LocalPlayers[0]);
        if (!localPlayer) return nullptr;

        auto* controller = reinterpret_cast<SDK::AController*>(localPlayer->PlayerController);
        if (!controller) return nullptr;

        return controller->Character;
    } __except(1) {
        return nullptr;
    }
}

// ============================================================================
// Validate a character pointer
// ============================================================================
inline bool IsValidCharacter(SDK::AIcarusCharacter* ch) {
    if (!ch) return false;
    __try {
        auto* state = ch->ActorState;
        if (!state) return false;
        int32_t hp = state->Health;
        int32_t maxHp = state->MaxHealth;
        int32_t sta = state->Stamina;
        int32_t maxSta = state->MaxStamina;
        return (hp > 0 && maxHp >= 50 && maxHp <= 5000 && hp <= maxHp &&
                sta > 0 && maxSta >= 50 && maxSta <= 5000 && sta <= maxSta);
    } __except(1) {
        return false;
    }
}

} // namespace UE4
