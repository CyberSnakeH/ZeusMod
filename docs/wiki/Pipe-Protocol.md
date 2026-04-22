# Pipe Protocol

ZeusMod uses two Windows named pipes to bridge external clients
(Electron UI, `inspect.py`) with `IcarusInternal.dll`. This page is
the wire-format spec — what bytes go on the wire, what responses look
like, and how to extend it.

---

## The two pipes

| Pipe                             | Purpose                       | Default client          |
|----------------------------------|-------------------------------|-------------------------|
| `\\.\pipe\ZeusModPipe`           | Cheat toggles + config values | Electron launcher       |
| `\\.\pipe\ZeusModDbg`            | Reflection + memory access    | `scripts/inspect.py`    |

Both run the same server code in different threads, share the same
wire format, and differ only in which command vocabulary is
recognised.

---

## Transport

- **Transport** — `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE` on the
  server side. Clients write and read as UTF-8 byte arrays.
- **Framing** — one **message per request, one message per response**.
  Pipe message boundaries are the framing mechanism; there is no
  length prefix.
- **Encoding** — 7-bit ASCII for commands and args, UTF-8 for FName
  payloads returned in responses. Addresses are always rendered as
  uppercase `0x[0-9A-F]+`.

The pipe client in the Electron app is `node:net` with
`\\.\pipe\<name>` as the address. `inspect.py` uses `open("\\\\.\\pipe\\ZeusModDbg", "r+b")`
on Windows.

---

## Request format

### Cheats pipe

```
<cmd>:<value>
```

- `cmd` — one of the names listed in [Feature Reference](Feature-Reference.md#command-reference-flat-table).
- `value` — a token interpreted by the cheat handler.

Examples:

```
godmode:1
stamina:0
speed_mult:4.0
time_val:12
give:Wood,10
```

### Debug pipe

```
dbg:<cmd>:<arg0>:<arg1>:...:<argN>
```

- `cmd` — one of the primitives listed in [Debug Client](Debug-Client.md).
- Args are colon-separated. Hex addresses are bare (no `0x` prefix on
  the wire). Spaces are illegal in a single arg.

Examples:

```
dbg:classof:28F2A3AC010
dbg:read64:28F2A3AC010
dbg:dump:28F2A3AC010:40
dbg:propget:28F2A3AC010:Inventory:CurrentWeight:4
dbg:search:7FF640330000:1000:ascii:DOS
```

Inside the DLL, `HandleDbgRaw` receives `(cmd, args)` already split.

---

## Response format

Every response starts with one of three tokens, followed by a space
and a free-form payload. The payload is **always a single-message
write** and lines inside it are separated by `\n`.

| Prefix | Meaning                                                         |
|--------|-----------------------------------------------------------------|
| `OK `  | Success. Payload conveys the result.                            |
| `ERR ` | Failure. Payload is a short human-readable reason.              |
| `WARN `| Succeeded but partial. Rare.                                    |

Examples:

```
OK character=0x28F2A3AC010 Off::Player_InventoryComp=0x758

OK modules count=174
  0x00007FF640330000  size=0x06A62000  Icarus-Win64-Shipping.exe
  0x00007FFD05540000  size=0x00267000  ntdll.dll
  …

OK dump @0x28F2A3AC010 size=0x40
  +0x000  90 F0 E6 44 F6 7F 00 00  …   a.Name.o..
  +0x010  40 21 26 CB A1 01 00 00  …
  …

ERR unknown primitive 'modules'
ERR usage refs:<target>:<scanStart>:<range>
```

### Hex line format (shared by `dump`, `strings`, `refs`, `search`)

```
  +0x<offset>  <byte0> <byte1> <byte2> …  <ascii or tag>
```

The `inspect.py` formatter parses this against a regex so label
overlays and diffs can be layered on top without re-implementing a
hex dumper.

---

## Server-side dispatch (DLL)

```
pipe-server thread:
  ConnectNamedPipe
  loop:
    ReadFile → raw line
    if starts with "dbg:" → HandleDbgCommand
    else                  → HandleCheatCommand
    WriteFile ← response
    (loop)
```

`HandleDbgCommand` is the first touchpoint for the debug pipe. It
splits the line on the first `:` (after stripping `dbg:`), then
forwards `(cmd, args, out, outCap)` to `HandleDbgRaw`. The raw
handler is a giant `if (!strcmp(cmd, "…"))` chain — see
`native/internal/src/cheats/Trainer.cpp`.

Cheats share a similar dispatcher, but the handler is
`Trainer::OnCheatChange(cmd, value)` which simply writes the matching
bool or value-field.

### Concurrency

- Per-client thread on the server side — one per accepted pipe
  handle. In practice both clients each hold a single connection.
- The trainer tick runs on its own thread. UPROPERTY writes from the
  debug pipe and cheat-toggle writes are **not synchronised** against
  the trainer tick — writes are small (≤8 bytes) and naturally
  atomic on x64, and game-state reads inside the tick are defensive.

---

## Response size limits

- Cheat pipe: 256 bytes. Cheat responses are short.
- Debug pipe: 64 KiB per single response. `dump` tops out at
  `0xC000` bytes (48 KiB), leaving room for the formatting overhead.

Anything longer is truncated with `…(truncated)`. In practice this
never triggers outside memory dumps against a huge range, which
is exactly what `inspect.py`'s chunk-walking composites are for.

---

## Extending the protocol

Adding a new debug command:

1. In `Trainer.cpp`, inside `HandleDbgRaw`, add a new
   `if (!strcmp(cmd, "myprim")) { … }` block and emit an `OK …`
   response.
2. In `scripts/inspect.py`, register the command in the `COMMANDS`
   list with the correct `group` (`Scanner`, `Memory`, whatever
   fits) so `DBG_PRIMITIVES` picks it up (the gate that lets
   `primitive_send` forward the command over the wire instead of
   rejecting it client-side).
3. (Optional) Add a composite wrapper if the command benefits from
   client-side orchestration (label overlays, argument defaulting).
4. Document it in [Debug Client](Debug-Client.md).

Adding a new cheat toggle:

1. Add a bool field on `Trainer` and wire it into the tick.
2. In `HandleCheatCommand`, accept the new token.
3. On the Electron side, add a card in
   `app/src/renderer/index.html` with
   `data-cheat-toggle="mycheat"`. The renderer forwards it to the
   pipe automatically — no additional JS wiring required.

---

## See also

- [Debug Client](Debug-Client.md) — the Python client that speaks this protocol.
- [Hook Catalog](Hook-Catalog.md) — cheats and their hooks.
- [Architecture](Architecture.md) — where the pipes fit in the bigger picture.
