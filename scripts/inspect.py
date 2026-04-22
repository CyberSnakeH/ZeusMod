#!/usr/bin/env python3
# ============================================================================
# ZeusMod — Icarus live inspector / debug REPL
#
# A polished single-file CLI that talks to the injected DLL over the named
# pipe `\\.\pipe\ZeusModPipe`. Provides three modes of operation:
#
#   REPL           python inspect.py                          (readline, history)
#   one-shot       python inspect.py "classof 0x..."
#   batch          python inspect.py -c "character = c; read64 $c+0x758"
#   scripted       python inspect.py --script recipe.zm       (semicolon/newline)
#   watch          python inspect.py --watch "listbag Backpack" 1.0
#   JSON output    python inspect.py --json ...               (machine-readable)
#
# Every typed argument goes through an expression evaluator that understands
# `0xHEX`, bare hex, `$prev`, `$<name>`, and arithmetic (`$char + 0x758`),
# so chains stay readable and variables persist inside batches/scripts.
#
# Streams are coloured when the terminal supports it (falls back gracefully);
# hex dumps render xxd-style with ASCII sidebars; `props`/`listitems` come
# back as aligned tables. The REPL has tab-completion on commands *and*
# $variables, plus a persistent ~/.zeusmod_history file.
#
# DESIGN NOTES
#   - Zero third-party deps required. `rich` is used opportunistically for
#     nicer tables / hex coloring if installed, but the script is fully
#     functional on a vanilla Python install.
#   - All DLL primitives are declared in COMMANDS as (name, desc, usage,
#     example). `help <name>` pulls from this single source of truth.
#   - Composites live in COMPOSITES dict; they build on `primitive_send()`
#     so every byte on the wire stays auditable.
# ============================================================================

from __future__ import annotations

# ── stdlib shadowing guard ──────────────────────────────────────────────
# This file is named `inspect.py` for historical / muscle-memory reasons,
# but its directory would otherwise be inserted as sys.path[0] by the Python
# launcher and shadow the real `inspect` stdlib module (imported indirectly
# by `dataclasses` and several other stdlib modules). Strip our own dir
# from sys.path before any further imports.
import os as _os, sys as _sys
_THIS_DIR = _os.path.dirname(_os.path.abspath(__file__)) if "__file__" in globals() else ""
if _THIS_DIR:
    _sys.path[:] = [p for p in _sys.path if _os.path.abspath(p) != _THIS_DIR]

# Force UTF-8 on stdout so our pretty Unicode glyphs (▸ ╔ │ etc.) render on
# Windows consoles that default to cp1252. Safe no-op on POSIX.
try:
    _sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    _sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import argparse
import ctypes
import difflib
import json
import os
import re
import shlex
import struct
import sys
import time
import uuid
from ctypes import wintypes
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

# Optional capstone for disasm
try:
    import capstone as _cs  # type: ignore
    _HAS_CAPSTONE = True
except Exception:
    _HAS_CAPSTONE = False

# ─── Optional `rich` for pretty output ──────────────────────────────────
try:
    from rich.console import Console
    from rich.table import Table
    from rich.text import Text
    _HAS_RICH = True
    _console: "Console | None" = Console(highlight=False, soft_wrap=False)
except Exception:
    _HAS_RICH = False
    _console = None

# ─── ANSI fallback ──────────────────────────────────────────────────────
# The fallback pipeline works without rich. Colors are stripped when stdout
# is not a TTY (scripts piping output stay clean).
_TTY = sys.stdout.isatty()

class _C:
    """ANSI shortcuts with a TTY guard so piping to a file stays clean."""
    RESET = "\x1b[0m"  if _TTY else ""
    DIM   = "\x1b[2m"  if _TTY else ""
    BOLD  = "\x1b[1m"  if _TTY else ""
    RED   = "\x1b[91m" if _TTY else ""
    GREEN = "\x1b[92m" if _TTY else ""
    YEL   = "\x1b[93m" if _TTY else ""
    BLUE  = "\x1b[94m" if _TTY else ""
    MAG   = "\x1b[95m" if _TTY else ""
    CYAN  = "\x1b[96m" if _TTY else ""
    WHITE = "\x1b[97m" if _TTY else ""
    GRAY  = "\x1b[90m" if _TTY else ""

# ─── Windows pipe I/O (no pywin32 dependency) ───────────────────────────
PIPE_NAME = r"\\.\pipe\ZeusModPipe"

GENERIC_READ          = 0x80000000
GENERIC_WRITE         = 0x40000000
OPEN_EXISTING         = 3
PIPE_READMODE_MESSAGE = 2
INVALID_HANDLE_VALUE  = ctypes.c_void_p(-1).value

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
_k32.CreateFileW.restype  = wintypes.HANDLE
_k32.WriteFile.argtypes   = [wintypes.HANDLE, ctypes.c_char_p, wintypes.DWORD,
                             ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_k32.WriteFile.restype    = wintypes.BOOL
_k32.ReadFile.argtypes    = [wintypes.HANDLE, ctypes.c_char_p, wintypes.DWORD,
                             ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_k32.ReadFile.restype     = wintypes.BOOL
_k32.SetNamedPipeHandleState.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                                         ctypes.c_void_p, ctypes.c_void_p]
_k32.SetNamedPipeHandleState.restype  = wintypes.BOOL
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype  = wintypes.BOOL


def _raw_send(cmd: str) -> str:
    """Open, write, read, close — with retry on the single-instance pipe
    recreation window (err 2 / err 231). Returns the decoded response."""
    last_err = 0
    h = INVALID_HANDLE_VALUE
    for _ in range(40):
        h = _k32.CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, None,
                             OPEN_EXISTING, 0, None)
        if h and h != INVALID_HANDLE_VALUE:
            break
        last_err = ctypes.get_last_error()
        if last_err not in (2, 231):  # anything else = real error
            break
        time.sleep(0.01)
    if not h or h == INVALID_HANDLE_VALUE:
        raise ConnectionError(
            f"pipe {PIPE_NAME} unreachable (err {last_err}) — "
            "is Icarus running with ZeusMod injected?")
    mode = wintypes.DWORD(PIPE_READMODE_MESSAGE)
    _k32.SetNamedPipeHandleState(h, ctypes.byref(mode), None, None)
    try:
        data = cmd.encode("utf-8", errors="replace")
        written = wintypes.DWORD(0)
        if not _k32.WriteFile(h, data, len(data), ctypes.byref(written), None):
            return f"ERR write failed ({ctypes.get_last_error()})"
        buf = ctypes.create_string_buffer(0x10000)
        read = wintypes.DWORD(0)
        if not _k32.ReadFile(h, buf, len(buf), ctypes.byref(read), None):
            return f"ERR read failed ({ctypes.get_last_error()})"
        return buf.raw[:read.value].decode("utf-8", errors="replace")
    finally:
        _k32.CloseHandle(h)


# ─── Variable store + expression evaluator ──────────────────────────────

class VarStore:
    """Single-scope variable store used by the REPL, batch and scripts.
    Supports `$prev` (last primary hex result) and `$<name>` (saved via the
    `cmd = name` suffix). `resolve()` accepts bare hex, 0xHEX, decimal, or
    full arithmetic expressions combining all of the above."""

    def __init__(self) -> None:
        self.vars: Dict[str, int] = {}
        self.prev: Optional[int] = None

    def set(self, name: str, value: int) -> None:
        self.vars[name] = value

    def resolve(self, token: str) -> int:
        expr = token.strip()

        def subst(m: re.Match) -> str:
            name = m.group(1)
            if name == "prev":
                if self.prev is None:
                    raise ValueError("$prev not set")
                return hex(self.prev)
            if name not in self.vars:
                raise ValueError(f"${name} not defined")
            return hex(self.vars[name])
        expr = re.sub(r"\$([A-Za-z_][A-Za-z0-9_]*)", subst, expr)

        # Fast paths
        if re.match(r"^[0-9A-Fa-f]+$", expr) and any(c in "ABCDEFabcdef" for c in expr):
            return int(expr, 16)
        if re.match(r"^\d+$", expr):
            return int(expr, 10)

        # Normalise bare hex inside mixed arithmetic (e.g. "132515DAAC0+0x10")
        expr = re.sub(
            r"(?<![\w0x])([0-9A-Fa-f]+)(?![\w])",
            lambda m: ("0x" + m.group(1))
                      if any(c in "ABCDEFabcdef" for c in m.group(1)) or len(m.group(1)) > 10
                      else m.group(1),
            expr)
        if not re.match(r"^[0-9A-Fa-fx\+\-\*\/\(\)\s]+$", expr):
            raise ValueError(f"bad expression: {token!r}")
        return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307

    def resolve_hex(self, token: str) -> str:
        return f"{self.resolve(token):X}"


# ─── Command catalog ────────────────────────────────────────────────────

