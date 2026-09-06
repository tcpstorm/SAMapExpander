#include <plugin.h>
#include <cstdarg>
#include <cstdio>
#include "Log.h"

#ifndef MAPEXPANDER_VERSION
#define MAPEXPANDER_VERSION "dev"
#endif
#ifdef NDEBUG
#define MAPEXPANDER_CONFIG "release"
#else
#define MAPEXPANDER_CONFIG "debug build"
#endif

static FILE* gLog = nullptr;
static bool gDebug = false;

namespace Log {
    void Open() {
        gLog = fopen(PLUGIN_PATH("MapExpander.log"), "w");
        Write("MapExpander " MAPEXPANDER_VERSION " (" MAPEXPANDER_CONFIG ")");
    }

    // Seconds since the log was opened, in front of every line, so a slow load says where the
    // time went rather than only that it went.
    static void Stamp() {
        static const unsigned long long start = GetTickCount64();
        fprintf(gLog, "[%8.3f] ", (double)(GetTickCount64() - start) / 1000.0);
    }

    void Write(const char* fmt, ...) {
        if (!gLog) return;
        Stamp();
        va_list args;
        va_start(args, fmt);
        vfprintf(gLog, fmt, args);
        va_end(args);
        fputc('\n', gLog);
        fflush(gLog);
    }

    void Debug(const char* fmt, ...) {
        if (!gLog || !gDebug) return;
        Stamp();
        va_list args;
        va_start(args, fmt);
        vfprintf(gLog, fmt, args);
        va_end(args);
        fputc('\n', gLog);
        fflush(gLog);
    }

    void SetDebug(bool on) {
        gDebug = on;
        if (on) Write("debug logging on ([World] Debug=1)");
    }

    bool DebugEnabled() { return gDebug; }
}
