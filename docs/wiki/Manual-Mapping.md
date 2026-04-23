# Manual Mapping

This page is the pedagogical companion to
`native/injector/src/core/ManualMap.cpp`. It walks through what a PE
manual-mapper actually does — every stage of the Windows loader, redone
by hand against a remote process — and ties each stage to the lines of
code that implement it.

> **Scope.** This is an *educational* implementation. Its purpose is
> to make the PE format and the Windows loader concrete. It is **not**
> an anti-cheat bypass; the project ZeusMod is a single-player trainer
> and the standard `LoadLibraryW` injector remains the default for
> reasons of simplicity and reliability. The manual mapper is opt-in
> behind the `--manual` CLI flag of `IcarusInjector.exe`.

---

## What does "manual mapping" actually mean?

When you call `LoadLibraryW(L"foo.dll")`, the Windows loader does a
surprising amount of work for you:

1. Opens the file.
2. Maps it into memory section-by-section.
3. Applies base relocations.
4. Resolves imports against other already-loaded modules.
5. Initializes TLS storage.
6. Calls `DllMain(DLL_PROCESS_ATTACH)`.
7. Registers the DLL in the **PEB** (`PEB->Ldr->InMemoryOrderModuleList`)
   so that `EnumProcessModules`, `GetModuleHandle`, debuggers, EDR
   notifications, etc. all see it.

A **manual mapper** does steps 1–6 by hand, in a chosen target process,
**skipping step 7**. The DLL ends up fully resolved, fully callable,
fully working — but it does not appear in the standard module list.

That side-effect is what makes manual mapping useful for studying the
loader: the only difference between a manually-mapped DLL and a
loader-mapped one is that you did the bookkeeping yourself, so you
can inspect every stage and see how it worked.

---

## The 10 steps in detail

The implementation in `ManualMap.cpp` follows these 10 steps in order.
Each step here matches a comment block in the source.

### 1. Read the DLL from disk

```cpp
ReadFileToBuffer(dllPath, file);
```

Pulls the entire file into a host buffer. We need it whole because PE
structures reference each other by file-relative offsets — you can't
parse the import table without first walking the optional header,
which is several pages in.

### 2. Parse PE headers

```
IMAGE_DOS_HEADER       (e_magic = 'MZ')
  e_lfanew → IMAGE_NT_HEADERS64
              .Signature == 'PE\0\0'
              .FileHeader   (machine, NumberOfSections, …)
              .OptionalHeader (entry point, ImageBase, sizes, data dirs)
```

We refuse anything that isn't `IMAGE_FILE_MACHINE_AMD64` early — our
manual mapper does not handle 32-bit PE images.

### 3. Allocate `SizeOfImage` in the target

```cpp
remoteBase = VirtualAllocEx(process, nullptr, imageSize,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
```

Whole image, RW for now. We'll demote sections individually at step 8.
Letting Windows choose the address (passing `nullptr`) means the base
will almost certainly differ from the binary's preferred
`OptionalHeader.ImageBase` — that's why step 5 exists.

### 4. Map sections

```cpp
memcpy(localImage + section->VirtualAddress,
       file       + section->PointerToRawData,
       section->SizeOfRawData);
```

We do all the section / reloc / import work in a host-side scratch
buffer (`localImage`) and push the result to the target in one
`WriteProcessMemory` at the end. Easier to debug — you can dump the
local image in the host and validate it without bouncing across
processes.

### 5. Apply base relocations

```
delta = remoteBase - OptionalHeader.ImageBase

for each IMAGE_BASE_RELOCATION block:
  for each WORD entry:
    if type == IMAGE_REL_BASED_DIR64:
      *(uint64_t*)(image + block.VA + entry.offset) += delta
```

The compiler bakes absolute addresses into the binary assuming it'll
be loaded at `ImageBase`. Whenever it isn't, every absolute pointer
needs to be adjusted by `delta`. The reloc table is a dense list of
per-page patch instructions; on x64 the only type that matters is
`IMAGE_REL_BASED_DIR64` (a 64-bit absolute pointer).

### 6. Resolve imports

For each entry in the import table:

```
mod = GetModuleHandleA(dllName)  // or LoadLibraryA if not loaded yet
for each thunk:
  fn = (ordinal flag set) ? GetProcAddress(mod, ordinal)
                          : GetProcAddress(mod, byName.Name)
  iatThunk.u1.Function = fn
```

We resolve in **our own** address space. Windows ASLR shares system-DLL
base addresses across processes within the same boot session, so
`GetProcAddress` in the injector returns the same address that
`GetProcAddress` in the target would. That assumption fails in two
cases:

- **Non-system DLLs** — if the target had `LoadLibrary` already loaded
  a library at a different base. ZeusMod's DLL only imports from
  kernel32, ntdll, user32, ws2_32 and a handful of other system
  modules, so this isn't an issue here. A production-grade mapper
  would synthesize a small bootstrap shellcode that resolves imports
  inside the target instead.
- **Cross-architecture or sandboxed processes** — out of scope for us.

### 7. Push the image to the target

```cpp
WriteProcessMemory(process, remoteBase, localImage.data(), imageSize);
```

A single bulk transfer of the fully fixed-up image.

### 8. Apply per-section page protections