@dataclass
class Cmd:
    name:     str
    group:    str
    desc:     str
    usage:    str
    example:  str

COMMANDS: List[Cmd] = [
    # Lookup / discovery
    Cmd("classof",   "Lookup", "UClass name of the UObject at <addr>.",              "classof <addr>",             "classof 0x153D77A6580"),
    Cmd("nameof",    "Lookup", "FName at <addr>+0x18 (UObject's own name).",        "nameof <addr>",              "nameof 0x153D77A6580"),
    Cmd("findcls",   "Lookup", "UClass address by exact class name.",                "findcls <name>",             "findcls Inventory"),
    Cmd("findstruct","Lookup", "UClass OR UScriptStruct address by name.",           "findstruct <name>",          "findstruct ItemData"),
    Cmd("findobj",   "Lookup", "First live non-CDO instance of <className>.",        "findobj <className>",        "findobj BP_IcarusPlayerCharacterSurvival_C"),
    Cmd("findname",  "Lookup", "First instance whose name matches a substring.",     "findname <class>:<substr>",  "findname Function:AddItem"),
    Cmd("listobj",   "Lookup", "List up to N live instances of <className>.",        "listobj <class>:<N>",        "listobj Inventory 10"),
    Cmd("getbyindex","Lookup", "Return GObjects[idx] (for FWeakObjectPtr lookup).",  "getbyindex <idxHex>",        "getbyindex 1DD5F"),
    Cmd("outer",     "Lookup", "UObject outer chain (up to 8 levels).",              "outer <addr>",               "outer 0x153D77A6580"),
    Cmd("isa",       "Lookup", "Walk the Super chain for a class match.",            "isa <addr>:<className>",     "isa 0x... Inventory"),
    Cmd("fname",     "Lookup", "Resolve an FName ComparisonIndex to a string.",      "fname <idxHex>",             "fname 0x907797"),
    Cmd("fnameof",   "Lookup", "Read the FName whose 8 bytes sit at <addr>.",        "fnameof <addr>",             "fnameof 0x..."),

    # Reflection
    Cmd("props",     "Reflection", "Own UPROPERTYs of a class/struct.",              "props <name>",               "props Inventory"),
    Cmd("propsall",  "Reflection", "props + walk Super classes.",                    "propsall <name>",            "propsall InventoryComponent"),
    Cmd("propoff",   "Reflection", "Single property offset (Super walk).",           "propoff <class> <prop>",     "propoff Inventory Slots"),
    Cmd("listfuncs", "Reflection", "UFunction children of a class.",                 "listfuncs <name>",           "listfuncs Inventory"),
    Cmd("funcoff",   "Reflection", "UFunction address by class:func.",               "funcoff <class>:<func>",     "funcoff IcarusController:OnServer_AddItem"),
    Cmd("propget",   "Reflection", "Typed read: <obj>:<class>:<field>[:<bytes>].",   "propget <obj>:<cls>:<field>:[bytes]", "propget 0x... Inventory CurrentWeight 4"),
    Cmd("propset",   "Reflection", "Typed write: <obj>:<class>:<field>:<hex>.",      "propset <obj>:<cls>:<field>:<hex>",   "propset 0x... Inventory CurrentWeight 00000000"),
    Cmd("callfn",    "Reflection", "Invoke a UFunction via ProcessEvent.",           "callfn <obj>:<cls>:<fn>[:<hex>]",     "callfn 0x... IcarusController OnServer_AddItem:..."),

    # Memory I/O
    Cmd("read8",    "Memory", "1-byte read.",                                  "read8 <addr>",                "read8 $char+0x100"),
    Cmd("read32",   "Memory", "4-byte read (u32 / float).",                    "read32 <addr>",               "read32 $char+0x758"),
    Cmd("read64",   "Memory", "8-byte read (ptr / u64).",                      "read64 <addr>",               "read64 $char+0x758"),
    Cmd("dump",     "Memory", "Hex dump, size ≤ 0xC000.",                      "dump <addr> <size>",          "dump $char 0x100"),
    Cmd("scan",     "Memory", "Detect TArray {ptr,num,max} in a range.",       "scan <addr> <range>",         "scan $char+0xF0 0x160"),
    Cmd("pattern",  "Memory", "Find byte sequences in a range.",               "pattern <addr>:<range>:<hex>","pattern 0x7FF... 0x10000 5DDD0100C63A0000"),
    Cmd("vtable",   "Memory", "Read vtable[idx] of a UObject.",                "vtable <addr>:<idx>",         "vtable 0x... 68"),
    Cmd("module",   "Memory", "Module base + size (empty = main exe).",        "module [name]",               "module Icarus-Win64-Shipping.exe"),

    Cmd("write8",   "Write",  "Write 1 byte.",                                 "write8 <addr>:<hex>",         "write8 0x... 01"),
    Cmd("write32",  "Write",  "Write 4 bytes.",                                "write32 <addr>:<hex>",        "write32 0x... 00000000"),
    Cmd("write64",  "Write",  "Write 8 bytes.",                                "write64 <addr>:<hex>",        "write64 0x... 0000000000000000"),
    Cmd("wbytes",   "Write",  "Write a raw hex blob.",                         "wbytes <addr>:<hex>",         "wbytes 0x... DEADBEEFCAFE"),

    # Player / inventory helpers
    Cmd("character", "Player", "Player pawn pointer + InvComp offset.",        "character",                   "character"),
    Cmd("playerinv", "Player", "Walk InvComp, list UInventory candidates.",    "playerinv",                   "playerinv"),
    Cmd("inv",       "Player", "Player inventories by name + slot counts.",   "inv",                         "inv"),
    Cmd("listitems", "Player", "Slot-by-slot row name + stack count.",        "listitems <bagAddr>",         "listitems $bp"),
    Cmd("dumpslot",  "Player", "Detailed FItemData dump for one slot.",       "dumpslot <bag>:<idx>",        "dumpslot $bp 0"),

    # Scanner (x64dbg-tier) ─ needs 22:10+ DLL
    Cmd("memmap",    "Scanner", "VirtualQueryEx walker. Empty = full AS scan.", "memmap [<start>] [<range>]", "memmap 0x1A1B0000000 0x1000000"),
    Cmd("modules",   "Scanner", "EnumProcessModules list (base + size + path).", "modules",                   "modules"),
    Cmd("strings",   "Scanner", "ASCII + UTF-16 scanner over a memory range.",   "strings <addr> <range> [<minLen>]", "strings $bp 0x1000 6"),
    Cmd("refs",      "Scanner", "Find 8-byte-aligned pointers to <target>.",     "refs <target> <scanStart> <range>", "refs $char $bp 0x1000"),
    Cmd("search",    "Scanner", "Typed memory search (u32/u64/f32/f64/ascii/utf16).", "search <addr> <range> <type> <value>", "search $bp 0x1000 u64 0xD78A"),

    # Cheats
    Cmd("godmode",   "Cheats", "Toggle God Mode.",                             "godmode <0|1>",               "godmode 1"),
    Cmd("stamina",   "Cheats", "Infinite Stamina.",                            "stamina <0|1>",               "stamina 1"),
    Cmd("armor",     "Cheats", "Infinite Armor.",                              "armor <0|1>",                 "armor 1"),
    Cmd("oxygen",    "Cheats", "Infinite Oxygen.",                             "oxygen <0|1>",                "oxygen 1"),
    Cmd("food",      "Cheats", "Infinite Food.",                               "food <0|1>",                  "food 1"),
    Cmd("water",     "Cheats", "Infinite Water.",                              "water <0|1>",                 "water 1"),
    Cmd("craft",     "Cheats", "Free Craft.",                                  "craft <0|1>",                 "craft 1"),
    Cmd("items",     "Cheats", "Infinite Items + durability lock.",            "items <0|1>",                 "items 1"),
    Cmd("weight",    "Cheats", "No Weight (AddModifierState hook).",           "weight <0|1>",                "weight 1"),
    Cmd("speed",     "Cheats", "Speed Hack.",                                  "speed <0|1>",                 "speed 1"),
    Cmd("speed_mult","Cheats", "Speed multiplier (float).",                    "speed_mult <float>",          "speed_mult 4.0"),
    Cmd("time",      "Cheats", "Lock time of day.",                            "time <0|1>",                  "time 1"),
    Cmd("time_val",  "Cheats", "Time value (0-24).",                           "time_val <hour>",             "time_val 12"),
    Cmd("temp",      "Cheats", "Stable temperature.",                          "temp <0|1>",                  "temp 1"),
    Cmd("temp_val",  "Cheats", "Target temperature.",                          "temp_val <degC>",             "temp_val 20"),
    Cmd("megaexp",   "Cheats", "Mega XP.",                                     "megaexp <0|1>",               "megaexp 1"),
    Cmd("talent",    "Cheats", "Max talent points.",                           "talent <0|1>",                "talent 1"),
    Cmd("tech",      "Cheats", "Max tech points.",                             "tech <0|1>",                  "tech 1"),
    Cmd("solo",      "Cheats", "Max solo points.",                             "solo <0|1>",                  "solo 1"),
    Cmd("give",      "Cheats", "Give item by row name.",                       "give <RowName>,<count>",      "give Wood,10"),
]

