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

    // Find UInventory::RemoveItem - CE confirmed AOB
    // jle ??; sub ecx,r15d; mov [rax+04],ecx; mov rcx,[rsp+60]
    if (!m_removeItemAddr) {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);
        uintptr_t match = UE4::PatternScan(b, sz,
            "7E ?? 41 2B ?? 89 48 04 48 8B 4C 24 60");
        if (match) {
            m_removeItemAddr = match + 5; // points to "89 48 04"
            printf("[PATCH] RemoveItem write at 0x%p\n", (void*)m_removeItemAddr);
        } else {
            printf("[PATCH] RemoveItem AOB not found\n");
        }
    }

    // Find GetScaledRecipeInputCount and GetScaledRecipeResourceItemCount
    // These functions convert int to float (cvtsi2ss), multiply (mulss), convert back (cvttss2si)
    // Patching them to return 0 makes all recipe costs = 0
    {
        uintptr_t b; size_t sz;
        UE4::GetModuleInfo(b, sz);

        auto findFuncStart = [](uintptr_t ref) -> uintptr_t {
            for (int back = 3; back < 128; back++) {
                uint8_t* bp = reinterpret_cast<uint8_t*>(ref - back);
                if (*bp == 0xCC && *(bp + 1) != 0xCC) return ref - back + 1;
            }
            return 0;
        };

        uintptr_t scan = b;
        size_t remain = sz;
        int found = 0;

        while (remain > 64 && found < 2) {
            // cvtsi2ss = F3 0F 2A (int->float, used for scaling recipe counts)
            uintptr_t match = UE4::PatternScan(scan, remain, "F3 0F 2A");
            if (!match) break;

            uint8_t* p = reinterpret_cast<uint8_t*>(match);
            bool hasMul = false, hasConvert = false;
            for (int i = 3; i < 50; i++) {
                if (p[i]==0xF3 && p[i+1]==0x0F && p[i+2]==0x59) hasMul = true;     // mulss
                if (p[i]==0xF3 && p[i+1]==0x0F && p[i+2]==0x2C) hasConvert = true;  // cvttss2si
            }

            if (hasMul && hasConvert) {
                uintptr_t func = findFuncStart(match);
                if (func) {
                    if (found == 0) {
                        m_scaledInputCount.addr = func;
                        printf("[CRAFT] ScaledInputCount at 0x%p\n", (void*)func);
                    } else if (func != m_scaledInputCount.addr) {
                        m_scaledResourceCount.addr = func;
                        printf("[CRAFT] ScaledResourceCount at 0x%p\n", (void*)func);
                    }
                    found++;
                }
            }

            scan = match + 1;
            remain = (b + sz) - scan;
        }
        if (found == 0) printf("[CRAFT] No crafting scale functions found\n");
    }

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
            PatchRemoveItem(true);
            if (!m_recipesZeroed) ZeroRecipeCosts();
        } else {
            PatchRemoveItem(false);
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

void Trainer::PatchFunc(FuncPatch& p, bool enable, const char* name) {
    if (!p.addr) return;
    int sz = p.floatReturn ? 4 : 3;
    p.patchSize = sz;

    if (enable && !p.patched) {
        memcpy(p.backup, reinterpret_cast<void*>(p.addr), sz);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(p.addr), sz, PAGE_EXECUTE_READWRITE, &oldP);
        if (p.floatReturn) {
            uint8_t patch[4] = { 0x0F, 0x57, 0xC0, 0xC3 }; // xorps xmm0,xmm0; ret (return 0.0f)
            memcpy(reinterpret_cast<void*>(p.addr), patch, 4);
        } else {
            uint8_t patch[3] = { 0xB0, 0x01, 0xC3 }; // mov al, 1; ret (return true)
            memcpy(reinterpret_cast<void*>(p.addr), patch, 3);
        }
        VirtualProtect(reinterpret_cast<void*>(p.addr), sz, oldP, &oldP);
        p.patched = true;
        printf("[PATCH] %s -> return %s\n", name, p.floatReturn ? "0.0f" : "true");
    } else if (!enable && p.patched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(p.addr), sz, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(p.addr), p.backup, sz);
        VirtualProtect(reinterpret_cast<void*>(p.addr), sz, oldP, &oldP);
        p.patched = false;
        printf("[PATCH] %s restored\n", name);
    }
}

