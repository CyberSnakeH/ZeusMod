#pragma once
// ============================================================================
// ZeusMod Logger — structured, colored, timestamped console with KPI tracking.
//
// Produces three layers of output:
//   1. Streaming lines            LOG_INFO / LOG_OK / LOG_WARN / LOG_ERR / LOG_*
//   2. Structural grouping         LOG_SECTION + LOG_GROUP / LOG_STEP / LOG_STAT
//   3. End-of-phase summary        Log::Resume::Ok / Fail / Set / Note
//                                 + Log::PrintResume() (boxed final report)
//
// Typical init flow:
//
//   Log::InitConsole();
//   LOG_SECTION("Resolving UPROPERTY offsets");
//   for (...) {
//       int off = FindPropertyOffset(c, p);
//       if (off >= 0) { LOG_STEP("%s::%s -> 0x%X", c, p, off); Log::Resume::Ok("offsets"); }
//       else          { LOG_WARN("unresolved %s::%s", c, p);    Log::Resume::Fail("offsets", "%s::%s", c, p); }
//   }
//   Log::Resume::Set("item_library", "%zu entries", g_itemLibrary.size());
//   Log::PrintResume("Initialisation");
//
// Uses VT100 ANSI (ENABLE_VIRTUAL_TERMINAL_PROCESSING) for color. UTF-8
// output code page set in InitConsole, so the summary box can use U+2500-
// ish box-drawing characters safely.
// ============================================================================

#include <cstdio>
#include <cstdarg>

namespace Log {

// Initialize the debug console (AllocConsole + CONOUT redirect + VT100 +
// UTF-8). Idempotent.
void InitConsole();

// Banner printed automatically by InitConsole. Exposed so hot reloads can
// reprint it.
void Banner();

// Emit a single line. Callers should prefer the macros below.
void Line(const char* level_color,
          const char* level_tag,
          const char* text_color,
          const char* fmt, ...);

// Structural separators — bigger visual weight than Line.
void Section(const char* title);            // top-level phase (INIT / HOOK / SCAN)
void Group(const char* title);              // scoped sub-phase, increases indent
void GroupClose();                          // close matching Group

// Step / stat — indented child of the current Group. Use them to show
// "what happened" as a tree under a section.
//   LOG_STEP("pipe server listening")
//   LOG_STAT("item library", "3054 entries")
void Step(const char* fmt, ...);
void Stat(const char* key, const char* fmt, ...);

// ── Resume (end-of-phase KPI tracker) ──────────────────────────────────
// Collects counters / highlights during init, then renders them as a
// boxed summary. Categories are arbitrary strings; use short labels.
namespace Resume {
    // Increment the success counter in `category`.
    void Ok(const char* category);
    // Increment the failure counter AND remember `why` (first N).
    void Fail(const char* category, const char* why_fmt, ...);
    // Record a one-shot named value (e.g. "item_library", "3054 entries").
    void Set(const char* key, const char* value_fmt, ...);
    // Flag a free-form warning or info the summary should surface.
    // severity: 'I' info (cyan), 'W' warning (yellow), 'E' error (red).
    void Note(char severity, const char* fmt, ...);
    // Reset all tracked state (useful between distinct init phases).
    void Reset();
} // namespace Resume

// Render everything the Resume namespace has accumulated as a framed
// table, then reset. Title is the phase name (e.g. "Initialisation",
// "Post-craft diagnostics").
void PrintResume(const char* title);

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
//     indent + colored level tag + message body. Tags are padded to 4
//     chars so every line aligns cleanly regardless of category.
//
// Generic log levels
#define LOG_INFO(fmt, ...)    ::Log::Line(LOG_ANSI_BR_CYAN,    "INFO",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_OK(fmt, ...)      ::Log::Line(LOG_ANSI_BR_GREEN,   " OK ",   LOG_ANSI_WHITE,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)    ::Log::Line(LOG_ANSI_BR_YELLOW,  "WARN",   LOG_ANSI_YELLOW, fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)     ::Log::Line(LOG_ANSI_BR_RED,     "ERR ",   LOG_ANSI_BR_RED, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)   ::Log::Line(LOG_ANSI_GRAY,       "DBG ",   LOG_ANSI_GRAY,   fmt, ##__VA_ARGS__)

// Subsystem-tagged macros. All route through ::Log::Line with the same
// timestamp / alignment formatter, just with distinct color and 4-char tag.
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

// Structural macros (nest as a tree under a section)
#define LOG_SECTION(title)    ::Log::Section(title)
#define LOG_GROUP(title)      ::Log::Group(title)
#define LOG_GROUP_CLOSE()     ::Log::GroupClose()
#define LOG_STEP(fmt, ...)    ::Log::Step(fmt, ##__VA_ARGS__)
#define LOG_STAT(k, fmt, ...) ::Log::Stat(k, fmt, ##__VA_ARGS__)
