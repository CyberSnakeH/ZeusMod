#include "Logger.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

namespace {

FILE* g_conOut = nullptr;
bool  g_vtEnabled = false;
int   g_indent = 0;                // current indent level (nested LOG_GROUP)
ULONGLONG g_startTickMs = 0;       // set on InitConsole; used for "ready in" totals

// ── Resume state ─────────────────────────────────────────────────────
struct Counter { int ok = 0, fail = 0; std::vector<std::string> fail_examples; };
struct Note    { char severity; std::string text; };
struct KV      { std::string key, value; };

std::unordered_map<std::string, Counter> g_counters;
std::vector<std::string>                 g_counter_order;
std::vector<KV>                          g_values;
std::vector<Note>                        g_notes;

void FormatTimestamp(char* out, size_t cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(out, cap, _TRUNCATE, "%02u:%02u:%02u.%03u",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void EnableVirtualTerminal() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;
    constexpr DWORD kEnableVT = 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    if (SetConsoleMode(h, mode | kEnableVT)) g_vtEnabled = true;
}

void GrowConsoleBuffer() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h, &info)) return;
    COORD sz;
    sz.X = info.dwSize.X > 140 ? info.dwSize.X : (SHORT)140;
    sz.Y = 10000;
    SetConsoleScreenBufferSize(h, sz);
}

// Shorthand helpers — emit ANSI only when the terminal accepts it.
const char* C(const char* code) { return g_vtEnabled ? code : ""; }

// Tree glyph for indented children. Uses plain ASCII to avoid any code-
// page surprises.
void WriteIndentPrefix() {
    if (g_indent <= 0) return;
    // Each level: 2 spaces + "|  ". The deepest level gets "`- " instead
    // of "|  " for a classic tree look.
    for (int i = 0; i < g_indent - 1; ++i) {
        fprintf(stdout, "%s|  %s", C(LOG_ANSI_GRAY), C(LOG_ANSI_RESET));
    }
    fprintf(stdout, "%s`- %s", C(LOG_ANSI_GRAY), C(LOG_ANSI_RESET));
}

std::string vformat(const char* fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    int needed = _vscprintf(fmt, cp);
    va_end(cp);
    if (needed < 0) return {};
    std::string out;
    out.resize(static_cast<size_t>(needed));
    vsnprintf(&out[0], static_cast<size_t>(needed) + 1, fmt, ap);
    return out;
}

} // namespace

