#include <plugin.h>
#include <cstdarg>
#include <cstdio>
#include "Log.h"

static FILE* gLog = nullptr;

namespace Log {
    void Open() {
        gLog = fopen(PLUGIN_PATH("MapExpander.log"), "w");
    }

    void Write(const char* fmt, ...) {
        if (!gLog) return;
        va_list args;
        va_start(args, fmt);
        vfprintf(gLog, fmt, args);
        va_end(args);
        fputc('\n', gLog);
        fflush(gLog);
    }
}