# Primitive name sets / arg-encoding rules ──────────────────────────────

DBG_PRIMITIVES = {c.name for c in COMMANDS if c.group in
                  ("Lookup","Reflection","Memory","Write","Player","Scanner")}

CHEAT_COMMANDS = {c.name for c in COMMANDS if c.group == "Cheats"} | {"god"}

# Commands whose argv positions contain addresses that must be expression-
# evaluated (via VarStore) before being shipped out. Keep this aligned with
# the DLL's HandleDbgCommand signatures.
HEX_ARGS: Dict[str, List[int]] = {
    "classof":[0], "nameof":[0], "outer":[0], "fnameof":[0], "fname":[0],
    "read8":[0], "read32":[0], "read64":[0],
    "dump":[0, 1], "scan":[0, 1], "pattern":[0, 1],
    "write8":[0, 1], "write32":[0, 1], "write64":[0, 1], "wbytes":[0],
    "vtable":[0], "propget":[0], "propset":[0], "callfn":[0],
    "listitems":[0], "dumpslot":[0], "isa":[0], "getbyindex":[0],
}


def primitive_send(cmd: str, args: List[str], vars_: VarStore) -> str:
    """Resolve expression args, build the wire-format string, send it, return
    the raw DLL response. Caller is responsible for parsing."""
    if cmd in HEX_ARGS:
        for idx in HEX_ARGS[cmd]:
            if idx < len(args):
                args[idx] = vars_.resolve_hex(args[idx])
    if cmd == "god":
        cmd = "godmode"
    if cmd in DBG_PRIMITIVES:
        wire = "dbg:" + cmd + (":" + ":".join(args) if args else ":")
    elif cmd in CHEAT_COMMANDS:
        val = args[0] if args else "1"
        wire = cmd + ":" + val
    else:
        return f"ERR unknown primitive '{cmd}'"
    return _raw_send(wire)


# ─── Composites (client-side orchestration) ─────────────────────────────

_HEX_TOKEN_RE = re.compile(r"0x([0-9A-Fa-f]+)")

def _extract_hex(resp: str) -> Optional[int]:
    m = _HEX_TOKEN_RE.search(resp)
    return int(m.group(1), 16) if m else None

def _extract_ok_token(resp: str) -> str:
    resp = resp.strip()
    if not resp.startswith("OK "):
        return ""
    rest = resp[3:].strip()
    return rest.split()[0] if rest else ""


def composite_follow(args, vars_):
    if not args:
        return "ERR usage: follow <addr> [+offN...]"
    addr = vars_.resolve(args[0])
    steps = [addr]
    for token in args[1:]:
        off = vars_.resolve(token)
        addr = addr + off
        v = _extract_hex(primitive_send("read64", [f"{addr:X}"], vars_))
        if v is None:
            return f"ERR bad read at step {len(steps)}"
        steps.append(v)
        addr = v
    cls = primitive_send("classof", [f"{addr:X}"], vars_).strip()
    vars_.prev = addr
    out = [f"OK final=0x{addr:X} {cls}"]
    for i, s in enumerate(steps):
        out.append(f"  step[{i}] = 0x{s:X}")
    return "\n".join(out)


def composite_obj(args, vars_):
    if not args:
        return "ERR usage: obj <addr>"
    addr = vars_.resolve(args[0])
    out = [f"OK obj 0x{addr:X}"]
    current = addr
    for lvl in range(5):
        cls  = _extract_ok_token(primitive_send("classof", [f"{current:X}"], vars_))
        name = _extract_ok_token(primitive_send("nameof",  [f"{current:X}"], vars_))
        out.append(f"  [{lvl}] 0x{current:X}  {cls} / {name}")
        outer = _extract_hex(primitive_send("read64", [f"{current + 0x20:X}"], vars_))
        if not outer or outer == current:
            break
        current = outer
    vars_.prev = addr
    return "\n".join(out)


def composite_finduobj(args, vars_):
    if not args:
        return "ERR usage: finduobj <className>"
    addr = _extract_hex(primitive_send("findobj", args, vars_))
    if not addr:
        return "ERR findobj returned no match"
    vars_.prev = addr
    return composite_obj([f"0x{addr:X}"], vars_)


def composite_invtest(args, vars_):
    if not args:
        return "ERR usage: invtest <addr>"
    addr = vars_.resolve(args[0])
    out = [f"OK invtest 0x{addr:X}"]
    cls  = _extract_ok_token(primitive_send("classof", [f"{addr:X}"], vars_))
    name = _extract_ok_token(primitive_send("nameof",  [f"{addr:X}"], vars_))
    out.append(f"  {cls} / {name}")
    out.append("  -- outer chain --")
    current = addr
    for level in range(4):
        outer = _extract_hex(primitive_send("read64", [f"{current + 0x20:X}"], vars_))
        if not outer or outer == current:
            break
        current = outer
        ocls  = _extract_ok_token(primitive_send("classof", [f"{current:X}"], vars_))
        oname = _extract_ok_token(primitive_send("nameof",  [f"{current:X}"], vars_))
        out.append(f"  outer[{level+1}] 0x{current:X}  {ocls} / {oname}")
    out.append("  -- TArray scan (0x30..0x300) --")
    out.append(primitive_send("scan", [f"{addr:X}", "300"], vars_))
    vars_.prev = addr
    return "\n".join(out)


def composite_findplayerinv(args, vars_):
    resp = primitive_send("character", [], vars_)
    char = _extract_hex(resp)
    if not char:
        return f"ERR no character ({resp.strip()})"
    m = re.search(r"Off::Player_InventoryComp=0x([0-9A-Fa-f]+)", resp)
    comp_off = int(m.group(1), 16) if m else 0x758
    comp = _extract_hex(primitive_send("read64", [f"{char + comp_off:X}"], vars_))
    if not comp:
        return "ERR bad InvComp read"
    out = [f"OK character=0x{char:X}  comp=0x{comp:X}"]
    candidates: List[Tuple[int,int]] = []
    for off in range(0x30, 0x300, 8):
        v = _extract_hex(primitive_send("read64", [f"{comp + off:X}"], vars_))
        if not v or v < 0x10000 or v > 0x00007FFFFFFFFFFF:
            continue
        cls = _extract_ok_token(primitive_send("classof", [f"{v:X}"], vars_))
        if cls != "Inventory":
            continue
        candidates.append((off, v))
    out.append(f"  {len(candidates)} UInventory candidates in InvComp")
    for off, v in candidates:
        cls  = _extract_ok_token(primitive_send("classof", [f"{v:X}"], vars_))
        name = _extract_ok_token(primitive_send("nameof",  [f"{v:X}"], vars_))
        current = v; reached = False; chain = []
        for _ in range(6):
            outer = _extract_hex(primitive_send("read64", [f"{current + 0x20:X}"], vars_))
            if not outer or outer == current:
                break
            chain.append(outer); current = outer
            if current == char: reached = True; break
        marker = "  [OWNS PLAYER]" if reached else ""
        out.append(f"  +0x{off:03X} -> 0x{v:X}  {cls} / {name}{marker}")
        for i, c in enumerate(chain):
            ccls = _extract_ok_token(primitive_send("classof", [f"{c:X}"], vars_))
            cname= _extract_ok_token(primitive_send("nameof",  [f"{c:X}"], vars_))
            out.append(f"      outer[{i}] 0x{c:X}  {ccls} / {cname}")
    vars_.prev = comp
    return "\n".join(out)


def composite_propswalk(args, vars_):
    if not args:
        return "ERR usage: propswalk <structAddr>"
    addr = vars_.resolve(args[0])
    field = _extract_hex(primitive_send("read64", [f"{addr + 0x50:X}"], vars_))
    if not field:
        return f"OK propswalk 0x{addr:X}\n  (empty ChildProperties at +0x50)"
    out = [f"OK propswalk 0x{addr:X}"]
    seen: set = set()
    safety = 200
    while field and field not in seen and safety > 0:
        seen.add(field)
        name = _extract_ok_token(primitive_send("nameof", [f"{field + 0x10:X}"], vars_))
        ro = primitive_send("read32", [f"{field + 0x4C:X}"], vars_)
        mm = re.search(r"\((-?\d+)\)", ro)
        off = int(mm.group(1)) if mm else -1
        out.append(f"  +0x{off & 0xFFFF:03X}  {name}")
        field = _extract_hex(primitive_send("read64", [f"{field + 0x20:X}"], vars_))
        safety -= 1
    vars_.prev = addr
    return "\n".join(out)