namespace Log {

void Banner() {
    const char* reset = C(LOG_ANSI_RESET);
    const char* cyan  = C(LOG_ANSI_BR_CYAN);
    const char* dim   = C(LOG_ANSI_GRAY);
    const char* bold  = C(LOG_ANSI_BOLD);
    const char* yel   = C(LOG_ANSI_BR_YELLOW);

    fprintf(stdout, "\n");
    fprintf(stdout, "%s  ┌──────────────────────────────────────────────────────────────┐%s\n", cyan, reset);
    fprintf(stdout, "%s  │%s                                                              %s%s│%s\n",
            cyan, reset, reset, cyan, reset);
    fprintf(stdout, "%s  │   %s%sZ E U S M O D%s    %s— Icarus internal trainer —         %s%s│%s\n",
            cyan, bold, yel, reset, dim, reset, cyan, reset);
    fprintf(stdout, "%s  │   %sreflection-driven   patch-resilient   windows x64          %s%s│%s\n",
            cyan, dim, reset, cyan, reset);
    fprintf(stdout, "%s  │                                                              %s│%s\n", cyan, cyan, reset);
    fprintf(stdout, "%s  └──────────────────────────────────────────────────────────────┘%s\n", cyan, reset);
    fprintf(stdout, "\n");
    fflush(stdout);
}

void InitConsole() {
    if (g_conOut) return;
    AllocConsole();
    SetConsoleTitleW(L"ZeusMod  -  Debug Console");
    SetConsoleOutputCP(CP_UTF8);

    freopen_s(&g_conOut, "CONOUT$", "w", stdout);
    freopen_s(&g_conOut, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    EnableVirtualTerminal();
    GrowConsoleBuffer();
    g_startTickMs = GetTickCount64();
    Banner();
}

void Line(const char* level_color, const char* level_tag,
          const char* text_color,  const char* fmt, ...) {
    char ts[32];
    FormatTimestamp(ts, sizeof(ts));

    const char* reset = C(LOG_ANSI_RESET);
    const char* dim   = C(LOG_ANSI_GRAY);
    const char* lvlC  = C(level_color);
    const char* txtC  = C(text_color);

    fprintf(stdout, "%s%s%s  %s[%-4s]%s  ",
            dim, ts, reset, lvlC, level_tag, reset);
    WriteIndentPrefix();
    fprintf(stdout, "%s", txtC);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);

    fprintf(stdout, "%s\n", reset);
    fflush(stdout);
}

void Section(const char* title) {
    const char* reset  = C(LOG_ANSI_RESET);
    const char* accent = C(LOG_ANSI_BR_MAGENTA);
    const char* dim    = C(LOG_ANSI_GRAY);
    const char* bold   = C(LOG_ANSI_BOLD);

    // Reset indent when a new top-level section starts. If nested sections
    // are ever needed they can use Group() instead.
    g_indent = 0;

    fprintf(stdout, "\n");
    fprintf(stdout, "%s  ╔══%s %s%s%s %s══", accent, reset, bold, title, reset, accent);
    // Pad the banner so it stays ~68 cols wide regardless of title length.
    int titlePadded = 62 - (int)strlen(title);
    for (int i = 0; i < titlePadded && i < 80; ++i) fputc('=', stdout);
    fprintf(stdout, "╗%s\n", reset);
    fprintf(stdout, "%s  ╚%s\n", dim, reset);
    fflush(stdout);
}

void Group(const char* title) {
    const char* reset = C(LOG_ANSI_RESET);
    const char* accent = C(LOG_ANSI_BR_CYAN);
    const char* dim   = C(LOG_ANSI_GRAY);

    // Emit the group header at current indent, then increase indent so
    // subsequent LOG_STEP / LOG_STAT lines nest under it.
    char ts[32]; FormatTimestamp(ts, sizeof(ts));
    fprintf(stdout, "%s%s%s  %s[STEP]%s  ", dim, ts, reset, accent, reset);
    WriteIndentPrefix();
    fprintf(stdout, "%s%s%s\n", accent, title, reset);
    fflush(stdout);
    ++g_indent;
}

void GroupClose() {
    if (g_indent > 0) --g_indent;
}

void Step(const char* fmt, ...) {
    char ts[32]; FormatTimestamp(ts, sizeof(ts));
    const char* reset = C(LOG_ANSI_RESET);
    const char* dim   = C(LOG_ANSI_GRAY);
    const char* col   = C(LOG_ANSI_BR_BLUE);
    fprintf(stdout, "%s%s%s  %s[STEP]%s  ", dim, ts, reset, col, reset);
    WriteIndentPrefix();
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s\n", reset);
    fflush(stdout);
}

void Stat(const char* key, const char* fmt, ...) {
    char ts[32]; FormatTimestamp(ts, sizeof(ts));
    const char* reset  = C(LOG_ANSI_RESET);
    const char* dim    = C(LOG_ANSI_GRAY);
    const char* keyCol = C(LOG_ANSI_BR_WHITE);
    const char* valCol = C(LOG_ANSI_BR_CYAN);
    fprintf(stdout, "%s%s%s  %s[STAT]%s  ", dim, ts, reset, C(LOG_ANSI_MAGENTA), reset);
    WriteIndentPrefix();
    fprintf(stdout, "%s%-24s%s  %s", keyCol, key, reset, valCol);
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s\n", reset);
    fflush(stdout);
}

namespace Resume {

void Ok(const char* category) {
    if (!category) return;
    std::string k(category);
    if (!g_counters.count(k)) g_counter_order.push_back(k);
    g_counters[k].ok++;
}

void Fail(const char* category, const char* why_fmt, ...) {
    if (!category) return;
    std::string k(category);
    if (!g_counters.count(k)) g_counter_order.push_back(k);
    auto& c = g_counters[k];
    c.fail++;
    if (why_fmt && c.fail_examples.size() < 8) {
        va_list ap; va_start(ap, why_fmt);
        c.fail_examples.push_back(vformat(why_fmt, ap));
        va_end(ap);
    }
}

void Set(const char* key, const char* value_fmt, ...) {
    if (!key || !value_fmt) return;
    va_list ap; va_start(ap, value_fmt);
    std::string v = vformat(value_fmt, ap);
    va_end(ap);
    g_values.push_back({ std::string(key), std::move(v) });
}

void Note(char severity, const char* fmt, ...) {
    if (!fmt) return;
    va_list ap; va_start(ap, fmt);
    std::string msg = vformat(fmt, ap);
    va_end(ap);
    g_notes.push_back({ severity, std::move(msg) });
}

void Reset() {
    g_counters.clear();
    g_counter_order.clear();
    g_values.clear();
    g_notes.clear();
}

} // namespace Resume

// Pad `s` to `width` with spaces (truncate if longer).
static std::string Pad(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}
static std::string Repeat(char c, int n) { return std::string(n > 0 ? n : 0, c); }

void PrintResume(const char* title) {
    if (!title) title = "Summary";

    const char* reset  = C(LOG_ANSI_RESET);
    const char* border = C(LOG_ANSI_BR_CYAN);
    const char* titleC = C(LOG_ANSI_BOLD);
    const char* okC    = C(LOG_ANSI_BR_GREEN);
    const char* warnC  = C(LOG_ANSI_BR_YELLOW);
    const char* errC   = C(LOG_ANSI_BR_RED);
    const char* dim    = C(LOG_ANSI_GRAY);
    const char* key    = C(LOG_ANSI_BR_WHITE);
    const char* val    = C(LOG_ANSI_BR_CYAN);

    constexpr int W = 78;

    auto hline = [&](const char* left, const char* mid, const char* right) {
        fprintf(stdout, "%s%s", border, left);
        for (int i = 0; i < W - 2; ++i) fprintf(stdout, "%s", mid);
        fprintf(stdout, "%s%s\n", right, reset);
    };
    auto padLine = [&](const std::string& s) {
        // s is raw content; pad to inner width W-4 (2 spaces each side).
        std::string padded = Pad(s, (size_t)(W - 4));
        fprintf(stdout, "%s│%s  %s  %s│%s\n", border, reset, padded.c_str(), border, reset);
    };
    auto padLineColored = [&](const char* colorPrefix, const std::string& visible) {
        // Same as padLine but wraps the VISIBLE content in a color. We pad
        // using the uncolored length so alignment stays correct.
        std::string padded = Pad(visible, (size_t)(W - 4));
        fprintf(stdout, "%s│%s  %s%s%s  %s│%s\n",
            border, reset, colorPrefix, padded.c_str(), reset, border, reset);
    };

    ULONGLONG now = GetTickCount64();
    ULONGLONG elapsedMs = now - g_startTickMs;

    fprintf(stdout, "\n");
    hline("╔", "═", "╗");

    // Title row
    {
        std::string t = std::string("▶  ") + title;
        std::string padded = Pad(t, (size_t)(W - 4));
        fprintf(stdout, "%s│%s  %s%s%s  %s│%s\n",
            border, reset, titleC, padded.c_str(), reset, border, reset);
    }
    hline("╠", "═", "╣");

    // Counter table
    if (!g_counter_order.empty()) {
        padLineColored(key, "▸ Checks");
        for (const auto& cat : g_counter_order) {
            const auto& c = g_counters[cat];
            int total = c.ok + c.fail;
            int pct   = total ? (int)((c.ok * 100.0) / total + 0.5) : 0;
            char buf[256];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "   %-24s %s%3d OK%s %s%3d FAIL%s  (%d%%)",
                cat.c_str(),
                okC, c.ok, reset,
                c.fail > 0 ? errC : dim, c.fail, reset,
                pct);
            // Padding: the ANSI codes don't take visible width, so we
            // compute a padded version without colors to size properly.
            char visible[256];
            _snprintf_s(visible, sizeof(visible), _TRUNCATE,
                "   %-24s %3d OK %3d FAIL  (%d%%)",
                cat.c_str(), c.ok, c.fail, pct);
            std::string padded = Pad(visible, (size_t)(W - 4));
            // Re-insert colors in the padded form by position — simpler:
            // just print `buf` and pad by spaces to reach W-4 visible chars.
            size_t vis = strlen(visible);
            std::string pad = vis < (size_t)(W - 4) ? std::string((W - 4) - vis, ' ') : std::string();
            fprintf(stdout, "%s│%s  %s%s  %s│%s\n",
                border, reset, buf, pad.c_str(), border, reset);
        }
        padLine("");
    }

