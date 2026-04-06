#include "Trainer.h"
#include "UE4.h"

// All memory access goes through ReadAt/WriteAt with __try for safety

void Trainer::Initialize() {
    AllocConsole();
    SetConsoleTitleW(L"IcarusMod");
    freopen_s(&m_con, "CONOUT$", "w", stdout);
    printf("=== IcarusMod Internal ===\n\n");
    FindPlayer();
}

void Trainer::Shutdown() {
    PatchSetHealth(false);
    PatchRemoveItem(false);
    PatchCraftCosts(false);
    printf("[EXIT] Shutdown.\n");
    if (m_con) { fclose(m_con); FreeConsole(); }
}

void Trainer::FindPlayer() {
    // Find SetHealth and dump the actual bytes to see what we're patching
    if (!m_setHealthAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz,
            "79 04 33 ?? EB 09 41 8B ?? 41 3B ?? 0F 4C ?? 89 ?? D8 01 00 00");
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

    // Find GetScaledRecipeInputCount and GetScaledRecipeResourceItemCount
    // via fixed offset from module base (from x64dbg symbols)
    // Offset 0x18167A0 = GetScaledRecipeInputCount
    // Offset 0x1816820 = GetScaledRecipeResourceItemCount
    {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);

        // Verify by checking the prologue bytes
        uintptr_t candidate1 = b + 0x18167A0;
        uintptr_t candidate2 = b + 0x1816820;

        // GetScaledRecipeInputCount starts with: 48 89 5C 24 08 48 89 6C 24 10
        uint8_t* p1 = reinterpret_cast<uint8_t*>(candidate1);
        if (p1[0] == 0x48 && p1[1] == 0x89 && p1[2] == 0x5C && p1[3] == 0x24 && p1[4] == 0x08) {
            m_scaledInputAddr = candidate1;
            printf("[CRAFT] GetScaledRecipeInputCount at 0x%p (offset verified)\n", (void*)m_scaledInputAddr);
        } else {
            printf("[CRAFT] GetScaledRecipeInputCount offset mismatch! Bytes: %02X %02X %02X %02X %02X\n",
                p1[0], p1[1], p1[2], p1[3], p1[4]);
            // Fallback: AOB scan for the unique sequence after prologue
            // 49 8B E9 49 8B F8 8B DA 48 8B F1 E8
            uintptr_t match = UE4::PatternScan(b, sz, "49 8B E9 49 8B F8 8B DA 48 8B F1 E8");
            if (match) {
                // Function starts 20 bytes before this (prologue is 20 bytes)
                m_scaledInputAddr = match - 20;
                printf("[CRAFT] GetScaledRecipeInputCount at 0x%p (AOB fallback)\n", (void*)m_scaledInputAddr);
            } else {
                printf("[CRAFT] GetScaledRecipeInputCount NOT FOUND\n");
            }
        }

        // GetScaledRecipeResourceItemCount starts similarly
        uint8_t* p2 = reinterpret_cast<uint8_t*>(candidate2);
        if (p2[0] == 0x48 && p2[1] == 0x89 && p2[2] == 0x5C && p2[3] == 0x24) {
            m_scaledResourceAddr = candidate2;
            printf("[CRAFT] GetScaledRecipeResourceItemCount at 0x%p (offset verified)\n", (void*)m_scaledResourceAddr);
        } else {
            printf("[CRAFT] GetScaledRecipeResourceItemCount offset mismatch\n");
        }
    }

    // Find UInventory::ConsumeItem - CE confirmed AOB
    // cmp rdi,rcx; jne; sub [rsi+04],r12d; jmp
    // The sub [rsi+04],r12d (44 29 66 04) is what decrements item count
    if (!m_removeItemAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz,
            "48 3B F9 75 F2 44 29 66 04 E9");
        if (match) {
            m_removeItemAddr = match + 5; // points to "44 29 66 04"
            printf("[PATCH] ConsumeItem sub at 0x%p\n", (void*)m_removeItemAddr);
        } else {
            printf("[PATCH] ConsumeItem AOB not found\n");
        }
    }

    // Old scan removed - now using offset-based approach in FindPlayer

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

            // TArray<ULocalPlayer*> at gi+0x38: data pointer at +0, count at +8
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
        if (GodMode) {
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

        if (InfiniteStamina) {
            int maxSta = ReadAt<int>(m_actorState, Off::State_MaxStamina);
            WriteAt<int>(m_actorState, Off::State_Stamina, maxSta);
        }

        if (InfiniteArmor) {
            int maxArmor = ReadAt<int>(m_actorState, Off::State_MaxArmor);
            WriteAt<int>(m_actorState, Off::State_Armor, maxArmor);
        }

        if (InfiniteOxygen) {
            int maxO2 = ReadAt<int>(m_actorState, Off::State_MaxOxygen);
            WriteAt<int>(m_actorState, Off::State_Oxygen, maxO2);
        }

        if (InfiniteFood) {
            int maxFood = ReadAt<int>(m_actorState, Off::State_MaxFood);
            WriteAt<int>(m_actorState, Off::State_Food, maxFood);
        }

        if (InfiniteWater) {
            int maxWater = ReadAt<int>(m_actorState, Off::State_MaxWater);
            WriteAt<int>(m_actorState, Off::State_Water, maxWater);
        }

        // Speed hack via CustomTimeDilation on the player actor
        // This accelerates everything: movement, animations, actions
        if (SpeedHack) {
            WriteAt<float>(m_character, Off::Actor_CustomTimeDilation, SpeedMultiplier);
        } else {
            // Restore to normal (1.0)
            float current = ReadAt<float>(m_character, Off::Actor_CustomTimeDilation);
            if (current != 1.0f) {
                WriteAt<float>(m_character, Off::Actor_CustomTimeDilation, 1.0f);
            }
        }

        // Free craft
        if (FreeCraft) {
            PatchRemoveItem(true);   // Don't consume items
            PatchCraftCosts(true);   // Set all costs to 0 (X/0 display)
        } else {
            PatchRemoveItem(false);
            PatchCraftCosts(false);
        }
    }
    __except (1) {
        m_actorState = nullptr;
        m_character = nullptr;
    }
}