def composite_bag(args, vars_):
    target = (args[0] if args else "Backpack").strip()
    resp = primitive_send("inv", [], vars_)
    for line in resp.splitlines():
        m = re.search(r"\[\d+\]\s+(\S+)\s+bag=0x([0-9A-Fa-f]+)\s+(\d+)/(\d+)", line)
        if m and m.group(1).lower() == target.lower():
            a = int(m.group(2), 16)
            vars_.prev = a
            return f"OK {m.group(1)} bag=0x{a:X} slots={m.group(3)}/{m.group(4)}"
    return f"ERR bag '{target}' not found\n{resp}"


def composite_listbag(args, vars_):
    b = composite_bag(args, vars_)
    if b.startswith("ERR"):
        return b
    addr = vars_.prev or 0
    items = primitive_send("listitems", [f"{addr:X}"], vars_)
    return f"{b}\n{items}"


def composite_setweightzero(args, vars_):
    resp = primitive_send("inv", [], vars_)
    out = []
    for line in resp.splitlines():
        m = re.search(r"\[\d+\]\s+(\S+)\s+bag=0x([0-9A-Fa-f]+)", line)
        if not m:
            continue
        bag = int(m.group(2), 16)
        r = primitive_send("write32", [f"{bag + 0xE8:X}", "0"], vars_)
        out.append(f"{m.group(1)}:0x{bag:X} -> {r.strip()}")
    return "OK setweightzero:\n  " + "\n  ".join(out)


def composite_diag(args, vars_):
    out = []
    out.append(primitive_send("character", [], vars_).strip())
    out.append(primitive_send("inv",       [], vars_).strip())
    b = composite_bag(["Backpack"], vars_)
    if b.startswith("OK"):
        addr = vars_.prev or 0
        out.append(primitive_send("listitems", [f"{addr:X}"], vars_).strip())
    return "\n\n".join(out)


def composite_getprop(args, vars_):
    if len(args) < 3:
        return "ERR usage: getprop <addr> <class> <field> [<bytes>]"
    addr = vars_.resolve(args[0])
    a = [f"{addr:X}", args[1], args[2]]
    if len(args) >= 4: a.append(args[3])
    return primitive_send("propget", a, vars_)


def composite_setprop(args, vars_):
    if len(args) < 4:
        return "ERR usage: setprop <addr> <class> <field> <hexvalue>"
    addr = vars_.resolve(args[0])
    return primitive_send("propset", [f"{addr:X}", args[1], args[2], args[3]], vars_)


# ═════════════════════════════════════════════════════════════════════════
# x64dbg-tier additions — typed readers, labels, bookmarks, snapshots,
# struct viewer, disasm. All composites; they build on primitive_send so
# every byte still flows through the single pipe audit path.
# ═════════════════════════════════════════════════════════════════════════

# ── Typed readers ────────────────────────────────────────────────────────
# Wrap existing read32/read64 responses and reinterpret them as float,
# double, UTF-8/UTF-16 strings, or an FGuid. Makes data-type interpretation
# (x64dbg's right-click "View as…") trivial from the REPL.

def _read32_u32(addr_tok: str, vars_: VarStore) -> Optional[int]:
    r = primitive_send("read32", [vars_.resolve_hex(addr_tok)], vars_)
    m = _HEX_TOKEN_RE.search(r)
    return int(m.group(1), 16) if m else None

def _read64_u64(addr_tok: str, vars_: VarStore) -> Optional[int]:
    r = primitive_send("read64", [vars_.resolve_hex(addr_tok)], vars_)
    m = _HEX_TOKEN_RE.search(r)
    return int(m.group(1), 16) if m else None

def composite_readf32(args, vars_):
    if not args: return "ERR usage: readf32 <addr>"
    u = _read32_u32(args[0], vars_)
    if u is None: return "ERR read failed"
    f = struct.unpack("<f", struct.pack("<I", u))[0]
    return f"OK 0x{u:08X}  float={f:g}"

def composite_readf64(args, vars_):
    if not args: return "ERR usage: readf64 <addr>"
    u = _read64_u64(args[0], vars_)
    if u is None: return "ERR read failed"
    f = struct.unpack("<d", struct.pack("<Q", u))[0]
    return f"OK 0x{u:016X}  double={f:g}"

def composite_readi32(args, vars_):
    if not args: return "ERR usage: readi32 <addr>"
    u = _read32_u32(args[0], vars_)
    if u is None: return "ERR read failed"
    signed = u - (1 << 32) if u & 0x80000000 else u
    return f"OK 0x{u:08X}  int32={signed}"

def composite_readi64(args, vars_):
    if not args: return "ERR usage: readi64 <addr>"
    u = _read64_u64(args[0], vars_)
    if u is None: return "ERR read failed"
    signed = u - (1 << 64) if u & (1 << 63) else u
    return f"OK 0x{u:016X}  int64={signed}"

def composite_readstr(args, vars_):
    """Read a NUL-terminated ASCII/UTF-8 string (up to <maxLen> bytes)."""
    if not args: return "ERR usage: readstr <addr> [maxLen=64]"
    addr = vars_.resolve(args[0])
    max_len = vars_.resolve(args[1]) if len(args) > 1 else 64
    size = min(max_len, 0x400)
    r = primitive_send("dump", [f"{addr:X}", f"{size:X}"], vars_)
    # Parse hex bytes from the dump output.
    bytes_ = bytearray()
    for ln in r.splitlines():
        m = re.match(r"\+0x[0-9A-Fa-f]+\s+((?:[0-9A-Fa-f]{2}\s+)+)", ln)
        if not m: continue
        for tok in m.group(1).split():
            bytes_.append(int(tok, 16))
    if not bytes_: return "ERR no bytes read"
    end = bytes_.find(0)
    payload = bytes_[:end] if end >= 0 else bytes_
    try:
        s = payload.decode("utf-8", errors="replace")
    except Exception:
        s = repr(payload)
    return f"OK @0x{addr:X}  len={len(payload)}  str={s!r}"

def composite_readunicode(args, vars_):
    """Read a NUL-terminated UTF-16 (LE) string."""
    if not args: return "ERR usage: readunicode <addr> [maxChars=64]"
    addr = vars_.resolve(args[0])
    max_chars = vars_.resolve(args[1]) if len(args) > 1 else 64
    size = min(max_chars * 2, 0x800)
    r = primitive_send("dump", [f"{addr:X}", f"{size:X}"], vars_)
    bytes_ = bytearray()
    for ln in r.splitlines():
        m = re.match(r"\+0x[0-9A-Fa-f]+\s+((?:[0-9A-Fa-f]{2}\s+)+)", ln)
        if not m: continue
        for tok in m.group(1).split():
            bytes_.append(int(tok, 16))
    # Parse until 00 00 word
    end = len(bytes_)
    for i in range(0, len(bytes_) - 1, 2):
        if bytes_[i] == 0 and bytes_[i+1] == 0:
            end = i; break
    payload = bytes(bytes_[:end])
    s = payload.decode("utf-16-le", errors="replace")
    return f"OK @0x{addr:X}  chars={len(s)}  str={s!r}"

def composite_readguid(args, vars_):
    """Read a 16-byte FGuid (little-endian Windows layout)."""
    if not args: return "ERR usage: readguid <addr>"
    addr = vars_.resolve(args[0])
    r = primitive_send("dump", [f"{addr:X}", "10"], vars_)
    bytes_ = bytearray()
    for ln in r.splitlines():
        m = re.match(r"\+0x[0-9A-Fa-f]+\s+((?:[0-9A-Fa-f]{2}\s+)+)", ln)
        if not m: continue
        for tok in m.group(1).split():
            bytes_.append(int(tok, 16))
    if len(bytes_) < 16: return "ERR short read"
    g = uuid.UUID(bytes_le=bytes(bytes_[:16]))
    return f"OK @0x{addr:X}  guid={{{g}}}"

def composite_readptrarr(args, vars_):
    """Read N consecutive pointers. Useful for TArray<T*> buffers."""
    if len(args) < 2: return "ERR usage: readptrarr <addr> <count>"
    addr = vars_.resolve(args[0]); n = vars_.resolve(args[1])
    if n <= 0 or n > 256: return "ERR bad count (1..256)"
    out = [f"OK {n} pointers @0x{addr:X}"]
    for i in range(n):
        u = _read64_u64(f"{addr + i*8:X}", vars_)
        if u is None:
            out.append(f"  [{i:>3}]  ERR"); continue
        out.append(f"  [{i:>3}]  0x{u:016X}")
    return "\n".join(out)


# ── Labels & bookmarks ──────────────────────────────────────────────────
# Named annotations on addresses, persisted to ~/.zeusmod_labels.json.
# Labels survive sessions and get overlaid on dumps/results. Bookmarks
# are a lighter variant with an optional note.

_LABELS_PATH = os.path.expanduser("~/.zeusmod_labels.json")

