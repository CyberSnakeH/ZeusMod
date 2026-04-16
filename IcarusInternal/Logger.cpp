#include "Logger.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace {

FILE* g_conOut = nullptr;
bool  g_vtEnabled = false;

// Pull a millisecond-precision HH:MM:SS.mmm timestamp without pulling in
// <chrono> — this file is part of a DLL injected into the game, so we keep
// dependencies minimal and the cost per line negligible.
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
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004. Guard against older
    // SDK headers that may not define it.
    constexpr DWORD kEnableVT = 0x0004;
    if (SetConsoleMode(h, mode | kEnableVT)) {
        g_vtEnabled = true;
    }
}

void GrowConsoleBuffer() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(h, &info)) return;
    COORD sz;
    sz.X = info.dwSize.X > 120 ? info.dwSize.X : (SHORT)120;
    sz.Y = 10000;
    SetConsoleScreenBufferSize(h, sz);
}

} // namespace

namespace Log {

void Banner() {
    // Box-drawing characters require UTF-8 + a console font that supports
    // them. We set the code page in InitConsole, so this is safe.
    // Keep width at 64 columns so the banner fits a default console window.
    const char* reset = g_vtEnabled ? LOG_ANSI_RESET : "";
    const char* cyan  = g_vtEnabled ? LOG_ANSI_BR_CYAN : "";
    const char* dim   = g_vtEnabled ? LOG_ANSI_GRAY  : "";
    const char* bold  = g_vtEnabled ? LOG_ANSI_BOLD  : "";

    fprintf(stdout, "\n");
    fprintf(stdout, "%s  +============================================================+%s\n", cyan, reset);
    fprintf(stdout, "%s  |%s%s                                                            %s%s|%s\n",
            cyan, bold, cyan, reset, cyan, reset);
    fprintf(stdout, "%s  |   %s%sZ E U S M O D%s%s         Internal Trainer for Icarus      %s|%s\n",
            cyan, bold, LOG_ANSI_BR_YELLOW, reset, cyan, cyan, reset);
    fprintf(stdout, "%s  |   %sReflection-driven  /  Patch-resilient  /  Windows x64 %s|%s\n",
            cyan, dim, cyan, reset);
    fprintf(stdout, "%s  |                                                            |%s\n", cyan, reset);
    fprintf(stdout, "%s  +============================================================+%s\n", cyan, reset);
    fprintf(stdout, "\n");
    fflush(stdout);
}

void InitConsole() {
    if (g_conOut) return; // idempotent

    AllocConsole();
    SetConsoleTitleW(L"ZeusMod  -  Debug Console");
    SetConsoleOutputCP(CP_UTF8);

    freopen_s(&g_conOut, "CONOUT$", "w", stdout);
    freopen_s(&g_conOut, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    EnableVirtualTerminal();
    GrowConsoleBuffer();
    Banner();
}

void Line(const char* level_color,
          const char* level_tag,
          const char* text_color,
          const char* fmt, ...) {
    char ts[32];
    FormatTimestamp(ts, sizeof(ts));

    const char* reset = g_vtEnabled ? LOG_ANSI_RESET : "";
    const char* dim   = g_vtEnabled ? LOG_ANSI_GRAY  : "";
    const char* lvlC  = g_vtEnabled ? level_color    : "";
    const char* txtC  = g_vtEnabled ? text_color     : "";

    fprintf(stdout, "%s%s%s  %s[%-5s]%s  %s",
            dim, ts, reset,
            lvlC, level_tag, reset,
            txtC);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);

    fprintf(stdout, "%s\n", reset);
    fflush(stdout);
}

void Section(const char* title) {
    const char* reset = g_vtEnabled ? LOG_ANSI_RESET      : "";
    const char* accent= g_vtEnabled ? LOG_ANSI_BR_MAGENTA : "";
    const char* dim   = g_vtEnabled ? LOG_ANSI_GRAY       : "";

    fprintf(stdout, "\n");
    fprintf(stdout, "%s  --<  %s%s%s  >--%s\n", accent, LOG_ANSI_BOLD, title, accent, reset);
    fprintf(stdout, "%s  ----------------------------------------------------------%s\n",
            dim, reset);
    fflush(stdout);
}

} // namespace Log