```
for each section:
  prot = SectionProtection(section.Characteristics)  // RX / R / RW / …
  VirtualProtectEx(process, remoteBase + section.VA, section.VirtualSize, prot)
```

Demotes the page protection from `PAGE_READWRITE` (used to receive
the WriteProcessMemory) to the section's intended state: `.text` →
`PAGE_EXECUTE_READ`, `.rdata` → `PAGE_READONLY`, `.data` →
`PAGE_READWRITE`, etc. This restores W^X / DEP semantics — without
this step the entire image would stay RW, which is a debugging aid
but a security smell.

### 9. TLS callbacks

If the DLL has a TLS directory, every callback in
`AddressOfCallBacks` must run **before** DllMain. The CRT uses TLS
callbacks to initialise per-thread state, exception handling tables,
etc. Skipping them leaves the DLL in a half-initialised state.

```cpp
for each callback in tls.AddressOfCallBacks:
  CreateRemoteThread(target, callback, base, …)
  WaitForSingleObject(thread)
```

### 10. Call DllMain(DLL_PROCESS_ATTACH)

```cpp
entry = remoteBase + OptionalHeader.AddressOfEntryPoint
CreateRemoteThread(target, entry, remoteBase, …)
```

`CreateRemoteThread` only gives us **one** parameter slot, but
`DllMain` wants three (`hModule`, `reason`, `reserved`). For a
learning implementation we cheat: we pass the remote base as the
single argument, which lands in `rcx` (the first integer argument
under the Windows x64 calling convention). That's correct for
`hModule`. `rdx` and `r8` are whatever was in those registers when
the thread started, but most CRT-stub `DllMain`s only branch on
`reason == DLL_PROCESS_ATTACH`, and our value happens to be non-zero
which the stub treats as "first call".

For a production-grade mapper you'd write a small shellcode bootstrap
that sets up `rcx`, `rdx`, `r8` correctly and only then jumps to the
entry point. This is the standard pattern in
[**Blackbone**](https://github.com/DarthTon/Blackbone) and
[**Reflective DLL Injection**](https://github.com/stephenfewer/ReflectiveDLLInjection).

---

## Trying it out

```powershell
# Standard LoadLibraryW path (default)
IcarusInjector.exe

# Pedagogical manual mapper
IcarusInjector.exe --manual
```

On success the status bar reports:

```
Mapped @0x7FFA12340000  size=0x158000  sections=8  relocs=2347  imports=132  tls=0
```

You can verify the DLL is **not** in the standard module list:

```python
# scripts/inspect.py
modules
# IcarusInternal.dll is missing — manual mapping skipped the PEB
# registration. Standard LoadLibraryW would have shown it here.
```

But it is callable — every cheat still works because the trainer
thread is running inside the mapped image.

---

## Caveats and rough edges

- **No exception handling tables.** The mapper does not register
  `RUNTIME_FUNCTION` entries for the mapped image, which means C++
  exceptions thrown inside the DLL won't unwind through external
  frames cleanly. Most ZeusMod code paths use SEH or simple integer
  return codes precisely because of this — but be aware.
- **No FreeLibrary support.** Manually mapped images cannot be
  unloaded with `FreeLibrary` because the loader doesn't know about
  them. Press <kbd>F10</kbd> in-game to do the trainer's own
  shutdown, but the memory will only be reclaimed when Icarus exits.
- **Imports assumed system-DLL.** See step 6. Acceptable for ZeusMod
  whose imports are exclusively system DLLs.
- **DllMain calling convention shortcut.** See step 10. Works for
  ZeusMod's DllMain, would need a shellcode bootstrap for
  general-purpose use.

These limitations are in the source as comments next to the
relevant code, so a reader of `ManualMap.cpp` will see them in
context.

---

## Why this lives in ZeusMod

ZeusMod is an open-source single-player trainer for *Icarus* — a
game with no anti-cheat. There is no anti-cheat to bypass on this
target. The manual mapper is included because:

- It pairs naturally with the existing `inspect.py` reflection
  tooling — you can compare the loader's view of the world with the
  mapper's view of the world side by side.
- It documents in code what the Windows Internals book describes
  in prose.
- It's the kind of system-programming work that's hard to motivate
  on a synthetic target but easy and fun on a real one.

The standard `LoadLibraryW` path stays the default, the desktop
launcher (`koffi` injector) doesn't change, and the wiki disclaimer
([Home](Home), [FAQ](FAQ)) remains accurate: ZeusMod doesn't ship
detection-evasion features, and using it on multiplayer / official
servers is not in scope.

---

## Further reading

- Microsoft, *PE Format* —
  https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
- Russinovich, Solomon, Ionescu, *Windows Internals 7e*, chapter 5
  (Process loading and the loader).
- Skywing, *What Goes On Inside Windows: Solving the Mysteries of
  the Loader* — Uninformed Vol. 8.
- DarthTon, *Blackbone* — production-grade open-source mapper.
- Stephen Fewer, *Reflective DLL Injection* — the original paper
  on self-loading DLLs.

---

## See also

- [Architecture](Architecture) — where the injector fits in the
  overall ZeusMod component layout.
- [Build From Source](Build-From-Source) — how to build the new
  `--manual` path.
- [Hook Catalog](Hook-Catalog) — what runs once the DLL is mapped.