    // KV values
    if (!g_values.empty()) {
        padLineColored(key, "▸ Metrics");
        for (const auto& kv : g_values) {
            char visible[256];
            _snprintf_s(visible, sizeof(visible), _TRUNCATE,
                "   %-24s %s", kv.key.c_str(), kv.value.c_str());
            char colored[384];
            _snprintf_s(colored, sizeof(colored), _TRUNCATE,
                "   %s%-24s%s %s%s%s",
                key, kv.key.c_str(), reset, val, kv.value.c_str(), reset);
            size_t vis = strlen(visible);
            std::string pad = vis < (size_t)(W - 4) ? std::string((W - 4) - vis, ' ') : std::string();
            fprintf(stdout, "%s│%s  %s%s  %s│%s\n",
                border, reset, colored, pad.c_str(), border, reset);
        }
        padLine("");
    }

    // Notes (warnings / errors / info)
    if (!g_notes.empty()) {
        padLineColored(key, "▸ Highlights");
        for (const auto& n : g_notes) {
            const char* badge = "•";
            const char* color = dim;
            if (n.severity == 'E') { badge = "✗"; color = errC;  }
            if (n.severity == 'W') { badge = "!"; color = warnC; }
            if (n.severity == 'I') { badge = "i"; color = C(LOG_ANSI_BR_CYAN); }
            std::string visible = std::string("   ") + badge + " " + n.text;
            if (visible.size() > (size_t)(W - 4)) visible.resize((size_t)(W - 4));
            std::string pad = visible.size() < (size_t)(W - 4)
                ? std::string((W - 4) - visible.size(), ' ') : std::string();
            fprintf(stdout, "%s│%s  %s%s%s%s  %s│%s\n",
                border, reset, color, visible.c_str(), reset, pad.c_str(), border, reset);
        }

        // Brief per-category failure hints (first 2)
        bool anyHints = false;
        for (const auto& cat : g_counter_order) {
            const auto& c = g_counters[cat];
            if (c.fail_examples.empty()) continue;
            if (!anyHints) { padLine(""); anyHints = true; }
            for (size_t i = 0; i < c.fail_examples.size() && i < 3; ++i) {
                std::string visible = std::string("     ") + cat + ": " + c.fail_examples[i];
                if (visible.size() > (size_t)(W - 4)) visible.resize((size_t)(W - 4));
                std::string pad = visible.size() < (size_t)(W - 4)
                    ? std::string((W - 4) - visible.size(), ' ') : std::string();
                fprintf(stdout, "%s│%s  %s%s%s%s  %s│%s\n",
                    border, reset, dim, visible.c_str(), reset, pad.c_str(), border, reset);
            }
        }
        padLine("");
    }

    // Footer
    {
        char visible[128];
        _snprintf_s(visible, sizeof(visible), _TRUNCATE,
            "ready in %llu ms", (unsigned long long)elapsedMs);
        char colored[256];
        _snprintf_s(colored, sizeof(colored), _TRUNCATE,
            "%s%s%s", okC, visible, reset);
        size_t vis = strlen(visible);
        std::string pad = vis < (size_t)(W - 4) ? std::string((W - 4) - vis, ' ') : std::string();
        fprintf(stdout, "%s│%s  %s%s  %s│%s\n",
            border, reset, colored, pad.c_str(), border, reset);
    }

    hline("╚", "═", "╝");
    fprintf(stdout, "\n");
    fflush(stdout);

    Resume::Reset();
}

} // namespace Log
