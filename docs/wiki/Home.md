<h1 align="center">ZeusMod Wiki</h1>

<p align="center">
  <em>Reference &amp; internals for the ZeusMod trainer, native injector,
  Electron desktop companion, and <code>inspect.py</code> debug client.</em>
</p>

<p align="center">
  <img alt="Version"  src="https://img.shields.io/badge/version-1.5.0-6e56cf">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20x64-0a84ff">
  <img alt="Engine"   src="https://img.shields.io/badge/engine-UE%204.27-13aa52">
</p>

---

## Where to start

| If you want to…                                 | Read this                                          |
|--------------------------------------------------|----------------------------------------------------|
| Install the release and toggle cheats           | [Getting Started](Getting-Started.md)              |
| Understand how the four components fit together | [Architecture](Architecture.md)                    |
| Build everything from source                    | [Build From Source](Build-From-Source.md)          |
| Learn what each cheat does and how              | [Feature Reference](Feature-Reference.md)          |
| Drive ZeusMod from the command line             | [Debug Client](Debug-Client.md)                    |
| Extend ZeusMod with a new hook                  | [Hook Catalog](Hook-Catalog.md)                    |
| Resolve a new UPROPERTY / UFunction at runtime  | [Reflection Internals](Reflection-Internals.md)    |
| Speak the debug pipe protocol directly          | [Pipe Protocol](Pipe-Protocol.md)                  |
| Look up a known struct offset                   | [Memory Layout](Memory-Layout.md)                  |
| Diagnose a broken install or a crash            | [Troubleshooting](Troubleshooting.md)              |
| Understand how a release gets cut               | [Release Process](Release-Process.md)              |
| Contribute code or docs                         | [Contributing](Contributing.md)                    |
| Skim the jargon                                 | [Glossary](Glossary.md)                            |

---

## What is ZeusMod?

ZeusMod is a **single-player research trainer** for *Icarus*
(RocketWerkz, Unreal Engine 4.27). It is composed of four cooperating
components:

```
┌──────────────────────────────────────────────────────────────────────┐
│                              Operator                                │
└────────┬──────────────────────────────────────────────────────┬──────┘
         │                                                      │
         │ Electron UI                             Python REPL  │
         │ (toggle cards, Attach)                  (inspect.py) │
         ▼                                                      ▼
┌────────────────────┐   named pipe (ZeusModPipe)   ┌────────────────────┐
│  ZeusMod.exe       │◄─────────────────────────────┤  Icarus-Win64-...  │
│  (Electron +       │         JSON-ish wire        │  Shipping.exe      │
│   koffi injector)  │         format               │   + IcarusInternal │
└────────────────────┘                              │     .dll           │
                                                    └────────────────────┘
```

- **`IcarusInternal.dll`** is the payload — it runs inside the game
  process and does all the actual trainer work (reflection lookups,
  MinHook detours, ImGui overlay, named-pipe server).
- **`IcarusInjector.exe`** is a standalone CLI injector, kept around
  for headless use and debugging. The desktop app has its own native
  injector built in and does **not** invoke this one.
- **`ZeusMod.exe`** is an Electron desktop companion. It detects
  Icarus, injects the DLL through a `koffi`-based FFI binding, and
  forwards UI events to the running DLL over the pipe.
- **`scripts/inspect.py`** is the command-line debug client — an
  x64dbg-style memory explorer that talks to the same pipe.

Design principle: **reflection over byte signatures.** Instead of
hard-coding AOB patterns that break on every patch, ZeusMod resolves
`UFunction`s by reflection name and `UPROPERTY` offsets via the
`FField` chain at runtime. The great majority of Icarus patches do
not require a rebuild.

---

## Wiki map

- [Home](Home.md) — *you are here*
- [Getting Started](Getting-Started.md)
- [Architecture](Architecture.md)
- [Build From Source](Build-From-Source.md)
- [Desktop App](Desktop-App.md)
- [Feature Reference](Feature-Reference.md)
- [Hook Catalog](Hook-Catalog.md)
- [Reflection Internals](Reflection-Internals.md)
- [Pipe Protocol](Pipe-Protocol.md)
- [Debug Client](Debug-Client.md)
- [Memory Layout](Memory-Layout.md)
- [Troubleshooting](Troubleshooting.md)
- [Release Process](Release-Process.md)
- [Contributing](Contributing.md)
- [FAQ](FAQ.md)
- [Glossary](Glossary.md)

---

## License &amp; disclaimer

ZeusMod is **MIT-licensed** (see [LICENSE](../../LICENSE)) and provided
strictly for **educational and reverse-engineering research**. It is
not affiliated with, endorsed by, or sponsored by RocketWerkz. Use it
only in single-player or private prospects you control.

Author: **[CyberSnake](https://github.com/CyberSnakeH)**.