@dataclass
class _LabelStore:
    labels:    Dict[str, str] = field(default_factory=dict)  # "0xADDR" -> name
    bookmarks: Dict[str, str] = field(default_factory=dict)  # name -> "0xADDR" (+ optional note after ' | ')

    def save(self) -> None:
        try:
            with open(_LABELS_PATH, "w", encoding="utf-8") as f:
                json.dump({"labels": self.labels, "bookmarks": self.bookmarks}, f, indent=2)
        except Exception:
            pass

    @classmethod
    def load(cls) -> "_LabelStore":
        s = cls()
        if os.path.exists(_LABELS_PATH):
            try:
                with open(_LABELS_PATH, "r", encoding="utf-8") as f:
                    data = json.load(f) or {}
                s.labels    = dict(data.get("labels", {}))
                s.bookmarks = dict(data.get("bookmarks", {}))
            except Exception:
                pass
        return s

_LABELS = _LabelStore.load()

def composite_label(args, vars_):
    """label <addr> <name>  — tag an address with a name."""
    if len(args) < 2: return "ERR usage: label <addr> <name>"
    addr = vars_.resolve(args[0])
    name = args[1]
    _LABELS.labels[f"0x{addr:X}"] = name
    _LABELS.save()
    return f"OK label 0x{addr:X} = {name}"

def composite_labels(args, vars_):
    if not _LABELS.labels: return "OK (no labels)"
    out = [f"OK {len(_LABELS.labels)} labels"]
    for a, n in sorted(_LABELS.labels.items(), key=lambda kv: int(kv[0], 16)):
        out.append(f"  {a}  {_C.CYAN}{n}{_C.RESET}")
    return "\n".join(out)

def composite_unlabel(args, vars_):
    if not args: return "ERR usage: unlabel <addr>"
    addr = vars_.resolve(args[0])
    k = f"0x{addr:X}"
    if k in _LABELS.labels:
        n = _LABELS.labels.pop(k); _LABELS.save()
        return f"OK removed label '{n}' from 0x{addr:X}"
    return f"ERR no label at 0x{addr:X}"

def composite_bookmark(args, vars_):
    """bookmark <name> <addr> [note...]  — save a named address for later recall."""
    if len(args) < 2: return "ERR usage: bookmark <name> <addr> [note...]"
    name = args[0]
    addr = vars_.resolve(args[1])
    note = " ".join(args[2:]).strip()
    _LABELS.bookmarks[name] = f"0x{addr:X}" + (f" | {note}" if note else "")
    _LABELS.save()
    return f"OK bookmark '{name}' = 0x{addr:X}" + (f"  — {note}" if note else "")

def composite_bookmarks(args, vars_):
    if not _LABELS.bookmarks: return "OK (no bookmarks)"
    out = [f"OK {len(_LABELS.bookmarks)} bookmarks"]
    for name, ref in sorted(_LABELS.bookmarks.items()):
        out.append(f"  {_C.CYAN}{name:<20}{_C.RESET}  {ref}")
    return "\n".join(out)

def composite_unbookmark(args, vars_):
    if not args: return "ERR usage: unbookmark <name>"
    if args[0] in _LABELS.bookmarks:
        _LABELS.bookmarks.pop(args[0]); _LABELS.save()
        return f"OK removed bookmark '{args[0]}'"
    return f"ERR no bookmark named '{args[0]}'"


# ── Memory snapshots + diff ─────────────────────────────────────────────
# Grab a region into a Python buffer, compare against a later grab of the
# same region. Highlights byte-level differences — what x64dbg does with
# its "compare" panel.

_SNAPSHOTS: Dict[str, Dict] = {}   # name -> {"addr": int, "size": int, "data": bytes}

def _fetch_bytes(addr: int, size: int, vars_: VarStore) -> Optional[bytes]:
    r = primitive_send("dump", [f"{addr:X}", f"{size:X}"], vars_)
    buf = bytearray()
    for ln in r.splitlines():
        m = re.match(r"\+0x[0-9A-Fa-f]+\s+((?:[0-9A-Fa-f]{2}\s+)+)", ln)
        if not m: continue
        for tok in m.group(1).split():
            buf.append(int(tok, 16))
    return bytes(buf) if buf else None

def composite_snapshot(args, vars_):
    """snapshot <name> <addr> <size>  — capture bytes for later diff."""
    if len(args) < 3: return "ERR usage: snapshot <name> <addr> <size>"
    name = args[0]
    addr = vars_.resolve(args[1])
    size = vars_.resolve(args[2])
    if size <= 0 or size > 0xC000:
        return "ERR size must be 1..0xC000"
    data = _fetch_bytes(addr, size, vars_)
    if data is None: return "ERR fetch failed"
    _SNAPSHOTS[name] = {"addr": addr, "size": size, "data": data}
    return f"OK snapshot '{name}' captured {len(data)} bytes @0x{addr:X}"

def composite_snapshots(args, vars_):
    if not _SNAPSHOTS: return "OK (no snapshots)"
    out = [f"OK {len(_SNAPSHOTS)} snapshots"]
    for n, s in _SNAPSHOTS.items():
        out.append(f"  {_C.CYAN}{n:<18}{_C.RESET}  @0x{s['addr']:X}  size=0x{s['size']:X}")
    return "\n".join(out)

def composite_diff(args, vars_):
    """diff <name>  — re-fetch the snapshot region and report changed bytes."""
    if not args: return "ERR usage: diff <snapshotName>"
    name = args[0]
    snap = _SNAPSHOTS.get(name)
    if not snap: return f"ERR no snapshot '{name}'"
    fresh = _fetch_bytes(snap["addr"], snap["size"], vars_)
    if fresh is None: return "ERR re-fetch failed"
    old = snap["data"]
    if len(fresh) != len(old):
        return f"OK size changed: was {len(old)} now {len(fresh)}"
    changes = [(i, old[i], fresh[i]) for i in range(len(old)) if old[i] != fresh[i]]
    if not changes:
        return f"OK no changes vs '{name}' ({len(old)} bytes compared)"
    out = [f"OK {len(changes)} byte(s) changed in '{name}' (base 0x{snap['addr']:X})"]
    for off, a, b in changes[:64]:
        out.append(f"  +0x{off:03X}  {_C.RED}{a:02X}{_C.RESET} → {_C.GREEN}{b:02X}{_C.RESET}")
    if len(changes) > 64: out.append(f"  ... ({len(changes) - 64} more)")
    return "\n".join(out)


# ── Struct viewer (uses propswalk + typed reads) ────────────────────────

def composite_struct(args, vars_):
    """struct <structName> <addr>  — decode each UPROPERTY at addr using the
    same reflection path propswalk uses. Prints name, offset, raw bytes,
    and (best-effort) a typed interpretation."""
    if len(args) < 2: return "ERR usage: struct <structName> <addr>"
    struct_name = args[0]
    base = vars_.resolve(args[1])

    # Resolve struct address once
    sresp = primitive_send("findstruct", [struct_name], vars_)
    saddr = _extract_hex(sresp)
    if not saddr:
        cresp = primitive_send("findcls", [struct_name], vars_)
        saddr = _extract_hex(cresp)
    if not saddr: return f"ERR struct/class '{struct_name}' not found"

    # Walk the field chain using primitives
    field_head = _extract_hex(primitive_send("read64", [f"{saddr + 0x50:X}"], vars_))
    if not field_head: return f"OK struct {struct_name} @0x{base:X}\n  (no ChildProperties)"

    fields: List[Tuple[int, str]] = []
    current = field_head
    seen = set(); safety = 200
    while current and current not in seen and safety > 0:
        seen.add(current)
        name = _extract_ok_token(primitive_send("nameof", [f"{current + 0x10:X}"], vars_))
        ro   = primitive_send("read32", [f"{current + 0x4C:X}"], vars_)
        mm   = re.search(r"\((-?\d+)\)", ro)
        off  = int(mm.group(1)) if mm else -1
        fields.append((off & 0xFFFF, name or "?"))
        current = _extract_hex(primitive_send("read64", [f"{current + 0x20:X}"], vars_))
        safety -= 1
    if not fields: return f"OK struct {struct_name} @0x{base:X}\n  (empty)"

    fields.sort()
    max_name = max(len(n) for _, n in fields)
    out = [f"OK struct {_C.BOLD}{struct_name}{_C.RESET} @0x{base:X}"]
    for off, name in fields:
        # Read 8 bytes speculatively — interpret as u64/float/ptr hint.
        u = _read64_u64(f"{base + off:X}", vars_)
        if u is None:
            out.append(f"  +0x{off:04X}  {name.ljust(max_name)}  {_C.RED}READ_ERR{_C.RESET}")
            continue
        lo = u & 0xFFFFFFFF
        f32 = struct.unpack("<f", struct.pack("<I", lo))[0]
        # Pretty hint: if it looks like a pointer (common UE address band),
        # show that. Otherwise show u64 / float decoding hints.
        hint = ""
        if 0x00007FF000000000 <= u <= 0x00007FFFFFFFFFFF or 0x0000020000000000 <= u <= 0x00000FFFFFFFFFFF:
            # UE usually allocs in these bands — very coarse heuristic
            cls = _extract_ok_token(primitive_send("classof", [f"{u:X}"], vars_))
            if cls: hint = f"{_C.DIM}→{_C.RESET} {_C.MAG}{cls}{_C.RESET}"
        elif abs(f32) > 1e-6 and abs(f32) < 1e9:
            hint = f"{_C.DIM}f32≈{_C.RESET}{f32:g}"
        out.append(f"  +0x{off:04X}  {_C.WHITE}{name.ljust(max_name)}{_C.RESET}  "
                   f"{_C.CYAN}0x{u:016X}{_C.RESET}  {hint}")
    return "\n".join(out)


