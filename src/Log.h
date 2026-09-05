#pragma once

namespace Log {
    void Open();
    // Always written: what was configured, what was patched, and anything that went wrong.
    void Write(const char* fmt, ...);
    // Written only with [World] Debug=1: byte dumps, per-hook traces, per-tile loads and the like.
    void Debug(const char* fmt, ...);
    void SetDebug(bool on);
    bool DebugEnabled();
}
