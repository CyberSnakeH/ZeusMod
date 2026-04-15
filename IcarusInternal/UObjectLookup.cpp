// ============================================================================
// UObjectLookup — minimal UE4.27 UFunction-by-name resolver for IcarusMod.
// See header for rationale.
// ============================================================================

#include "UObjectLookup.h"
#include "UE4.h"
#include "libs/minhook/src/hde/hde64.h"
#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>

extern "C" unsigned int hde64_disasm(const void* code, hde64s* hs);

#pragma comment(lib, "psapi.lib")

namespace UObjectLookup {

// === UE 4.27 hardcoded constants ===========================================

// FUObjectArray (chunked) layout — Default UE4.21+ layout
// struct FChunkedFixedUObjectArray {
//   FUObjectItem** Objects;     // +0x00 — pointer to a chunk pointer table
//   FUObjectItem*  PreAllocated; // +0x08 — unused for chunked
//   int32          MaxElements;  // +0x10
//   int32          NumElements;  // +0x14
//   int32          MaxChunks;    // +0x18
//   int32          NumChunks;    // +0x1C
// };
constexpr int OFF_OBJ_ARRAY_OBJECTS  = 0x00;
constexpr int OFF_OBJ_ARRAY_NUMELEMS = 0x14;

// FUObjectItem (UE4): 24 bytes
//   UObject* Object;        +0x00
//   int32    Flags;         +0x08
//   int32    ClusterRoot;   +0x0C
//   int32    SerialNumber;  +0x10
//   int32    pad;           +0x14
constexpr int FUOBJECT_ITEM_SIZE = 24;
constexpr int OBJECTS_PER_CHUNK  = 64 * 1024;

// UObject layout (stable across UE 4.18 — UE 5.4)
constexpr int OFF_UOBJECT_INDEX = 0x0C;
constexpr int OFF_UOBJECT_CLASS = 0x10;
constexpr int OFF_UOBJECT_NAME  = 0x18;
constexpr int OFF_UOBJECT_OUTER = 0x20;

// UStruct: Children (UField chain) at +0x48 — contains UFunctions
constexpr int OFF_USTRUCT_CHILDREN = 0x48;

// UField: Next (next link in the Children chain). Position varies; for
// UE 4.25-4.27 with Outer=0x20, Next is at 0x28 (right after the UObject
// fields end).
constexpr int OFF_UFIELD_NEXT = 0x28;

// UFunction layout (UE 4.25 — 4.27):
//   FunctionFlags  +0xB0  uint32
//   NumParms       +0xB4  uint8
//   ParmsSize      +0xB6  uint16
//   ReturnValueOff +0xB8  uint16
//   RPCId          +0xBA  uint16
//   RPCResponseId  +0xBC  uint16
//   FirstPropToInit+0xC0  FProperty*
//   EventGraphFn   +0xC8  UFunction*
//   EventGraphCall +0xD0  int32
//   Func           +0xD8  Native (C++ function pointer)  <-- our target
constexpr int OFF_UFUNCTION_FUNC = 0xD8;

// FNamePool layout — varies by UE build. We auto-detect chunks offset and
// header format at init time by validating that entry[0] decodes to "None".
//
// Common layouts:
//   UE 4.27 standard:    ChunksOffset=0x10, lenShift=6, stride=2
//   UE 4.27 alt builds:  ChunksOffset=0x30 / 0x40
//   UE 5.0+ standard:    ChunksOffset=0x10, lenShift=6
//
// Each FName index decomposes as:
//   block  = index >> 16
//   offset = (index & 0xFFFF) * stride
constexpr int FNAME_BLOCK_OFFSET_BITS = 16;

// Detected at init time. Defaults are most common UE 4.27 values.
static int s_chunksOffset = 0x10;
static int s_lenShift     = 6;
static int s_lenMask      = 0x3FF;
static int s_stride       = 2;
// Some UE 4.26+ "hashed" pools store a 4-byte hash before the header.
// 0 = standard layout, 4 = hashed layout.
static int s_headerOffset = 0;

// Helpers ====================================================================

static uintptr_t s_moduleBase = 0;
static size_t    s_moduleSize = 0;

static uintptr_t s_gobjectsAddr = 0; // &FUObjectArray
static uintptr_t s_gnamesAddr   = 0; // &FNamePool

static bool s_initialized = false;

template<typename T>
static bool SafeRead(uintptr_t addr, T& out) {
    if (!addr) return false;
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadBytes(uintptr_t addr, void* dst, size_t n) {
    if (!addr) return false;
    __try {
        memcpy(dst, reinterpret_cast<void*>(addr), n);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsCodePtr(uintptr_t p) {
    return s_moduleBase && p >= s_moduleBase && p < s_moduleBase + s_moduleSize;
}

static bool IsHeapPtr(uintptr_t p) {
    if (!p || p < 0x10000) return false;
    if (p > 0x00007FFFFFFFFFFFULL) return false;
    return !IsCodePtr(p);
}

// === GObjects discovery ====================================================

// Try the most common UE4.27 GObjects AOBs and validate by reading
// NumElements (must be a sane positive value).
static uintptr_t ScanGObjects() {
    struct Pat { const char* aob; int opcodeLen; int instrLen; };
    static const Pat patterns[] = {
        // mov rax, [rip+disp32]; mov rcx, [rax+rcx*8]   -- the classic one
        { "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8",                3, 7 },
        // mov rax, [rip+disp32]; mov rcx, [rax+rcx*8]; lea rax, [rcx+rdx]
        { "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1",    3, 7 },
        // UE4.27 G427_1: mov rax, [GUObjectArray.Objects]
        { "48 8B 05 ?? ?? ?? ?? C1 F9 ?? 48 63 C9 48 8B",    3, 7 },
    };

    for (const auto& p : patterns) {
        uintptr_t hit = UE4::PatternScan(s_moduleBase, s_moduleSize, p.aob);
        if (!hit) continue;
        uintptr_t resolved = UE4::ResolveRIP(hit, p.opcodeLen, p.instrLen);
        if (!resolved) continue;

        // Validate: read NumElements at +0x14, must be positive and sane
        int32_t numElems = 0;
        if (!SafeRead(resolved + OFF_OBJ_ARRAY_NUMELEMS, numElems)) continue;
        if (numElems <= 0 || numElems > 16'000'000) continue;

        // Also validate the Objects pointer is a heap address
        uintptr_t objsPtr = 0;
        if (!SafeRead(resolved + OFF_OBJ_ARRAY_OBJECTS, objsPtr)) continue;
        if (!IsHeapPtr(objsPtr)) continue;

        printf("[UObjLookup] GObjects @ 0x%p (NumElements=%d) via AOB hit 0x%p\n",
            (void*)resolved, numElems, (void*)hit);
        return resolved;
    }
    return 0;
}

// === FNamePool discovery via memory landmark scan ==========================
//
// Strategy: find the "None" FNameEntry directly in heap memory by signature,
// then locate the FNamePool struct that contains a pointer to it. This is
// AOB-free for the global and works regardless of which UE version, because
// "None" is always the first FName entry by UE convention.
//
// FNameEntry signature for "None" (length=4, ANSI):
//   shift=6 build: header = (4<<6)|0 = 0x0100  -> "00 01 4E 6F 6E 65"
//   shift=1 build: header = (4<<1)|0 = 0x0008  -> "08 00 4E 6F 6E 65"
//   shift=0 build: header = 4                  -> "04 00 4E 6F 6E 65"

// Scan heap for "ByteProperty" string (12 chars, very distinctive UE name).
// Validate by checking that "IntProperty" or "Int8Property" appears within
// the next 256 bytes — both names are guaranteed to be in FNamePool block 0
// near "ByteProperty", regardless of header format / hash prefix / padding.
//
// This handles all known UE 4.x layouts including hashed entries.
//
// Returns the address WHERE "ByteProperty" string starts (not the entry start).
// The header before this address tells us shift / hash prefix size.
static uintptr_t ScanHeapForByteProperty() {
    // No region size limit: scan everything that's readable private memory.
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    int regionsScanned = 0;
    int candidatesFound = 0;
    size_t bytesScanned = 0;
    DWORD startTick = GetTickCount();

    while (addr < 0x00007FFFFFFF0000ULL) {
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
            addr += 0x1000;
            continue;
        }

        bool readable = (mbi.State == MEM_COMMIT) &&
                        (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) &&
                        ((mbi.Protect & PAGE_GUARD) == 0) &&
                        (mbi.Type == MEM_PRIVATE);

        bool inModule = (uintptr_t)mbi.BaseAddress >= s_moduleBase &&
                        (uintptr_t)mbi.BaseAddress < s_moduleBase + s_moduleSize;

        if (readable && !inModule && mbi.RegionSize >= 0x1000) {
            regionsScanned++;
            const uint8_t* base = reinterpret_cast<const uint8_t*>(mbi.BaseAddress);
            size_t sz = mbi.RegionSize;
            bytesScanned += sz;

            __try {
                const uint8_t* end = base + sz - 12;
                const uint8_t* p = base;
                while (p < end) {
                    // Find next 'B' candidate
                    p = static_cast<const uint8_t*>(memchr(p, 0x42, end - p));
                    if (!p) break;

                    if (memcmp(p, "ByteProperty", 12) != 0) {
                        p++;
                        continue;
                    }
                    candidatesFound++;

                    // Validate: scan next 512 bytes for "IntProperty" or
                    // "Int8Property". Both must be present in the same
                    // FNamePool block as ByteProperty.
                    size_t windowSize = 512;
                    if (p + 12 + windowSize > end + 12) windowSize = (end + 12) - (p + 12);

                    bool foundInt = false;
                    bool foundInt8 = false;
                    const uint8_t* w = p + 12;
                    const uint8_t* wend = w + windowSize;
                    while (w + 11 < wend) {
                        const uint8_t* m = static_cast<const uint8_t*>(memchr(w, 0x49, wend - w)); // 'I'
                        if (!m) break;
                        if (m + 12 < wend && memcmp(m, "Int8Property", 12) == 0) foundInt8 = true;
                        else if (m + 11 < wend && memcmp(m, "IntProperty", 11) == 0) foundInt = true;
                        if (foundInt && foundInt8) break;
                        w = m + 1;
                    }

                    if (foundInt || foundInt8) {
                        DWORD ms = GetTickCount() - startTick;
                        printf("[UObjLookup] ByteProperty FNamePool block validated at 0x%p (foundInt=%d foundInt8=%d) after %d regions, %zu MB, %ums\n",
                            (const void*)p, (int)foundInt, (int)foundInt8,
                            regionsScanned, bytesScanned / (1024 * 1024), ms);
                        return reinterpret_cast<uintptr_t>(p);
                    }
                    p++;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}

            if ((regionsScanned % 1000) == 0) {
                printf("[UObjLookup]   ...scanned %d regions, %zu MB, %d ByteProperty candidates, %u ms\n",
                    regionsScanned, bytesScanned / (1024 * 1024), candidatesFound,
                    GetTickCount() - startTick);
            }
        }

        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    DWORD ms = GetTickCount() - startTick;
    printf("[UObjLookup] Heap scan done: %d regions, %zu MB, %d ByteProperty candidates, %ums — NO valid FNamePool block found\n",
        regionsScanned, bytesScanned / (1024 * 1024), candidatesFound, ms);
    return 0;
}

// Detect FNamePool format by searching backwards from the BP string for
// any byte position where the length 12 (or 13 with null) is encoded in some
// way that we recognize. Tests headers from -1 to -8 bytes before the string,
// with multiple shifts and sizes. Also dumps surrounding bytes for diagnosis.
static bool DetectFNamePoolFormatFromBP(uintptr_t bpAddr) {
    // Dump 32 bytes BEFORE and 32 bytes AFTER the BP string for diagnosis
    {
        uint8_t buf[64] = {};
        if (SafeReadBytes(bpAddr - 32, buf, 64)) {
            printf("[UObjLookup] BP context (-32..+32, BP at offset 32):\n[UObjLookup]   ");
            for (int i = 0; i < 64; ++i) {
                if (i == 32) printf("|");
                printf("%02X ", buf[i]);
                if ((i + 1) % 16 == 0 && i != 63) printf("\n[UObjLookup]   ");
            }
            printf("\n[UObjLookup]   ASCII: '");
            for (int i = 0; i < 64; ++i) {
                char c = static_cast<char>(buf[i]);
                printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("'\n");
        }
    }

    // Try every byte offset from -1 to -8, with several encodings, looking
    // for one that produces length=12 (the length of "ByteProperty").
    // Encodings tried at each offset:
    //   - 1-byte length: bytes[off] == 12
    //   - 2-byte u16 length: bytes[off..off+1] decoded as u16 == 12
    //   - 2-byte header with shift 6: ((u16 >> 6) & 0x3FF) == 12  → header == 0x300 (3,0)
    //   - 2-byte header with shift 1: ((u16 >> 1) & 0x7FFF) == 12 → header == 0x18 (24,0)
    //   - 4-byte length: u32 == 12
    static const int candHdrSizes[] = { 1, 2, 4 };
    static const struct { int shift; int mask; } shifts[] = {
        { 0, 0xFFFF },
        { 1, 0x7FFF },
        { 6, 0x3FF  },
    };

    for (int hdrSize : candHdrSizes) {
        for (int dist = hdrSize; dist <= 16; dist++) {
            // Header would start at bpAddr - dist, end at bpAddr - dist + hdrSize
            uintptr_t hdrAddr = bpAddr - dist;
            if (hdrSize == 1) {
                uint8_t v = 0;
                if (!SafeRead(hdrAddr, v)) continue;
                if (v == 12) {
                    s_headerOffset = dist - hdrSize; // bytes between header end and string start
                    s_lenShift = 0;
                    s_lenMask  = 0xFF;
                    printf("[UObjLookup] Format: 1-byte length=12 at BP-%d (hdr→string gap=%d)\n",
                        dist, s_headerOffset);
                    return true;
                }
            } else if (hdrSize == 2) {
                uint16_t v = 0;
                if (!SafeRead(hdrAddr, v)) continue;
                for (const auto& sh : shifts) {
                    int len = (v >> sh.shift) & sh.mask;
                    if (len == 12) {
                        s_headerOffset = dist - hdrSize;
                        s_lenShift = sh.shift;
                        s_lenMask  = sh.mask;
                        printf("[UObjLookup] Format: 2-byte hdr=0x%04X (shift=%d len=12) at BP-%d (gap=%d)\n",
                            v, sh.shift, dist, s_headerOffset);
                        return true;
                    }
                }
            } else { // 4 bytes
                uint32_t v = 0;
                if (!SafeRead(hdrAddr, v)) continue;
                if ((v & 0xFFFF) == 12 || (v >> 16) == 12) {
                    s_headerOffset = dist - hdrSize;
                    s_lenShift = 0;
                    s_lenMask  = 0xFFFF;
                    printf("[UObjLookup] Format: 4-byte hdr=0x%08X contains 12 at BP-%d (gap=%d)\n",
                        v, dist, s_headerOffset);
                    return true;
                }
            }
        }
    }

    printf("[UObjLookup] Could not detect FNamePool format from BP context\n");
    return false;
}

// === REAL APPROACH: find FNamePool.Blocks[] array directly in module ===
//
// FNamePool's Blocks array has a very recognizable shape in static memory:
//   - 8192 consecutive uintptr_t slots (64 KB total)
//   - First N slots (N typically 1-50 in shipping builds) are HEAP pointers
//     to allocated blocks
//   - Remaining slots are all NULL (zero-initialized BSS)
//
// We scan the module's non-executable sections for any 8-byte-aligned
// position where: (a) the first slot is a heap pointer, (b) the next several
// slots are either heap pointers or null, (c) reading the first block
// produces an FName entry that looks like "None" within the first 32 bytes.
//
// This is the same technique reverse engineers use in IDA: identify the
// global table by its memory layout, not by its content.

// Strict validation: a real FNamePool block 0 must look exactly like an
// FNameEntry for "None" at offset 0 (or 0+hashSize). The header byte must
// decode to length=4 with one of the known shifts.
static bool IsValidFNamePoolBlock(uintptr_t blockAddr, int* outNoneOffset) {
    *outNoneOffset = -1;
    uint8_t buf[64] = {};
    if (!SafeReadBytes(blockAddr, buf, 64)) return false;

    // Try standard layout: header at offset 0, "None" at offset 2.
    // Try hash-prefixed: header at offset 4, "None" at offset 6.
    static const int hashOffsets[] = { 0, 4 };
    static const struct { int shift; int mask; } shifts[] = {
        { 6, 0x3FF }, { 1, 0x7FFF }, { 0, 0xFFFF }
    };

    for (int hashOff : hashOffsets) {
        int hdrPos = hashOff;
        int strPos = hashOff + 2;

        // Verify "None" at strPos
        if (strPos + 4 > 64) continue;
        if (buf[strPos] != 'N' || buf[strPos+1] != 'o' ||
            buf[strPos+2] != 'n' || buf[strPos+3] != 'e') continue;

        // Verify header decodes to length=4
        uint16_t hdr = static_cast<uint16_t>(buf[hdrPos]) |
                       (static_cast<uint16_t>(buf[hdrPos+1]) << 8);
        for (const auto& sh : shifts) {
            int len = (hdr >> sh.shift) & sh.mask;
            if (len == 4) {
                // Bonus: verify next entry header decodes to length=12 ("ByteProperty")
                int nextEntryPos = strPos + 4;
                if (nextEntryPos + 14 < 64) {
                    int nextHdrPos = nextEntryPos;
                    int nextStrPos = nextEntryPos + 2;
                    uint16_t nextHdr = static_cast<uint16_t>(buf[nextHdrPos]) |
                                       (static_cast<uint16_t>(buf[nextHdrPos+1]) << 8);
                    int nextLen = (nextHdr >> sh.shift) & sh.mask;
                    if (nextLen == 12 && memcmp(buf + nextStrPos, "ByteProperty", 12) == 0) {
                        // PERFECT match
                        s_lenShift = sh.shift;
                        s_lenMask  = sh.mask;
                        s_headerOffset = hashOff;
                        *outNoneOffset = strPos;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static uintptr_t ScanModuleForBlocksArray() {
    auto mod = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    int sectionsScanned = 0;
    int candidatesChecked = 0;
    int candidatesValidated = 0;
    DWORD startTick = GetTickCount();

    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;

        char name[9] = {};
        memcpy(name, sec[i].Name, 8);

        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec[i].VirtualAddress;
        size_t sz = sec[i].Misc.VirtualSize;
        if (!sz) sz = sec[i].SizeOfRawData;
        uintptr_t end = start + sz;
        sectionsScanned++;

        printf("[UObjLookup]   scanning '%s' [0x%p..0x%p]\n",
            name, (void*)start, (void*)end);

        for (uintptr_t p = start; p + 64 <= end; p += 8) {
            uintptr_t v0 = 0;
            if (!SafeRead(p, v0)) continue;
            if (!IsHeapPtr(v0)) continue;

            // STRICT: block 0 must be 64KB aligned (FNamePool block size)
            if (v0 & 0xFFFF) continue;

            // Verify the next 7 slots (heap pointers or null)
            bool looksLikeArray = true;
            int heapCount = 1;
            for (int k = 1; k < 8; ++k) {
                uintptr_t vk = 0;
                if (!SafeRead(p + k * 8, vk)) { looksLikeArray = false; break; }
                if (vk == 0) continue;
                if (!IsHeapPtr(vk)) { looksLikeArray = false; break; }
                // Other blocks should also be 64KB aligned
                if (vk & 0xFFFF) { looksLikeArray = false; break; }
                heapCount++;
            }
            if (!looksLikeArray) continue;
            candidatesChecked++;

            // STRICT validation: block 0 must start with a valid "None" entry
            int noneOffset = -1;
            if (!IsValidFNamePoolBlock(v0, &noneOffset)) continue;
            candidatesValidated++;

            // Read full block 0 dump for diagnostics
            uint8_t buf[64] = {};
            SafeReadBytes(v0, buf, 64);

            DWORD ms = GetTickCount() - startTick;
            printf("[UObjLookup] FOUND FNamePool.Blocks[] @ 0x%p in section '%s' (heapCount=%d, hashOff=%d, shift=%d, %ums)\n",
                (void*)p, name, heapCount, s_headerOffset, s_lenShift, ms);
            printf("[UObjLookup]   block 0 = 0x%p (64KB-aligned), 'None' at +%d, 'ByteProperty' validated\n",
                (void*)v0, noneOffset);
            printf("[UObjLookup]   block 0 first 64 bytes:\n[UObjLookup]   ");
            for (int j = 0; j < 64; ++j) {
                printf("%02X ", buf[j]);
                if ((j + 1) % 16 == 0 && j != 63) printf("\n[UObjLookup]   ");
            }
            printf("\n[UObjLookup]   ASCII: '");
            for (int j = 0; j < 64; ++j) {
                char c = static_cast<char>(buf[j]);
                printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("'\n");

            return p;
        }
    }

    DWORD ms = GetTickCount() - startTick;
    printf("[UObjLookup] Module scan done: %d sections, %d candidates, %d validated, %ums — no Blocks[] found\n",
        sectionsScanned, candidatesChecked, candidatesValidated, ms);
    return 0;
}

// Adapter shim: ScanFNamePool now uses the module scan.
static uintptr_t ScanHeapForNoneEntry() { return 0; } // kept for compat (unused)

// Diagnostic dump: 32 bytes BEFORE the None entry, to see if block 0 has
// a header (some UE versions prepend a block header).
static void DumpBytesBefore(uintptr_t addr, int count) {
    uintptr_t start = addr - count;
    uint8_t buf[64] = {};
    if (count > 64) count = 64;
    if (!SafeReadBytes(start, buf, count)) {
        printf("[UObjLookup]   pre-None dump: read failed at 0x%p\n", (void*)start);
        return;
    }
    printf("[UObjLookup]   pre-None bytes (0x%p..0x%p):\n[UObjLookup]   ", (void*)start, (void*)addr);
    for (int i = 0; i < count; ++i) printf("%02X ", buf[i]);
    printf("\n");
}

// Scan the entire non-executable address space of the module for a 8-byte
// value equal to one of several candidate addresses. Returns the static
// pointer location and (via out param) which candidate matched.
static uintptr_t FindStaticPointerToCandidates(
    const std::vector<uintptr_t>& candidates,
    uintptr_t& outMatchValue)
{
    auto mod = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        // Skip executable sections (.text and friends)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        // Skip discardable
        if (sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;

        char name[9] = {};
        memcpy(name, sec[i].Name, 8);

        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec[i].VirtualAddress;
        size_t sz = sec[i].Misc.VirtualSize;
        if (!sz) sz = sec[i].SizeOfRawData;
        uintptr_t end = start + sz;

        printf("[UObjLookup]   scanning section '%s' [0x%p..0x%p] (%zu bytes)\n",
            name, (void*)start, (void*)end, sz);

        for (uintptr_t p = start; p + 8 <= end; p += 8) {
            uintptr_t value = 0;
            if (!SafeRead(p, value)) continue;
            for (uintptr_t cand : candidates) {
                if (value == cand) {
                    outMatchValue = cand;
                    printf("[UObjLookup]   match in section '%s' at 0x%p -> 0x%p\n",
                        name, (void*)p, (void*)cand);
                    return p;
                }
            }
        }
    }
    return 0;
}

// We now have a heap address known to be INSIDE FNamePool block 0 (the
// position of the "ByteProperty" string). FNamePool blocks are 128 KB. The
// static pointer in module .data points to the START of the block, which is
// somewhere in [bpAddr - 128KB, bpAddr]. Scan module memory for any 8-byte
// value in that range — first match is &FNamePool.Blocks[0].
static uintptr_t FindFNamePoolFromNoneEntry(uintptr_t insideBlockAddr) {
    DumpBytesBefore(insideBlockAddr, 32);

    constexpr uintptr_t BLOCK_SIZE = 0x20000; // 128 KB
    uintptr_t windowLo = insideBlockAddr > BLOCK_SIZE ? insideBlockAddr - BLOCK_SIZE : 0;
    uintptr_t windowHi = insideBlockAddr;
    printf("[UObjLookup]   block start window: [0x%p..0x%p]\n",
        (void*)windowLo, (void*)windowHi);

    // Scan module non-executable sections for an 8-byte value within window.
    auto mod = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(mod) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    uintptr_t ptrLoc = 0;
    uintptr_t block0Found = 0;

    for (int i = 0; i < nt->FileHeader.NumberOfSections && !ptrLoc; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;

        char name[9] = {};
        memcpy(name, sec[i].Name, 8);

        uintptr_t start = reinterpret_cast<uintptr_t>(mod) + sec[i].VirtualAddress;
        size_t sz = sec[i].Misc.VirtualSize;
        if (!sz) sz = sec[i].SizeOfRawData;
        uintptr_t end = start + sz;

        printf("[UObjLookup]   scanning '%s' [0x%p..0x%p]\n", name, (void*)start, (void*)end);

        for (uintptr_t p = start; p + 8 <= end; p += 8) {
            uintptr_t value = 0;
            if (!SafeRead(p, value)) continue;
            if (value < windowLo || value >= windowHi) continue;
            // Verify alignment (FNamePool blocks are typically 16-byte aligned by allocator)
            if (value & 0xF) continue;

            ptrLoc = p;
            block0Found = value;
            printf("[UObjLookup]   match: section '%s' @ 0x%p -> block 0x%p\n",
                name, (void*)p, (void*)value);
            break;
        }
    }

    if (!ptrLoc) {
        printf("[UObjLookup]   no static pointer to FNamePool block 0 found\n");
        return 0;
    }

    int blockDelta = static_cast<int>(static_cast<intptr_t>(insideBlockAddr - block0Found));
    printf("[UObjLookup]   FNamePool block 0 = 0x%p, BP string is +%d bytes inside\n",
        (void*)block0Found, blockDelta);

    // ptrLoc is &FNamePool.Blocks[0]. Try common chunksOffsets.
    uintptr_t modBase = reinterpret_cast<uintptr_t>(mod);
    for (int co : { 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 }) {
        if (ptrLoc < modBase + co) continue;
        uintptr_t poolCandidate = ptrLoc - co;
        uintptr_t block1 = 0;
        if (!SafeRead(poolCandidate + co + 8, block1)) continue;
        if (block1 == 0 || IsHeapPtr(block1)) {
            s_chunksOffset = co;
            printf("[UObjLookup] FNamePool struct @ 0x%p (Blocks at 0x%p, ChunksOff=0x%X)\n",
                (void*)poolCandidate, (void*)ptrLoc, co);
            return poolCandidate;
        }
    }
    return 0;
}

// New unified FNamePool discovery via static module scan.
static uintptr_t ScanFNamePool() {
    uintptr_t blocksArrayAddr = ScanModuleForBlocksArray();
    if (!blocksArrayAddr) return 0;

    // blocksArrayAddr = &FNamePool.Blocks[0]. Try common chunksOffsets for
    // the enclosing FNamePool struct.
    auto mod = GetModuleHandleW(nullptr);
    uintptr_t modBase = reinterpret_cast<uintptr_t>(mod);
    for (int co : { 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 }) {
        if (blocksArrayAddr < modBase + co) continue;
        uintptr_t poolCandidate = blocksArrayAddr - co;
        // Sanity: Blocks[1] at Blocks_ptr + 8 is already validated by caller
        s_chunksOffset = co;
        printf("[UObjLookup] FNamePool struct @ 0x%p (Blocks at 0x%p, ChunksOff=0x%X)\n",
            (void*)poolCandidate, (void*)blocksArrayAddr, co);
        return poolCandidate;
    }
    return 0;
}

// === FName -> string resolution ============================================

static std::string ResolveFNameIndex(int32_t nameIndex) {
    if (nameIndex < 0) return std::string();
    if (!s_gnamesAddr) return std::string();

    int32_t blockIdx = nameIndex >> FNAME_BLOCK_OFFSET_BITS;
    int32_t inBlock  = (nameIndex & ((1 << FNAME_BLOCK_OFFSET_BITS) - 1)) * s_stride;
    if (blockIdx < 0 || blockIdx > 8192) return std::string();

    uintptr_t blockPtr = 0;
    if (!SafeRead(s_gnamesAddr + s_chunksOffset + blockIdx * sizeof(uintptr_t), blockPtr))
        return std::string();
    if (!IsHeapPtr(blockPtr)) return std::string();

    uintptr_t entry = blockPtr + inBlock;

    // Read 2-byte header at entry + s_headerOffset. Some pools have 4 bytes
    // of hash prefix, in which case headerOffset = 4.
    uint16_t header = 0;
    if (!SafeRead(entry + s_headerOffset, header)) return std::string();

    int len = (header >> s_lenShift) & s_lenMask;
    bool isWide = (header & 1) != 0;
    if (len <= 0 || len > 1024) return std::string();
    if (isWide) return std::string(); // skip wide for now — UFunctions are ANSI

    char buf[1025];
    if (!SafeReadBytes(entry + s_headerOffset + 2, buf, len)) return std::string();

    std::string out;
    out.reserve(len);
    for (int i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
    }
    return out;
}

// Auto-detect FNamePool layout: 4D scan over (ChunksOffset, headerOffset,
// lenShift, stride). First combination that decodes FName[0]='None' wins.
static bool DetectFNamePoolLayout() {
    static const int chunkOffsets[] = { 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50 };
    static const int hdrOffsets[]   = { 0, 4 };
    struct ShiftCfg { int shift; int mask; };
    static const ShiftCfg shifts[] = {
        { 6, 0x3FF },
        { 1, 0x7FFF },
        { 0, 0xFFFF },
    };
    static const int strides[] = { 2, 4 };

    int triedCombos = 0;
    for (int chunksOff : chunkOffsets) {
        uintptr_t blockPtr = 0;
        if (!SafeRead(s_gnamesAddr + chunksOff, blockPtr)) continue;
        if (!IsHeapPtr(blockPtr)) continue;

        for (int hdrOff : hdrOffsets) {
            for (const auto& sh : shifts) {
                for (int stride : strides) {
                    triedCombos++;
                    s_chunksOffset = chunksOff;
                    s_headerOffset = hdrOff;
                    s_lenShift     = sh.shift;
                    s_lenMask      = sh.mask;
                    s_stride       = stride;

                    std::string name0 = ResolveFNameIndex(0);
                    if (name0 == "None") {
                        printf("[UObjLookup] FNamePool layout: ChunksOff=0x%X hdrOff=%d shift=%d mask=0x%X stride=%d\n",
                            chunksOff, hdrOff, sh.shift, sh.mask, stride);
                        return true;
                    }
                }
            }
        }
    }

    // Detection failed — dump first 64 bytes of the first block for diagnosis.
    printf("[UObjLookup] FNamePool detection FAILED after %d combos. Block dump:\n", triedCombos);
    for (int chunksOff : chunkOffsets) {
        uintptr_t blockPtr = 0;
        if (!SafeRead(s_gnamesAddr + chunksOff, blockPtr)) continue;
        if (!IsHeapPtr(blockPtr)) continue;
        printf("[UObjLookup]   pool+0x%02X -> block 0x%p:\n", chunksOff, (void*)blockPtr);
        uint8_t buf[64] = {};
        if (SafeReadBytes(blockPtr, buf, 64)) {
            printf("[UObjLookup]     ");
            for (int i = 0; i < 64; ++i) {
                printf("%02X ", buf[i]);
                if ((i + 1) % 16 == 0) printf("\n[UObjLookup]     ");
            }
            printf("\n[UObjLookup]     ASCII: '");
            for (int i = 0; i < 64; ++i) {
                char c = static_cast<char>(buf[i]);
                printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            printf("'\n");
        }
        break; // only dump first valid block
    }
    return false;
}

std::string ReadFNameAt(uintptr_t fnameAddr) {
    int32_t compIdx = 0;
    if (!SafeRead(fnameAddr, compIdx)) return std::string();
    return ResolveFNameIndex(compIdx);
}

std::string GetObjectName(uintptr_t uobjectAddr) {
    if (!uobjectAddr) return std::string();
    return ReadFNameAt(uobjectAddr + OFF_UOBJECT_NAME);
}

std::string GetObjectClassName(uintptr_t uobjectAddr) {
    if (!uobjectAddr) return std::string();
    uintptr_t cls = 0;
    if (!SafeRead(uobjectAddr + OFF_UOBJECT_CLASS, cls)) return std::string();
    if (!IsHeapPtr(cls)) return std::string();
    return GetObjectName(cls);
}

// === GObjects iteration ====================================================

int32_t GetObjectCount() {
    if (!s_gobjectsAddr) return 0;
    int32_t n = 0;
    SafeRead(s_gobjectsAddr + OFF_OBJ_ARRAY_NUMELEMS, n);
    return n;
}

uintptr_t GetObjectByIndex(int32_t index) {
    if (!s_gobjectsAddr || index < 0) return 0;

    uintptr_t arrayBase = 0;
    if (!SafeRead(s_gobjectsAddr + OFF_OBJ_ARRAY_OBJECTS, arrayBase) || !arrayBase)
        return 0;

    int32_t chunkIdx   = index / OBJECTS_PER_CHUNK;
    int32_t withinChunk = index % OBJECTS_PER_CHUNK;

    uintptr_t chunk = 0;
    if (!SafeRead(arrayBase + chunkIdx * sizeof(uintptr_t), chunk) || !chunk) return 0;

    uintptr_t itemAddr = chunk + static_cast<uintptr_t>(withinChunk) * FUOBJECT_ITEM_SIZE;
    uintptr_t obj = 0;
    if (!SafeRead(itemAddr, obj)) return 0;
    return obj;
}

int32_t GetObjectSerialNumberByIndex(int32_t index) {
    if (!s_gobjectsAddr || index < 0) return 0;

    uintptr_t arrayBase = 0;
    if (!SafeRead(s_gobjectsAddr + OFF_OBJ_ARRAY_OBJECTS, arrayBase) || !arrayBase)
        return 0;

    int32_t chunkIdx   = index / OBJECTS_PER_CHUNK;
    int32_t withinChunk = index % OBJECTS_PER_CHUNK;

    uintptr_t chunk = 0;
    if (!SafeRead(arrayBase + chunkIdx * sizeof(uintptr_t), chunk) || !chunk) return 0;

    uintptr_t itemAddr = chunk + static_cast<uintptr_t>(withinChunk) * FUOBJECT_ITEM_SIZE;
    int32_t serial = 0;
    // FUObjectItem layout: { Object*@+0, Flags@+8, ClusterRootIndex@+0xC,
    // SerialNumber@+0x10 }, size 24 (FUOBJECT_ITEM_SIZE).
    if (!SafeRead(itemAddr + 0x10, serial)) return 0;
    return serial;
}

// === Function / class lookup ===============================================

uintptr_t FindFunctionInClass(uintptr_t uclassAddr, const char* funcName) {
    if (!uclassAddr || !funcName || !*funcName) return 0;

    uintptr_t child = 0;
    if (!SafeRead(uclassAddr + OFF_USTRUCT_CHILDREN, child) || !child) return 0;

    int safety = 4096;
    while (child && safety-- > 0) {
        // Verify this child is a UFunction by checking its class name
        std::string clsName = GetObjectClassName(child);
        if (clsName == "Function") {
            std::string name = GetObjectName(child);
            if (name == funcName) return child;
        }

        // Walk to next child (UField::Next)
        uintptr_t next = 0;
        if (!SafeRead(child + OFF_UFIELD_NEXT, next)) break;
        child = next;
    }
    return 0;
}

uintptr_t FindClassByName(const char* className) {
    if (!className || !*className) return 0;
    int32_t total = GetObjectCount();
    if (total <= 0) return 0;

    for (int32_t i = 0; i < total; ++i) {
        uintptr_t obj = GetObjectByIndex(i);
        if (!obj) continue;

        std::string clsOfObj = GetObjectClassName(obj);
        if (clsOfObj != "Class" && clsOfObj != "BlueprintGeneratedClass") continue;

        std::string name = GetObjectName(obj);
        if (name == className) return obj;
    }
    return 0;
}

uintptr_t GetUFunctionNativeAddr(uintptr_t ufunctionAddr) {
    if (!ufunctionAddr) return 0;
    uintptr_t func = 0;
    if (!SafeRead(ufunctionAddr + OFF_UFUNCTION_FUNC, func)) return 0;
    if (!IsCodePtr(func)) return 0;
    return func;
}

// === Thunk -> C++ impl resolver (HDE64-based proper disassembler) ==========
//
// UE UFunction thunks generated by UnrealHeaderTool follow a predictable
// pattern in compiled x64 code:
//
//   ; --- Prologue ---
//   push   rbp,rbx,...
//   sub    rsp, X
//   mov    rXX, r8       ; SAVE the result buffer pointer (3rd fastcall arg)
//   mov    rXX, rdx      ; save FFrame*
//   mov    rXX, rcx      ; save UObject*
//
//   ; --- Argument unpacking from FFrame ---
//   call   FFrame::Step  ; or specific arg readers (helpers)
//   call   FFrame::Step
//   ...
//
//   ; --- Native call ---
//   mov    args, ...     ; setup args for the C++ impl
//   call   cpp_impl      ; <-- THIS IS WHAT WE WANT
//
//   ; --- Result write ---
//   mov    [resultReg], al/ax/eax/rax  ; write return value to caller buffer
//
//   ; --- Epilogue ---
//   add    rsp, X
//   pop    ...
//   ret
//
// To find the impl call reliably:
//   1. Walk the prologue with HDE64 looking for "mov regX, r8" — that tells
//      us which register holds the result buffer pointer.
//   2. Walk all instructions collecting CALL rel32 targets and positions.
//   3. Walk all instructions collecting "mov [resultReg], xxx" positions.
//   4. The impl call is the CALL immediately preceding a write to [resultReg].
//
// This works for ANY UE 4.x / 5.x UFunction thunk because UHT-generated code
// is structurally identical across UE versions and game builds.

// Identify which 64-bit register the thunk uses to hold the result buffer.
// Returns the x86-64 register encoding (0=rax, 1=rcx, ..., 7=rdi, 8=r8, ..., 15=r15).
// Defaults to 8 (r8) if no save instruction is found in the prologue.
static int IdentifyResultRegister(const uint8_t* code, size_t maxScan) {
    size_t pos = 0;
    while (pos < maxScan) {
        hde64s hs = {};
        unsigned int len = hde64_disasm(code + pos, &hs);
        if (hs.flags & F_ERROR || len == 0) break;

        // Match "mov reg64, r8":
        //   REX.W=1 (64-bit operand)
        //   REX.B=1 (source register r8 is r0 + REX.B<<3)
        //   opcode=0x8B (MOV r64, rm64)
        //   modrm.mod=3 (register-to-register)
        //   modrm.rm=0 (rm = 0 → with REX.B → r8)
        if (hs.opcode == 0x8B && hs.rex_w && hs.rex_b &&
            hs.modrm_mod == 3 && hs.modrm_rm == 0)
        {
            int destReg = (hs.rex_r ? 8 : 0) + hs.modrm_reg;
            return destReg;
        }

        pos += len;
    }
    return 8; // r8 was not saved; result buffer stays in r8
}

// Check if the disassembled instruction is a memory store whose base
// register is the given register code.
static bool IsStoreToReg(const hde64s& hs, int targetReg) {
    // MOV r/m8, r8  (opcode 0x88)
    // MOV r/m16/32/64, r16/32/64  (opcode 0x89)
    if (hs.opcode != 0x88 && hs.opcode != 0x89) return false;
    // Must be a memory destination (not register-register)
    if (hs.modrm_mod == 3) return false;
    // Some encodings use SIB (modrm.rm == 4) — those have a different base
    // calculation. For UE thunks the result is always [reg] without SIB.
    if (hs.modrm_rm == 4) return false;
    // RIP-relative addressing (mod=00, rm=5) is not what we want either
    if (hs.modrm_mod == 0 && hs.modrm_rm == 5) return false;

    int baseReg = (hs.rex_b ? 8 : 0) + hs.modrm_rm;
    return baseReg == targetReg;
}

uintptr_t ResolveThunkToImpl(uintptr_t thunkAddr) {
    if (!thunkAddr) return 0;

    // Read 512 bytes of the thunk. Try smaller if read fails.
    uint8_t buf[512] = {};
    size_t available = 512;
    if (!SafeReadBytes(thunkAddr, buf, available)) {
        available = 256;
        if (!SafeReadBytes(thunkAddr, buf, available)) {
            available = 128;
            if (!SafeReadBytes(thunkAddr, buf, available)) return 0;
        }
    }

    // Phase 1: identify result-buffer-holding register
    int resultReg = IdentifyResultRegister(buf, (available > 80 ? 80u : (unsigned)available));

    // Phase 2: walk all instructions, collecting CALL rel32 entries and
    // positions of writes to [resultReg].
    struct CallInfo { size_t pos; size_t len; uintptr_t target; };
    constexpr int MAX_CALLS = 64;
    CallInfo calls[MAX_CALLS] = {};
    int callCount = 0;

    constexpr int MAX_WRITES = 16;
    size_t writePositions[MAX_WRITES] = {};
    int writeCount = 0;

    size_t pos = 0;
    while (pos < available) {
        hde64s hs = {};
        unsigned int len = hde64_disasm(buf + pos, &hs);
        if (hs.flags & F_ERROR || len == 0) break;

        // CALL rel32 (E8)
        if (hs.opcode == 0xE8 && callCount < MAX_CALLS) {
            int32_t rel = static_cast<int32_t>(hs.imm.imm32);
            uintptr_t target = thunkAddr + pos + len + rel;
            calls[callCount++] = { pos, len, target };
        }

        // Store to result register
        if (IsStoreToReg(hs, resultReg) && writeCount < MAX_WRITES) {
            writePositions[writeCount++] = pos;
        }

        // Stop walking after we hit a RET (epilogue done)
        if (hs.opcode == 0xC3) break;

        pos += len;
    }

    if (callCount == 0) return 0;

    // Phase 3: find the impl call by matching it to a result write.
    // Strategy: for each write to [resultReg], find the closest preceding
    // CALL within 64 bytes. The first such call (chronologically) where the
    // target is in code section is the impl.
    for (int w = 0; w < writeCount; ++w) {
        size_t writePos = writePositions[w];
        for (int c = callCount - 1; c >= 0; --c) {
            if (calls[c].pos >= writePos) continue;
            size_t gap = writePos - (calls[c].pos + calls[c].len);
            if (gap > 64) continue;
            if (!IsCodePtr(calls[c].target)) continue;
            return calls[c].target;
        }
    }

    // Fallback: if no result write was found (or no call pairs with it),
    // return the LAST CALL in the function — most thunks end with a call
    // to the impl as a tail-style invocation.
    for (int c = callCount - 1; c >= 0; --c) {
        if (IsCodePtr(calls[c].target)) {
            return calls[c].target;
        }
    }

    return 0;
}

uintptr_t FindNativeFunction(const char* className, const char* funcName) {
    if (!s_initialized) {
        printf("[UObjLookup] FindNativeFunction(%s::%s): not initialized\n",
            className, funcName);
        return 0;
    }

    uintptr_t cls = FindClassByName(className);
    if (!cls) {
        printf("[UObjLookup] FindNativeFunction: class '%s' not found\n", className);
        return 0;
    }

    uintptr_t ufunc = FindFunctionInClass(cls, funcName);
    if (!ufunc) {
        printf("[UObjLookup] FindNativeFunction: %s.%s not found in class\n",
            className, funcName);
        return 0;
    }

    uintptr_t thunk = GetUFunctionNativeAddr(ufunc);
    if (!thunk) {
        printf("[UObjLookup] %s.%s: UFunc=0x%p but Func ptr invalid\n",
            className, funcName, (void*)ufunc);
        return 0;
    }

    // Walk past the thunk to find the C++ impl (safe to byte-patch).
    uintptr_t impl = ResolveThunkToImpl(thunk);
    if (impl) return impl;

    // Fallback: return the thunk if impl resolution failed.
    printf("[UObjLookup] %s.%s: thunk=0x%p impl resolve FAILED, returning thunk\n",
        className, funcName, (void*)thunk);
    return thunk;
}

// === UPROPERTY offset resolver ============================================
//
// For UE 4.25+ (with bUseFProperty=true), properties are stored as FField/
// FProperty objects in a separate chain from functions. Layout for UE 4.27:
//
//   UStruct
//     +0x40  UStruct*     Super         (parent class pointer)
//     +0x48  UField*      Children      (function chain — UFunction objects)
//     +0x50  FField*      ChildProperties (property chain — FField/FProperty)
//
//   FField (base of FProperty)
//     +0x20  FField*      Next          (next link in chain)
//     +0x28  FName        NamePrivate   (uint32 ComparisonIndex first)
//
//   FProperty (inherits FField)
//     +0x4C  int32        Offset_Internal  (the byte offset we want)

constexpr int OFF_USTRUCT_SUPER         = 0x40;
constexpr int OFF_USTRUCT_CHILDPROPS    = 0x50;
constexpr int OFF_FFIELD_NEXT           = 0x20;
constexpr int OFF_FFIELD_NAME           = 0x28;
constexpr int OFF_FPROPERTY_OFFSET_INT  = 0x4C;

int32_t FindPropertyOffsetInClass(uintptr_t uclassAddr, const char* propName) {
    if (!uclassAddr || !propName || !*propName) return -1;

    uintptr_t field = 0;
    if (!SafeRead(uclassAddr + OFF_USTRUCT_CHILDPROPS, field) || !field) return -1;

    int safety = 4096;
    while (field && safety-- > 0) {
        std::string name = ReadFNameAt(field + OFF_FFIELD_NAME);
        if (name == propName) {
            int32_t off = -1;
            if (SafeRead(field + OFF_FPROPERTY_OFFSET_INT, off)) return off;
            return -1;
        }
        uintptr_t next = 0;
        if (!SafeRead(field + OFF_FFIELD_NEXT, next)) break;
        field = next;
    }
    return -1;
}

int32_t FindPropertyOffset(const char* className, const char* propName) {
    if (!s_initialized || !className || !propName) return -1;

    uintptr_t cls = FindClassByName(className);
    if (!cls) {
        printf("[UObjLookup] FindPropertyOffset: class '%s' not found\n", className);
        return -1;
    }

    int hops = 0;
    while (cls && hops < 16) {
        int32_t offset = FindPropertyOffsetInClass(cls, propName);
        if (offset >= 0) {
            printf("[UObjLookup] %s::%s -> +0x%X\n", className, propName, offset);
            return offset;
        }
        // Walk to parent class via UStruct::Super
        uintptr_t super = 0;
        if (!SafeRead(cls + OFF_USTRUCT_SUPER, super) || !super) break;
        cls = super;
        hops++;
    }

    printf("[UObjLookup] %s::%s NOT FOUND (walked %d parent classes)\n",
        className, propName, hops);
    return -1;
}

// === Diagnostic enumeration (used to discover real class/function names) ===

static void StrLowerInPlace(std::string& s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

void DumpClassesContaining(const char* substring, int maxResults) {
    if (!s_initialized || !substring || !*substring) return;

    std::string needle(substring);
    StrLowerInPlace(needle);

    int total = GetObjectCount();
    int found = 0;
    printf("[UObjLookup] === DumpClassesContaining('%s') ===\n", substring);

    for (int32_t i = 0; i < total && found < maxResults; ++i) {
        uintptr_t obj = GetObjectByIndex(i);
        if (!obj) continue;

        std::string clsOfObj = GetObjectClassName(obj);
        // Filter: only UClass-derived objects (so we don't enumerate every instance)
        if (clsOfObj != "Class" && clsOfObj != "BlueprintGeneratedClass" &&
            clsOfObj != "ScriptStruct") continue;

        std::string name = GetObjectName(obj);
        if (name.empty()) continue;

        std::string lname = name;
        StrLowerInPlace(lname);
        if (lname.find(needle) == std::string::npos) continue;

        printf("[UObjLookup]   [%d] %s @ 0x%p (kind=%s)\n",
            i, name.c_str(), (void*)obj, clsOfObj.c_str());
        found++;
    }
    printf("[UObjLookup] === %d match(es) ===\n", found);
}

void DumpFunctionsOf(uintptr_t uclassAddr, int maxResults) {
    if (!s_initialized || !uclassAddr) return;

    uintptr_t child = 0;
    if (!SafeRead(uclassAddr + OFF_USTRUCT_CHILDREN, child) || !child) {
        printf("[UObjLookup] DumpFunctionsOf: class @ 0x%p has no Children\n", (void*)uclassAddr);
        return;
    }

    int found = 0;
    int safety = 4096;
    printf("[UObjLookup] === DumpFunctionsOf(0x%p) ===\n", (void*)uclassAddr);
    while (child && safety-- > 0 && found < maxResults) {
        std::string clsName = GetObjectClassName(child);
        if (clsName == "Function") {
            std::string name = GetObjectName(child);
            uintptr_t native = GetUFunctionNativeAddr(child);
            printf("[UObjLookup]   %s @ 0x%p (Native=0x%p)\n",
                name.c_str(), (void*)child, (void*)native);
            found++;
        }
        uintptr_t next = 0;
        if (!SafeRead(child + OFF_UFIELD_NEXT, next)) break;
        child = next;
    }
    printf("[UObjLookup] === %d function(s) ===\n", found);
}

// === Init ==================================================================

bool Initialize() {
    if (s_initialized) return true;

    HMODULE mod = GetModuleHandleW(nullptr);
    s_moduleBase = reinterpret_cast<uintptr_t>(mod);
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info));
    s_moduleSize = info.SizeOfImage;
    printf("[UObjLookup] module base=0x%p size=0x%zX\n",
        (void*)s_moduleBase, s_moduleSize);

    s_gobjectsAddr = ScanGObjects();
    if (!s_gobjectsAddr) {
        printf("[UObjLookup] ERROR: failed to find GObjects\n");
        return false;
    }

    s_gnamesAddr = ScanFNamePool();
    if (!s_gnamesAddr) {
        printf("[UObjLookup] ERROR: failed to find FNamePool via heap scan\n");
        return false;
    }
    // ScanFNamePool already set s_chunksOffset and s_lenShift via the
    // landmark-based discovery; no further auto-detection needed.

    // Final sanity check using the resolved layout
    std::string none = ResolveFNameIndex(0);
    std::string name1 = ResolveFNameIndex(1);
    printf("[UObjLookup] FName[0]='%s' FName[1]='%s'\n", none.c_str(), name1.c_str());

    if (none != "None") {
        printf("[UObjLookup] WARNING: FName[0] != 'None' after layout detection\n");
    }

    s_initialized = true;
    printf("[UObjLookup] initialized OK (objects=%d)\n", GetObjectCount());
    return true;
}

bool IsInitialized() { return s_initialized; }

} // namespace UObjectLookup
