// Logs the first fatal exception (address, owning module and offset, registers, raw stack) to
// MapExpander.log and writes MapExpander.dmp beside it, then lets the game's own handler continue.
// Single player has no crash dump of its own; with the plugin's symbols the module offset resolves
// straight to a function, and the dump carries the stacks the log has no room for.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <safetyhook.hpp>
#include "CrashLog.h"
#include "RenderGuard.h"
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

// A minidump next to the log. The raw stack below is 0x180 bytes and names only return addresses
// that happen to still be on it - useless when a wild indirect call lands somewhere with no module,
// which leaves nothing to walk back from. A dump carries every thread's stack and the module list,
// so the call chain can be recovered afterwards without a debugger attached: San Andreas takes the
// screen exclusively, and breaking in a debugger while it holds it is its own fight.
//
// dbghelp is loaded on demand so the plugin never depends on it, and MiniDumpNormal is enough -
// thread stacks and modules, a few MB, not the hundreds a full-memory dump of the streamer costs.
static void WriteMiniDump(EXCEPTION_POINTERS* info) {
    HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    if (!dbghelp) return;
    struct DumpExceptionInfo {
        DWORD threadId;
        EXCEPTION_POINTERS* pointers;
        BOOL clientPointers;
    };
    typedef BOOL(WINAPI* WriteDumpFn)(HANDLE, DWORD, HANDLE, DWORD, DumpExceptionInfo*, void*, void*);
    WriteDumpFn writeDump = reinterpret_cast<WriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!writeDump) return;

    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, path, sizeof(path));      // the game exe, so the dump lands beside it
    char* slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "MapExpander.dmp");

    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log::Write("CRASH: cannot write %s (error %u)", path, (unsigned)GetLastError());
        return;
    }
    DumpExceptionInfo dumpInfo = { GetCurrentThreadId(), info, FALSE };
    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, 0, &dumpInfo, nullptr, nullptr);
    CloseHandle(file);
    Log::Write(ok ? "CRASH: minidump written to %s" : "CRASH: minidump failed (%s)", path);
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Anything with NTSTATUS' error severity (0xCxxxxxxx) is fatal. Listing the codes by hand
    // missed the ones that matter most here: heap corruption (0xC0000374), raised by the allocator
    // long after whatever overwrote the block, and stack buffer overrun (0xC0000409). A run that
    // died with neither a log line nor a dump is what found this. Codes outside that severity are
    // left alone - 0xE06D7363 is an ordinary C++ throw and 0x406D1388 is a debugger naming a
    // thread, and catching the first of those would use up the one report we get.
    if ((code & 0xF0000000u) != 0xC0000000u)
        return EXCEPTION_CONTINUE_SEARCH;
    // IsBadReadPtr and friends work by touching the page and catching the fault themselves, and
    // a vectored handler sees that fault first. The watchdog probes stack values with it, one of
    // which is the executable's own base - a probe eight bytes below that is unmapped - and the
    // resulting "crash" in KERNEL32 used up the one report this handler makes, leaving the real
    // crash a minute later unrecorded. A fault raised inside the system DLLs is theirs to handle.
    {
        HMODULE faulting = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &faulting) && faulting) {
            char path[MAX_PATH] = { 0 };
            GetModuleFileNameA(faulting, path, sizeof(path));
            const char* name = strrchr(path, '\\');
            name = name ? name + 1 : path;
            if (_stricmp(name, "KERNEL32.DLL") == 0 || _stricmp(name, "KERNELBASE.dll") == 0 ||
                _stricmp(name, "ntdll.dll") == 0)
                return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    // (Two builds tried to recover from this render fault here by unwinding the stack to RenderEntity.
    // Neither held: the first assumed a return address at esp and never fired, the second matched the
    // exact return, fired once, and then the frame faulted again inside this module. VEH stack surgery
    // over a live RenderWare frame is not survivable, so this handler only reports. The cure is to
    // convert the geometry into San Andreas RenderWare form in the importer, not to catch the fault.)

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
    const int rendering = RenderGuard::LastRenderedModel();
    if (rendering >= 0)
        Log::Write("CRASH: RenderEntity was drawing model %d when this happened", rendering);

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
    WriteMiniDump(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Two runs ended with no exception at all: no crash line, no minidump, the log stopping mid-frame,
// and the process simply gone. The handler above sees every fatal exception code, so whatever ends
// the process is not raising one - it is asking to be ended. San Andreas does that in a handful of
// places (the loop in WinMain reads RsGlobal.quit at 0xC17050, and the message-and-quit screen at
// 0x731520 ends with a write to it), and another ASI could equally call ExitProcess itself.
//
// So log the request and who made it. __builtin_return_address gives the instruction after the
// call, which lands the caller to within one instruction: an address inside gta_sa.exe names the
// game's own quit path, one inside another module names the mod that decided to leave. Both hooks
// pass the call straight through afterwards; they only watch.
static SafetyHookInline& ExitHook() { static SafetyHookInline h; return h; }
static SafetyHookInline& TerminateHook() { static SafetyHookInline h; return h; }

static void LogCaller(const char* what, unsigned code, void* caller) {
    char where[256];
    DescribeAddress(caller, where, sizeof(where));
    Log::Write("EXIT: %s(%u) called from %s", what, code, where);
}

static void WINAPI Hooked_ExitProcess(UINT code) {
    LogCaller("ExitProcess", (unsigned)code, __builtin_return_address(0));
    ExitHook().stdcall<void>(code);
}

static BOOL WINAPI Hooked_TerminateProcess(HANDLE process, UINT code) {
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        LogCaller("TerminateProcess", (unsigned)code, __builtin_return_address(0));
    return TerminateHook().stdcall<BOOL>(process, code);
}

namespace CrashLog {
    void Install() {
        AddVectoredExceptionHandler(1, OnException);

        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (!k32) return;
        if (void* p = (void*)GetProcAddress(k32, "ExitProcess"))
            ExitHook() = safetyhook::create_inline(p, reinterpret_cast<void*>(&Hooked_ExitProcess));
        if (void* p = (void*)GetProcAddress(k32, "TerminateProcess"))
            TerminateHook() = safetyhook::create_inline(p, reinterpret_cast<void*>(&Hooked_TerminateProcess));
    }
}
