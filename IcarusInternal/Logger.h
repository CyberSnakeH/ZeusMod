#pragma once
// ============================================================================
// ZeusMod Logger — pretty, colored, timestamped console output.
//
// Uses Windows VT100 escape sequences (ENABLE_VIRTUAL_TERMINAL_PROCESSING)
// to drive the standard console. Keeps a tiny no-dependency surface so the
// trainer's existing printf calls keep working side by side.
//
// Usage:
//   Log::InitConsole();                             // once, before anything
//   LOG_OK("ready in %d ms", ms);
//   LOG_INFO("[INIT] %s resolved", name);
//   LOG_WARN("unresolved %s", name);
//   LOG_ERR("failed to attach: %lu", GetLastError());
//   LOG_DEBUG("raw ptr = 0x%llx", (unsigned long long)ptr);
//   LOG_SECTION("Offset resolution");
//
// Existing bare printf("...") calls still appear — they just come through
// uncolored. New diagnostic output should go through the macros above so
// the console stays consistent and readable.
// ============================================================================

#include <cstdio>

namespace Log {

// Allocates and initializes the debug console:
//  - AllocConsole / CONOUT redirect
//  - VT100 ANSI mode enabled
//  - UTF-8 output code page
//  - ZeusMod title + ASCII banner
//  - Large scrollback buffer
void InitConsole();

// Print a raw line. Returns number of characters written (unused).
// Callers should prefer the macros below.
void Line(const char* level_color,
          const char* level_tag,
          const char* text_color,
          const char* fmt, ...);

// Print a visually distinct section separator. Used to break up init,
// discovery and tick phases in the log.
void Section(const char* title);

// ASCII banner. Called automatically by InitConsole; exposed so a hook
// or reload can reprint it.
void Banner();

} // namespace Log

// --- ANSI color palette (VT100). Escape codes are inline so the logger
//     header has zero runtime dependencies.
#define LOG_ANSI_RESET   "\x1b[0m"
#define LOG_ANSI_DIM     "\x1b[2m"
#define LOG_ANSI_BOLD    "\x1b[1m"
#define LOG_ANSI_BLACK   "\x1b[30m"
#define LOG_ANSI_RED     "\x1b[31m"
#define LOG_ANSI_GREEN   "\x1b[32m"
#define LOG_ANSI_YELLOW  "\x1b[33m"
#define LOG_ANSI_BLUE    "\x1b[34m"
#define LOG_ANSI_MAGENTA "\x1b[35m"
#define LOG_ANSI_CYAN    "\x1b[36m"
#define LOG_ANSI_WHITE   "\x1b[37m"
#define LOG_ANSI_GRAY    "\x1b[90m"
#define LOG_ANSI_BR_RED     "\x1b[91m"
#define LOG_ANSI_BR_GREEN   "\x1b[92m"
#define LOG_ANSI_BR_YELLOW  "\x1b[93m"
#define LOG_ANSI_BR_BLUE    "\x1b[94m"
#define LOG_ANSI_BR_MAGENTA "\x1b[95m"
#define LOG_ANSI_BR_CYAN    "\x1b[96m"
#define LOG_ANSI_BR_WHITE   "\x1b[97m"

// --- Public logging macros. Each formats a full line with timestamp +
//     colored level tag + message body. Tags are padded to 5 chars so
//     every line aligns cleanly regardless of category.
//
// Generic log levels
#define LOG_INFO(fmt, ...)    ::Log::Line(LOG_ANSI_BR_CYAN,    "INFO",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_OK(fmt, ...)      ::Log::Line(LOG_ANSI_BR_GREEN,   " OK ",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    ::Log::Line(LOG_ANSI_BR_YELLOW,  "WARN",   LOG_ANSI_YELLOW, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)     ::Log::Line(LOG_ANSI_BR_RED,     "ERR ",   LOG_ANSI_BR_RED, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   ::Log::Line(LOG_ANSI_GRAY,       "DBG ",   LOG_ANSI_GRAY,   fmt, ##__VA_ARGS__)

// Subsystem-tagged macros. All route through ::Log::Line with the same
// timestamp / alignment formatter, just with distinct color and 5-char tag.
#define LOG_UOBJ(fmt, ...)    ::Log::Line(LOG_ANSI_GRAY,       "UOBJ",   LOG_ANSI_GRAY,   fmt, ##__VA_ARGS__)
#define LOG_PIPE(fmt, ...)    ::Log::Line(LOG_ANSI_BR_YELLOW,  "PIPE",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_HOOK(fmt, ...)    ::Log::Line(LOG_ANSI_BR_MAGENTA, "HOOK",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_SCAN(fmt, ...)    ::Log::Line(LOG_ANSI_BR_BLUE,    "SCAN",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_PROPS(fmt, ...)   ::Log::Line(LOG_ANSI_BR_CYAN,    "PROP",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_META(fmt, ...)    ::Log::Line(LOG_ANSI_BR_MAGENTA, "META",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_RESOLVE(fmt, ...) ::Log::Line(LOG_ANSI_BR_BLUE,    "RSLV",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_PATCH(fmt, ...)   ::Log::Line(LOG_ANSI_BR_RED,     "PTCH",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_FC(fmt, ...)      ::Log::Line(LOG_ANSI_BR_CYAN,    "CRFT",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_DISC(fmt, ...)    ::Log::Line(LOG_ANSI_MAGENTA,    "DISC",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_TICKFIX(fmt, ...) ::Log::Line(LOG_ANSI_YELLOW,     "TICK",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_LAYOUT(fmt, ...)  ::Log::Line(LOG_ANSI_GRAY,       "LAYT",   LOG_ANSI_GRAY,   fmt, ##__VA_ARGS__)
#define LOG_RENDER(fmt, ...)  ::Log::Line(LOG_ANSI_BR_GREEN,   "RNDR",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_TEMP(fmt, ...)    ::Log::Line(LOG_ANSI_BR_CYAN,    "TEMP",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_MEGA(fmt, ...)    ::Log::Line(LOG_ANSI_BR_YELLOW,  "MEGA",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_FOUND(fmt, ...)   ::Log::Line(LOG_ANSI_BR_GREEN,   "FIND",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_COMP(fmt, ...)    ::Log::Line(LOG_ANSI_GRAY,       "COMP",   LOG_ANSI_GRAY,   fmt, ##__VA_ARGS__)
#define LOG_SECTION(title)    ::Log::Section(title)