void Trainer::PatchRemoveItem(bool enable) {
    if (!m_removeItemAddr) return;
    if (enable && !m_removeItemPatched) {
        memcpy(m_removeItemBackup, reinterpret_cast<void*>(m_removeItemAddr), 3);
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 3, PAGE_EXECUTE_READWRITE, &oldP);
        memset(reinterpret_cast<void*>(m_removeItemAddr), 0x90, 3); // NOP
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 3, oldP, &oldP);
        m_removeItemPatched = true;
        printf("[PATCH] RemoveItem write NOPed (free craft ON)\n");
    } else if (!enable && m_removeItemPatched) {
        DWORD oldP;
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 3, PAGE_EXECUTE_READWRITE, &oldP);
        memcpy(reinterpret_cast<void*>(m_removeItemAddr), m_removeItemBackup, 3);
        VirtualProtect(reinterpret_cast<void*>(m_removeItemAddr), 3, oldP, &oldP);
        m_removeItemPatched = false;
        printf("[PATCH] RemoveItem restored (free craft OFF)\n");
    }
}

void Trainer::ZeroRecipeCosts() {
    // Safe heap scan for FCraftingInput arrays
    // FCraftingInput = { FItemsStaticRowHandle(0x18), int32 Count } = 0x1C bytes
    // We require 3+ consecutive entries with valid FName + Count to reduce false positives
    // Only scan PAGE_READWRITE regions (skip GPU/DX12 memory)

    printf("[CRAFT] Safe scanning for recipe costs...\n");

    uintptr_t moduleBase; size_t moduleSize;
    UE4::GetModuleInfo(moduleBase, moduleSize);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    int totalZeroed = 0;
    int regionsScanned = 0;

    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t regionEnd = regionBase + mbi.RegionSize;

        // STRICT filter: only pure heap memory
        bool isSafe = (mbi.State == MEM_COMMIT) &&
                      (mbi.Protect == PAGE_READWRITE) &&   // EXACTLY PAGE_READWRITE, nothing else
                      (mbi.Type == MEM_PRIVATE) &&          // Private heap, not mapped file
                      (mbi.RegionSize >= 0x1000) &&         // At least 4KB
                      (mbi.RegionSize <= 0x4000000);        // Max 64MB (skip huge GPU buffers)

        // Skip game module memory
        if (regionBase >= moduleBase && regionBase < moduleBase + moduleSize) isSafe = false;

        if (isSafe) {
            regionsScanned++;
            __try {
                uint8_t* mem = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;

                for (size_t i = 0; i + 0x1C * 3 < regionSize; i += 4) {
                    // Require 3 consecutive valid FCraftingInputs
                    bool valid = true;
                    for (int e = 0; e < 3 && valid; e++) {
                        int32_t name = *reinterpret_cast<int32_t*>(mem + i + e * 0x1C);
                        int32_t nameNum = *reinterpret_cast<int32_t*>(mem + i + e * 0x1C + 4);
                        int32_t count = *reinterpret_cast<int32_t*>(mem + i + e * 0x1C + 0x18);

                        // FName: ComparisonIndex > 0, Number == 0 (most FNames)
                        // Count: 1-200 (reasonable recipe cost)
                        if (name <= 0 || name > 100000) valid = false;
                        if (nameNum != 0) valid = false;  // FName::Number should be 0
                        if (count < 1 || count > 200) valid = false;
                    }

                    if (valid) {
                        // Also check that all 3 have different FNames (different items)
                        int32_t n1 = *reinterpret_cast<int32_t*>(mem + i);
                        int32_t n2 = *reinterpret_cast<int32_t*>(mem + i + 0x1C);
                        int32_t n3 = *reinterpret_cast<int32_t*>(mem + i + 0x1C * 2);
                        if (n1 == n2 || n2 == n3 || n1 == n3) { continue; }

                        // Zero all counts in this array
                        for (int e = 0; e < 10; e++) {
                            int32_t* countPtr = reinterpret_cast<int32_t*>(mem + i + e * 0x1C + 0x18);
                            int32_t* namePtr = reinterpret_cast<int32_t*>(mem + i + e * 0x1C);
                            int32_t* numPtr = reinterpret_cast<int32_t*>(mem + i + e * 0x1C + 4);
                            if (*countPtr >= 1 && *countPtr <= 200 && *namePtr > 0 && *namePtr < 100000 && *numPtr == 0) {
                                *countPtr = 0;
                                totalZeroed++;
                            } else {
                                break;
                            }
                        }
                        i += 10 * 0x1C;
                    }
                }
            }
            __except (1) {}
        }
        addr = regionEnd;
    }

    printf("[CRAFT] Scanned %d regions. Zeroed %d recipe costs!\n", regionsScanned, totalZeroed);
    m_recipesZeroed = (totalZeroed > 0);
}