# ── Disasm (optional — capstone) ────────────────────────────────────────

def composite_disasm(args, vars_):
    if not _HAS_CAPSTONE:
        return ("ERR capstone not installed — `pip install capstone` "
                "for disassembly, or use `dump <addr> <size>` for raw bytes.")
    if len(args) < 2: return "ERR usage: disasm <addr> <nInstructions>"
    addr = vars_.resolve(args[0])
    n    = vars_.resolve(args[1])
    if n <= 0 or n > 256: return "ERR instruction count must be 1..256"
    data = _fetch_bytes(addr, min(n * 16, 0x400), vars_)
    if not data: return "ERR byte fetch failed"
    md = _cs.Cs(_cs.CS_ARCH_X86, _cs.CS_MODE_64)
    out = [f"OK disasm 0x{addr:X} ({n} instructions)"]
    cnt = 0
    for ins in md.disasm(data, addr):
        if cnt >= n: break
        hexstr = " ".join(f"{b:02X}" for b in ins.bytes)
        out.append(f"  {_C.GRAY}0x{ins.address:016X}{_C.RESET}  "
                   f"{_C.DIM}{hexstr:<32}{_C.RESET}  "
                   f"{_C.CYAN}{ins.mnemonic:<8}{_C.RESET} {ins.op_str}")
        cnt += 1
    return "\n".join(out)


# ── Phase B wrappers (will light up when DLL gets rebuilt) ──────────────
# These forward to DLL primitives that don't exist yet — they return
# whatever the DLL says. Once the server-side scanner commands land,
# these become fully functional without any Python change.

def composite_memmap(args, vars_):
    """memmap [addr [range]] — VirtualQuery walker. Needs DLL v+1."""
    if len(args) >= 1:
        a = vars_.resolve_hex(args[0])
        if len(args) >= 2:
            return primitive_send("memmap", [a, vars_.resolve_hex(args[1])], vars_)
        return primitive_send("memmap", [a], vars_)
    return primitive_send("memmap", [], vars_)

def composite_modules(args, vars_):
    """modules — EnumProcessModules list. Needs DLL v+1."""
    return primitive_send("modules", args, vars_)

def composite_strings(args, vars_):
    """strings <addr> <range> [minLen]  — ASCII + UTF-16 scanner. Needs DLL v+1."""
    if len(args) < 2: return "ERR usage: strings <addr> <range> [minLen=6]"
    a = vars_.resolve_hex(args[0]); r = vars_.resolve_hex(args[1])
    minl = args[2] if len(args) > 2 else "6"
    return primitive_send("strings", [a, r, minl], vars_)

def composite_refs(args, vars_):
    """refs <target> <scanAddr> <range>  — find pointers to target. Needs DLL v+1."""
    if len(args) < 3: return "ERR usage: refs <targetAddr> <scanAddr> <range>"
    t = vars_.resolve_hex(args[0]); a = vars_.resolve_hex(args[1]); r = vars_.resolve_hex(args[2])
    return primitive_send("refs", [t, a, r], vars_)

def composite_search(args, vars_):
    """search <addr> <range> <type> <value>  — memory search. Needs DLL v+1.
    type ∈ u32, u64, f32, f64, ascii, utf16."""
    if len(args) < 4: return "ERR usage: search <addr> <range> <type> <value>"
    a = vars_.resolve_hex(args[0]); r = vars_.resolve_hex(args[1])
    return primitive_send("search", [a, r, args[2], args[3]], vars_)


COMPOSITES: Dict[str, Callable] = {
    "follow":         composite_follow,
    "obj":            composite_obj,
    "finduobj":       composite_finduobj,
    "invtest":        composite_invtest,
    "findplayerinv": composite_findplayerinv,
    "propswalk":      composite_propswalk,
    "bag":            composite_bag,
    "listbag":        composite_listbag,
    "setweightzero":  composite_setweightzero,
    "diag":           composite_diag,
    "getprop":        composite_getprop,
    "setprop":        composite_setprop,
    # Typed readers
    "readf32":        composite_readf32,
    "readf64":        composite_readf64,
    "readi32":        composite_readi32,
    "readi64":        composite_readi64,
    "readstr":        composite_readstr,
    "readunicode":    composite_readunicode,
    "readguid":       composite_readguid,
    "readptrarr":     composite_readptrarr,
    # Labels & bookmarks
    "label":          composite_label,
    "labels":         composite_labels,
    "unlabel":        composite_unlabel,
    "bookmark":       composite_bookmark,
    "bookmarks":      composite_bookmarks,
    "unbookmark":     composite_unbookmark,
    # Snapshots + diff
    "snapshot":       composite_snapshot,
    "snapshots":      composite_snapshots,
    "diff":           composite_diff,
    # Struct viewer + disasm
    "struct":         composite_struct,
    "disasm":         composite_disasm,
    # DLL-backed scanners (Phase B — dormant until DLL rebuild)
    "memmap":         composite_memmap,
    "modules":        composite_modules,
    "strings":        composite_strings,
    "refs":           composite_refs,
    "search":         composite_search,
}


# ─── Output formatters (pretty hex, tables, etc.) ───────────────────────

_DUMP_LINE_RE = re.compile(r"^\+0x([0-9A-Fa-f]+)\s+((?:[0-9A-Fa-f]{2}\s+)+)\s*$")

def _ascii_sidebar(hexbytes: str) -> str:
    out = []
    for b in hexbytes.split():
        try:
            v = int(b, 16)
        except ValueError:
            out.append(" "); continue
        out.append(chr(v) if 32 <= v < 127 else ".")
    return "".join(out)


def _labels_lookup() -> Dict[int, str]:
    """Flatten the persistent label dict to {int addr: name} once per call."""
    out = {}
    for k, v in _LABELS.labels.items():
        try: out[int(k, 16)] = v
        except Exception: pass
    return out

def format_dump(resp: str, base_addr: Optional[int] = None) -> str:
    """Render a `dump` response as a colored xxd-style block with ASCII
    sidebar. If any labeled addresses fall inside the dump, their names
    are overlaid at the end of the matching row (x64dbg-style annotations)."""
    lines = resp.splitlines()
    out: List[str] = []
    labels = _labels_lookup()
    for ln in lines:
        m = _DUMP_LINE_RE.match(ln)
        if not m:
            out.append(ln); continue
        off = int(m.group(1), 16)
        off_hex = f"{off:X}".upper()
        bytes_str = m.group(2).rstrip()
        tokens = bytes_str.split()
        lhs = " ".join(tokens[:8]).ljust(23)
        rhs = " ".join(tokens[8:]).ljust(23)
        ascii_ = _ascii_sidebar(bytes_str)
        # Label annotation: any label whose address falls in [base+off, base+off+16)?
        annotation = ""
        if base_addr is not None and labels:
            for i in range(16):
                n = labels.get(base_addr + off + i)
                if n:
                    annotation = f"  {_C.MAG}; {n}{_C.RESET}"; break
        out.append(f"{_C.GRAY}+0x{off_hex:>4}{_C.RESET}  "
                   f"{_C.CYAN}{lhs}{_C.RESET}  {_C.CYAN}{rhs}{_C.RESET}  "
                   f"{_C.DIM}│ {ascii_} │{_C.RESET}{annotation}")
    return "\n".join(out)


_PROPS_LINE_RE = re.compile(r"^\s*\+0x([0-9A-Fa-f]+)\s+(\S+)(?:\s+(\S.*))?$")

def format_props_table(resp: str) -> str:
    """Render `props`/`propsall` output as an aligned coloured table."""
    header_lines, rows = [], []
    for ln in resp.splitlines():
        m = _PROPS_LINE_RE.match(ln)
        if m:
            rows.append((m.group(1).upper(), m.group(2), m.group(3) or ""))
        else:
            header_lines.append(ln)
    if not rows:
        return resp
    max_name = max(len(r[1]) for r in rows)
    out = [*header_lines]
    for off, name, tail in rows:
        out.append(f"  {_C.GRAY}+0x{off:>3}{_C.RESET}  "
                   f"{_C.WHITE}{name.ljust(max_name)}{_C.RESET}  "
                   f"{_C.DIM}{tail}{_C.RESET}")
    return "\n".join(out)


