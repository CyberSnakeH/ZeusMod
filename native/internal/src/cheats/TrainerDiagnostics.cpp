#include "Trainer.h"
#include "UE4.h"
#include "UObjectLookup.h"
#include "Logger.h"
#include "TrainerInternal.h"
#include <cstdio>
#include <cstring>
#include <string>

// ============================================================================
// UE reflection / introspection helpers used for offset discovery, property
// walks, live-instance enumeration and hex dumps. Most of this is cold-path
// code invoked from Trainer::Initialize and Trainer::RunOnceDiagnostics.
// Helpers only referenced within this file keep their `static` keyword.
// ============================================================================

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
void* SafeReadPtrAt(const void* base, uintptr_t off) {
    __try { return *(void**)((const unsigned char*)base + off); }
    __except (1) { return nullptr; }
}

// Hex dump of the first `bytes` bytes of a UObject, for layout discovery.
// Helps locate valid-looking pointer candidates (0x000001e0xxxxxxxx heap
// range) when a known-good field offset isn't yet discovered.
void HexDump(const char* label, void* obj, size_t bytes) {
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
void EnumerateLiveInstancesWithProbe(const char* className,
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
void* FindCharacterComponentByClass(void* character, const char* className) {
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
void* FindFirstLiveInstance(const char* className) {
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
void* FindFirstInstanceWithNonNullProbe(const char* className,
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
void DumpMetaResourceArray(const char* label, uintptr_t arrayAddr) {
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
void DumpClassProperties(const char* className) {
    if (!UObjectLookup::IsInitialized()) return;
    // Use the broad resolver so we can walk USTRUCT layouts too (ItemData etc.).
    uintptr_t cls = UObjectLookup::FindStructByName(className);
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
void DiscoverTickSubsystem() {
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