/* old code removed
    // UE4 DataTable internal layout:
    // UDataTable inherits UObject (0x28) + RowStruct (0x28)
    // The RowMap is a TMap<FName, uint8*> at offset ~0x30
    // But the exact offset varies. We use a different approach:
    //
    // FProcessorRecipe::Inputs is a TArray<FCraftingInput> at offset 0x90 in each recipe
    // FCraftingInput::Count is at offset 0x18, size 0x1C per entry
    //
    // Strategy: Find GObjects array, iterate to find UDataTable named "D_ProcessorRecipes"
    // then access its RowMap
    //
    // Simpler strategy: Since we know GObjects from our earlier scan,
    // we scan the game's heap memory for FProcessorRecipe structures
    // by looking for the pattern: TArray at known layout positions

    // Actually, the simplest approach: scan all readable memory for
    // FCraftingInput arrays with known Count values (10, 4, 6 = Stone Pickaxe)
    // and zero them

    printf("[CRAFT] Scanning for recipe data in memory...\n");

    uintptr_t base; size_t size;
    UE4::GetModuleInfo(base, size);

    // The DataTable rows are allocated on the heap, not in the module
    // We need to scan process memory regions
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    int totalZeroed = 0;

    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            mbi.RegionSize > 0x100 && mbi.RegionSize < 0x10000000) {

            __try {
                uint8_t* mem = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
                size_t regionSize = mbi.RegionSize;

                // FProcessorRecipe has Inputs TArray at +0x90
                // TArray = {Data*, Count, Max}
                // We look for TArrays where each element is 0x1C bytes
                // and has Count field (int32) at +0x18 with values > 0

                // Scan for FCraftingInput patterns: a valid FName (RowName)
                // followed by a DataTableName, then Count > 0
                // FCraftingInput is 0x1C bytes: FItemsStaticRowHandle(0x18) + int32 Count

                for (size_t i = 0; i + 0x1C * 3 < regionSize; i += 4) {
                    // Check if this looks like a FCraftingInput with Count > 0
                    // followed by another FCraftingInput with Count > 0
                    int32_t count1 = *reinterpret_cast<int32_t*>(mem + i + 0x18);
                    int32_t count2 = *reinterpret_cast<int32_t*>(mem + i + 0x18 + 0x1C);

                    // Valid counts: 1-1000, and both should be reasonable
                    if (count1 >= 1 && count1 <= 500 && count2 >= 1 && count2 <= 500) {
                        // Check the FName indices are reasonable (positive, < 100000)
                        int32_t name1 = *reinterpret_cast<int32_t*>(mem + i);
                        int32_t name2 = *reinterpret_cast<int32_t*>(mem + i + 0x1C);

                        if (name1 > 0 && name1 < 200000 && name2 > 0 && name2 < 200000 && name1 != name2) {
                            // This looks like 2 consecutive FCraftingInputs
                            // Zero all counts in this array (up to 10 entries)
                            for (int e = 0; e < 10; e++) {
                                int32_t* countPtr = reinterpret_cast<int32_t*>(mem + i + e * 0x1C + 0x18);
                                if (*countPtr >= 1 && *countPtr <= 500) {
                                    *countPtr = 0;
                                    totalZeroed++;
                                } else {
                                    break; // End of array
                                }
                            }
                            i += 10 * 0x1C; // Skip past this array
                        }
                    }
                }
            }
            __except (1) {}
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    printf("[CRAFT] Zeroed %d recipe input counts!\n", totalZeroed);
*/

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