void Trainer::RemoveDebuffs() {
    if (!m_character) return;

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

void Trainer::PatchCraftCosts(bool enable) {
    // Patch both functions with: xor eax, eax; ret (return 0)
    // This makes all recipe costs = 0, displaying X/0 in the UI
    uint8_t retZero[3] = { 0x31, 0xC0, 0xC3 }; // xor eax,eax; ret
    PatchBytes(m_scaledInputAddr, retZero, m_scaledInputBackup, 3, enable, m_scaledInputPatched, "GetScaledRecipeInputCount");
    PatchBytes(m_scaledResourceAddr, retZero, m_scaledResourceBackup, 3, enable, m_scaledResourcePatched, "GetScaledRecipeResourceItemCount");
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

// Old ZeroDataTableCosts removed - replaced by PatchCraftCosts

void Trainer::TickGodModefast() {
    if (!m_actorState) return;
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
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Health); } __except(1) { return 0; }
}
int Trainer::GetMaxHealth() const {
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxHealth); } __except(1) { return 0; }
}
int Trainer::GetStamina() const {
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Stamina); } __except(1) { return 0; }
}
int Trainer::GetMaxStamina() const {
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxStamina); } __except(1) { return 0; }
}
int Trainer::GetArmor() const {
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_Armor); } __except(1) { return 0; }
}
int Trainer::GetMaxArmor() const {
    if (!m_actorState) return 0;
    __try { return ReadAt<int>(m_actorState, Off::State_MaxArmor); } __except(1) { return 0; }
}
