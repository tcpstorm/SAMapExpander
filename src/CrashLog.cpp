// Logs the first fatal exception (address, owning module and offset, registers, raw stack) to
// MapExpander.log, then lets the game's own handler continue. Single player has no crash dump
// of its own; with the plugin's symbols the module offset resolves straight to a function.
#include <windows.h>
#include <cstdio>
#include "CrashLog.h"
#include "Log.h"

static LONG gLogged = 0;

static void DescribeAddress(const void* address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module) {
        char path[MAX_PATH] = { 0 };
        GetModuleFileNameA(module, path, sizeof(path));
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%p = %s+0x%X (base %p)", address, name,
                 (unsigned)((uintptr_t)address - (uintptr_t)module), (void*)module);
    } else {
        snprintf(out, size, "%p (no module)", address);
    }
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&gLogged, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    const CONTEXT& c = *info->ContextRecord;
    char where[256];
    DescribeAddress(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    Log::Write("CRASH: exception 0x%08X at %s", (unsigned)code, where);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        Log::Write("CRASH: %s address 0x%08X",
                   info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                   (unsigned)info->ExceptionRecord->ExceptionInformation[1]);
    Log::Write("CRASH: eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X",
               (unsigned)c.Eax, (unsigned)c.Ebx, (unsigned)c.Ecx, (unsigned)c.Edx,
               (unsigned)c.Esi, (unsigned)c.Edi, (unsigned)c.Ebp, (unsigned)c.Esp);

    const DWORD* stack = reinterpret_cast<const DWORD*>(c.Esp);
    for (int row = 0; row < 12; row++) {
        char line[160];
        int n = snprintf(line, sizeof(line), "CRASH: esp+%03X:", row * 32);
        for (int i = 0; i < 8; i++) {
            DWORD value = 0;
            if (IsBadReadPtr(stack + row * 8 + i, sizeof(DWORD))) break;
            value = stack[row * 8 + i];
            n += snprintf(line + n, sizeof(line) - n, " %08X", (unsigned)value);
        }
        Log::Write("%s", line);
    }
    // Return addresses into our own module are the ones worth resolving.
    for (int i = 0; i < 96; i++) {
        if (IsBadReadPtr(stack + i, sizeof(DWORD))) break;
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(stack[i]), &module) && module == GetModuleHandleA("MapExpander.SA.asi")) {
            char text[256];
            DescribeAddress(reinterpret_cast<void*>(stack[i]), text, sizeof(text));
            Log::Write("CRASH: stack[%d] -> %s", i, text);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace CrashLog {
    void Install() {
        AddVectoredExceptionHandler(1, OnException);
    }
}
