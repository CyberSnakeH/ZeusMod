# Glossary

Terms that come up across the wiki, each in one sentence.

---

**AOB / Array-Of-Bytes signature.** A fixed byte pattern used to locate
a function in a compiled binary. ZeusMod deliberately avoids them in
favour of UE reflection, because they break on every patch.

**Blueprint.** Unreal's visual scripting system. Most gameplay logic
in Icarus is implemented as Blueprints with a thin C++ layer.

**Composite (inspect.py).** A client-side command that orchestrates
several pipe primitives into one result — e.g. `struct`, `dump` with
label overlays, `snapshot`/`diff`.

**Detour.** A function-patching technique that re-routes a call from
the original function to a custom wrapper. ZeusMod uses
[MinHook](https://github.com/TsudaKageyu/minhook) to install detours.

**Electron.** The Chromium + Node runtime ZeusMod's desktop launcher
runs on. See [Desktop App](Desktop-App.md).

**`FField` / `FProperty` / `UFunction`.** The reflection types
Unreal uses for structural data and methods. ZeusMod walks the
`FField` linked list off `UStruct::Children` to find everything by
name.

**`FName`.** Unreal's interned string type, hashed into `GNames`.
Every class, function and property name in the engine is an
`FName`.

**`FWeakObjectPtr`.** A reference to a `UObject` that survives the
object being destroyed (the pointer compares against
`GObjects[index]` and the recorded serial number). Free Craft
injects synthetic `FWeakObjectPtr`s into an unreflected `TArray`
inside `DeployableTickSubsystem`.

**`GObjects`.** The global table of all live `UObject`s. ZeusMod
walks it to find live instances (`findobj`, `listobj` in
`inspect.py`).

**ImGui.** [Dear ImGui](https://github.com/ocornut/imgui), the
immediate-mode GUI library used for the in-game overlay.

**IPC.** Inter-Process Communication. In ZeusMod that means two
Windows named pipes: `\\.\pipe\ZeusModPipe` and
`\\.\pipe\ZeusModDbg`.

**koffi.** A Node.js native-function FFI library. Used by the 1.5+
injector to call kernel32 directly from the Electron main process,
replacing the old PowerShell helper.

**MinHook.** The x86/x64 function-hooking library ZeusMod uses to
install detours. Fast, small, MIT-licensed.

**NSIS.** Nullsoft Scriptable Install System — the installer
toolkit `electron-builder` generates into
`ZeusMod-Setup-X.Y.Z.exe`.

**Pipe protocol.** The wire format for the two named pipes. See
[Pipe Protocol](Pipe-Protocol.md).

**Prospect.** Icarus's term for a mission / session — loaded from the
station and ended by finishing objectives or dying. ZeusMod only
makes sense with a live prospect.

**Reflection.** UE's structural knowledge of its own classes,
structs, functions and properties. The whole reason ZeusMod doesn't
rely on byte signatures. See [Reflection Internals](Reflection-Internals.md).

**SEH / Structured Exception Handling.** Windows's OS-level
exception mechanism (`__try` / `__except`). ZeusMod uses SEH for
raw-memory probes where a C++ exception would be the wrong tool.

**Thunk.** A tiny wrapper the UE compiler generates for every
UFunction's Blueprint-callable path. `FindNativeFunction` walks the
thunk to reach the C++ `exec` body — detouring the thunk alone would
miss native callers.

**Tick.** One pass of UE's per-frame update loop. ZeusMod's cheats
that clamp a value run in their own trainer thread, not in the UE
tick — but the game's own tick is what reads the clamped values.

**UPROPERTY.** A C++ member on a UClass that is registered with UE's
reflection system. ZeusMod resolves UPROPERTY offsets at runtime via
`UObjectLookup::FindPropertyOffset` — no build-time dump required.

**UObject.** The root of Unreal's object hierarchy. Almost every
interesting runtime thing in Icarus is a UObject.

**UClass / UScriptStruct / UEnum.** The reflection objects that
describe classes, structs, and enums respectively. ZeusMod finds
them via `UObjectLookup::FindClassByName` /
`FindScriptStructByName`.

---

## Two words that *look* interchangeable but aren't

- **Hook** vs **detour.** Hook is the *verb* (what ZeusMod does);
  detour is the *technique* (where you patch the function's prologue
  to jump into your wrapper). MinHook installs detours on our
  behalf when we tell it to hook something.

- **Inject** vs **attach.** Inject is the low-level act of getting
  the DLL loaded into Icarus. Attach is the end-user button that
  does `detect PID + inject + wait for pipe`. In conversation we
  tend to use "attach" for the UI operation and "inject" for what
  the launcher does behind the scenes.

---

See [FAQ](FAQ.md) for higher-level answers or [Home](Home.md) for
the full wiki map.