_LISTITEMS_RE = re.compile(r"^\s*\[(\d+)\]\s+(\S+)\s+x(\d+)\s*$")

def format_listitems(resp: str) -> str:
    """Render `listitems` as a coloured table with slot/name/count columns."""
    header, rows = [], []
    for ln in resp.splitlines():
        m = _LISTITEMS_RE.match(ln)
        if m:
            rows.append((m.group(1), m.group(2), m.group(3)))
        elif re.match(r"^\s*\[(\d+)\]\s+\(empty\)\s*$", ln):
            mm = re.search(r"\[(\d+)\]", ln)
            rows.append((mm.group(1) if mm else "", "(empty)", "0"))
        else:
            header.append(ln)
    if not rows:
        return resp
    max_name = max(len(r[1]) for r in rows)
    out = [*header]
    for slot, name, n in rows:
        empty = name == "(empty)"
        nc = _C.DIM if empty else _C.WHITE
        cc = _C.DIM if empty else (_C.GREEN if int(n) > 0 else _C.GRAY)
        out.append(f"  {_C.GRAY}[{slot:>2}]{_C.RESET}  "
                   f"{nc}{name.ljust(max_name)}{_C.RESET}  "
                   f"{cc}x{n}{_C.RESET}")
    return "\n".join(out)


def format_response(cmd: str, resp: str, base_addr: Optional[int] = None) -> str:
    """Dispatch a response to its per-command pretty formatter, falling back
    to generic colorization (OK green, ERR red) if none matches."""
    if cmd == "dump":
        return format_dump(resp, base_addr)
    if cmd in ("props", "propsall"):
        return format_props_table(resp)
    if cmd == "listitems" or cmd == "listbag":
        return format_listitems(resp)
    # Generic: highlight first OK/ERR token
    if resp.startswith("OK "):
        return f"{_C.GREEN}OK{_C.RESET} {resp[3:]}"
    if resp.startswith("ERR "):
        return f"{_C.RED}ERR{_C.RESET} {resp[4:]}"
    return resp


# ─── Request dispatch (unified path for CLI/REPL/script) ────────────────

def run_one(line: str, vars_: VarStore, formatter: bool = True,
            timing: bool = False) -> str:
    """Parse + execute a single command line. Supports `cmd args = name` to
    save the extracted hex result into a named variable. If formatter=True,
    the returned string is already colorised (for direct printing). Set
    False to get the raw wire response (JSON / tests)."""
    line = line.strip()
    if not line:
        return ""

    # Save-suffix: "cmd args = name"
    save_as: Optional[str] = None
    m = re.match(r"^(.+?)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*$", line)
    if m:
        line = m.group(1); save_as = m.group(2)

    parts = shlex.split(line)
    if not parts:
        return ""
    cmd = parts[0].lower()
    args = parts[1:]

    t0 = time.perf_counter()
    if cmd == "help":
        out = help_text(args[0] if args else None)
    elif cmd in COMPOSITES:
        out = COMPOSITES[cmd](args, vars_)
    else:
        out = primitive_send(cmd, args, vars_)
    dt = (time.perf_counter() - t0) * 1000

    hv = _extract_hex(out)
    if hv is not None:
        vars_.prev = hv
    if save_as and hv is not None:
        vars_.set(save_as, hv)
        out += f"\n  [saved ${save_as} = 0x{hv:X}]"

    if formatter:
        # Pass the resolved base address for `dump` so label annotations
        # can be overlaid on the hex grid.
        base_addr: Optional[int] = None
        if cmd == "dump" and args:
            try: base_addr = vars_.resolve(args[0])
            except Exception: pass
        out = format_response(cmd, out, base_addr)
    if timing:
        out += f"\n  {_C.DIM}({dt:.2f} ms){_C.RESET}"
    return out


def run_batch(script: str, vars_: VarStore, formatter: bool = True,
              timing: bool = False) -> List[str]:
    cmds, buf, in_q = [], [], False
    for ch in script:
        if ch == '"': in_q = not in_q
        if (ch == ';' or ch == '\n') and not in_q:
            if buf: cmds.append("".join(buf).strip()); buf = []
        else:
            buf.append(ch)
    if buf: cmds.append("".join(buf).strip())
    return [run_one(c, vars_, formatter, timing) for c in cmds if c]


# ─── Help system ────────────────────────────────────────────────────────

def help_text(query: Optional[str] = None) -> str:
    """Return a formatted help block. With no argument, list every command
    grouped by category. With a command name, show the detailed signature
    and example."""
    if query:
        q = query.lower()
        match = next((c for c in COMMANDS if c.name == q), None)
        if not match and q in COMPOSITES:
            ccat = {
                "follow":"Chase a pointer chain, report final class.",
                "obj":"Class + name + outer chain (5 levels).",
                "finduobj":"findobj + classof + nameof in one call.",
                "invtest":"Exhaustive UInventory candidate analysis.",
                "findplayerinv":"Full walk: character → InvComp → bags.",
                "propswalk":"Manual FField walk (works on UScriptStruct).",
                "bag":"UInventory pointer by name (Backpack / Quickbar…).",
                "listbag":"bag + listitems combo.",
                "setweightzero":"One-shot weight zero on every bag.",
                "diag":"character + inv + Backpack contents.",
                "getprop":"propget wrapper with resolved addr.",
                "setprop":"propset wrapper with resolved addr.",
                "readf32":"Read 4 bytes as IEEE 754 float.",
                "readf64":"Read 8 bytes as IEEE 754 double.",
                "readi32":"Read 4 bytes as signed int32.",
                "readi64":"Read 8 bytes as signed int64.",
                "readstr":"Read NUL-terminated UTF-8 string (readstr <addr> [maxLen]).",
                "readunicode":"Read NUL-terminated UTF-16LE string (readunicode <addr> [maxChars]).",
                "readguid":"Read a 16-byte FGuid as {UUID}.",
                "readptrarr":"Read N consecutive 8-byte pointers.",
                "label":"Tag an address with a name (persisted to ~/.zeusmod_labels.json).",
                "labels":"List every labelled address.",
                "unlabel":"Remove the label on <addr>.",
                "bookmark":"Save a named address for later recall (bookmark <name> <addr> [note…]).",
                "bookmarks":"List every bookmark.",
                "unbookmark":"Remove a named bookmark.",
                "snapshot":"Capture a memory region for later diff (snapshot <name> <addr> <size>).",
                "snapshots":"List every captured snapshot.",
                "diff":"Re-fetch a snapshot and report changed bytes.",
                "struct":"Decode a UStruct at <addr> — fields, offsets, best-effort typed interpretation.",
                "disasm":"Disassemble N x64 instructions at <addr> (needs capstone).",
                "memmap":"List target memory regions via VirtualQuery [DLL v+1].",
                "modules":"List every loaded DLL / module [DLL v+1].",
                "strings":"ASCII + UTF-16 string scan in a region [DLL v+1].",
                "refs":"Find pointers pointing to a target address [DLL v+1].",
                "search":"Find u32/u64/f32/f64/ascii/utf16 matches in a region [DLL v+1].",
            }[q]
            return f"{_C.BOLD}{q}{_C.RESET}  {_C.DIM}[composite]{_C.RESET}\n  {ccat}"
        if not match:
            return f"{_C.RED}ERR{_C.RESET} no command named '{q}'"
        return (f"{_C.BOLD}{match.name}{_C.RESET}  "
                f"{_C.MAG}[{match.group}]{_C.RESET}\n"
                f"  {match.desc}\n\n"
                f"  {_C.DIM}usage  {_C.RESET}{match.usage}\n"
                f"  {_C.DIM}e.g.   {_C.RESET}{_C.CYAN}{match.example}{_C.RESET}")
    # Listing mode
    groups: Dict[str, List[Cmd]] = {}
    for c in COMMANDS:
        groups.setdefault(c.group, []).append(c)
    lines = [f"{_C.BOLD}ZeusMod inspector — command index{_C.RESET}"]
    lines.append(f"{_C.DIM}Variables: save with ' = name' suffix, use as $name or $prev.{_C.RESET}")
    lines.append("")
    for group in ["Lookup","Reflection","Memory","Write","Player","Cheats"]:
        if group not in groups: continue
        lines.append(f"{_C.MAG}▸ {group}{_C.RESET}")
        for c in groups[group]:
            lines.append(f"  {_C.CYAN}{c.name:<14}{_C.RESET}{c.desc}")
        lines.append("")
    # Break composites into logical sub-groups for readability.
    comp_groups = [
        ("Navigation",      ["follow","obj","finduobj","invtest","findplayerinv","propswalk","diag"]),
        ("Inventory",       ["bag","listbag","setweightzero","getprop","setprop"]),
        ("Typed readers",   ["readf32","readf64","readi32","readi64","readstr","readunicode","readguid","readptrarr"]),
        ("Labels & marks",  ["label","labels","unlabel","bookmark","bookmarks","unbookmark"]),
        ("Snapshots",       ["snapshot","snapshots","diff"]),
        ("Advanced",        ["struct","disasm"]),
        ("DLL scanners",    ["memmap","modules","strings","refs","search"]),
    ]
    for title, names in comp_groups:
        lines.append(f"{_C.MAG}▸ {title}{_C.RESET}")
        for n in names:
            if n in COMPOSITES:
                lines.append(f"  {_C.CYAN}{n:<14}{_C.RESET}{_C.DIM}help {n}{_C.RESET}")
        lines.append("")
    return "\n".join(lines).rstrip()


# ─── REPL (readline + persistent history + tab completion) ──────────────

_HISTORY_PATH = os.path.expanduser("~/.zeusmod_history")

def _setup_readline(vars_: VarStore) -> None:
    try:
        import readline  # type: ignore
    except ImportError:
        try:
            import pyreadline3 as readline  # type: ignore  (Windows)
        except ImportError:
            return

    if os.path.exists(_HISTORY_PATH):
        try: readline.read_history_file(_HISTORY_PATH)
        except Exception: pass
    try: readline.set_history_length(1000)
    except Exception: pass

    completion_pool = (
        [c.name for c in COMMANDS]
        + list(COMPOSITES.keys())
        + ["help", "quit", "exit", "vars", "clear"]
    )

    def completer(text: str, state: int):
        if text.startswith("$"):
            seed = text[1:]
            candidates = ["$" + n for n in (["prev"] + list(vars_.vars.keys()))
                          if n.startswith(seed)]
        else:
            candidates = [n for n in completion_pool if n.startswith(text)]
        return candidates[state] if state < len(candidates) else None

    readline.set_completer(completer)
    try: readline.parse_and_bind("tab: complete")
    except Exception: pass


def _save_history() -> None:
    try:
        import readline  # type: ignore
        readline.write_history_file(_HISTORY_PATH)
    except Exception:
        pass


def repl(timing: bool = False) -> None:
    vars_ = VarStore()
    _setup_readline(vars_)
    banner = (f"{_C.BOLD}ZeusMod inspector{_C.RESET}  "
              f"{_C.DIM}— pipe: {PIPE_NAME}{_C.RESET}\n"
              f"{_C.DIM}Type 'help' or 'help <cmd>'. Tab-completes commands and $variables. "
              f"Ctrl-D or 'quit' to exit.{_C.RESET}")
    print(banner)
    while True:
        try:
            line = input(f"{_C.CYAN}zm>{_C.RESET} ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line: continue
        if line in ("quit", "exit", "q"):
            break
        if line == "clear":
            os.system("cls" if os.name == "nt" else "clear"); continue
        if line == "vars":
            if vars_.prev is not None:
                print(f"  {_C.CYAN}$prev{_C.RESET} = 0x{vars_.prev:X}")
            for k, v in vars_.vars.items():
                print(f"  {_C.CYAN}${k}{_C.RESET} = 0x{v:X}")
            if vars_.prev is None and not vars_.vars:
                print(f"  {_C.DIM}(no variables defined yet){_C.RESET}")
            continue
        try:
            print(run_one(line, vars_, formatter=True, timing=timing))
        except Exception as e:
            print(f"{_C.RED}ERR{_C.RESET} {e}")
    _save_history()


# ─── Watch mode (re-run at interval, highlight diffs) ───────────────────

def watch_mode(cmd: str, interval: float, timing: bool) -> None:
    vars_ = VarStore()
    previous: Optional[str] = None
    try:
        while True:
            os.system("cls" if os.name == "nt" else "clear")
            ts = time.strftime("%H:%M:%S")
            header = f"{_C.BOLD}watch{_C.RESET} {_C.DIM}{ts}{_C.RESET}  {_C.CYAN}{cmd}{_C.RESET}  {_C.DIM}every {interval}s{_C.RESET}"
            print(header); print()
            try:
                out = run_one(cmd, vars_, formatter=True, timing=timing)
            except Exception as e:
                out = f"{_C.RED}ERR{_C.RESET} {e}"
            # Highlight diff lines vs previous iteration (unified diff glyphs)
            if previous is not None and previous != out:
                diff = difflib.ndiff(previous.splitlines(), out.splitlines())
                for ln in diff:
                    if   ln.startswith("+ "): print(f"{_C.GREEN}+{_C.RESET} {ln[2:]}")
                    elif ln.startswith("- "): print(f"{_C.RED}-{_C.RESET} {ln[2:]}")
                    elif ln.startswith("? "): continue
                    else:                     print(f"  {ln[2:]}")
            else:
                print(out)
            previous = out
            time.sleep(interval)
    except KeyboardInterrupt:
        pass


# ─── JSON output wrapper ────────────────────────────────────────────────

def _jsonify_one(cmd_line: str, raw_resp: str) -> Dict:
    ok   = raw_resp.startswith("OK")
    hvs  = [int(m.group(1), 16) for m in _HEX_TOKEN_RE.finditer(raw_resp)]
    return {
        "command":    cmd_line,
        "ok":         ok,
        "response":   raw_resp.strip(),
        "hex_values": [hex(h) for h in hvs[:8]],
    }


# ─── CLI entry ──────────────────────────────────────────────────────────

def _build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="inspect.py",
        description="ZeusMod live inspector (DLL pipe client). "
                    "With no args, starts an interactive REPL.",
        epilog="Run `python inspect.py -c help` to list every command.",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    p.add_argument("cmd", nargs="*",
                   help="one-shot command (e.g. `classof 0x...`).")
    p.add_argument("-c", "--batch", metavar="SCRIPT",
                   help="run a ';'- or newline-separated batch.")
    p.add_argument("--script", metavar="PATH",
                   help="load a .zm script file (one command per line, # for comments).")
    p.add_argument("--watch", nargs=2, metavar=("CMD", "INTERVAL"),
                   help="re-run CMD every INTERVAL seconds, highlight diffs.")
    p.add_argument("--json", action="store_true",
                   help="machine-readable output: one JSON object per line.")
    p.add_argument("--timing", action="store_true",
                   help="append per-command latency in milliseconds.")
    return p


def _run_script_file(path: str, vars_: VarStore, as_json: bool, timing: bool) -> int:
    if not os.path.isfile(path):
        print(f"{_C.RED}ERR{_C.RESET} script not found: {path}", file=sys.stderr); return 2
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    # Strip comment lines (# ...) and blanks
    filtered = "\n".join(
        ln for ln in content.splitlines()
        if ln.strip() and not ln.lstrip().startswith("#"))
    for line in filtered.splitlines():
        if as_json:
            raw = run_one(line, vars_, formatter=False, timing=False)
            print(json.dumps(_jsonify_one(line, raw), ensure_ascii=False))
        else:
            out = run_one(line, vars_, formatter=True, timing=timing)
            if out: print(out); print("---")
    return 0


def main() -> int:
    parser = _build_argparser()
    ns = parser.parse_args()
    vars_ = VarStore()

    # Watch — preempts other modes.
    if ns.watch:
        try: interval = float(ns.watch[1])
        except ValueError:
            print(f"{_C.RED}ERR{_C.RESET} interval must be a number", file=sys.stderr); return 2
        watch_mode(ns.watch[0], interval, ns.timing)
        return 0

    # Script file.
    if ns.script:
        return _run_script_file(ns.script, vars_, ns.json, ns.timing)

    # Batch.
    if ns.batch:
        results = run_batch(ns.batch, vars_,
                            formatter=not ns.json, timing=ns.timing and not ns.json)
        if ns.json:
            # Re-run raw so we can embed real responses into JSON.
            for c in [ln.strip() for ln in re.split(r"[;\n]", ns.batch) if ln.strip()]:
                raw = run_one(c, VarStore(), formatter=False, timing=False)
                print(json.dumps(_jsonify_one(c, raw), ensure_ascii=False))
        else:
            for r in results:
                print(r); print("---")
        return 0

    # One-shot positional.
    if ns.cmd:
        cmdline = " ".join(ns.cmd)
        if ns.json:
            raw = run_one(cmdline, vars_, formatter=False, timing=False)
            print(json.dumps(_jsonify_one(cmdline, raw), ensure_ascii=False))
        else:
            print(run_one(cmdline, vars_, formatter=True, timing=ns.timing))
        return 0

    # Interactive REPL.
    repl(timing=ns.timing)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ConnectionError as ce:
        print(f"{_C.RED}ERR{_C.RESET} {ce}", file=sys.stderr); sys.exit(3)
