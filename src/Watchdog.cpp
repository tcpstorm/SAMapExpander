// Where is the game when a frame stops finishing?
//
// Three hangs were diagnosed from full-memory dumps taken from outside the process, and all three
// were guesses. A dump written by a 64-bit tool of a 32-bit process stores the WOW64 thread context,
// not the 32-bit one, so there is no instruction pointer for the thread that matters and no stack
// pointer either - which leaves scanning the stack for values that look like return addresses and
// hoping the innermost one is live rather than left over from an earlier, deeper call. That
// produced two different answers for what is probably one bug.
//
// From inside the process there is no such problem: GetThreadContext on the game's own thread
// returns the real Eip and Esp. So run a thread that watches the frame counter, and when it stops
// moving, suspend the main thread, write down exactly where it is, and let it go again. A spin
// shows up as the same Eip in every sample; slow progress shows up as a moving one.
//
// Two things this deliberately does not do:
//
//   * It does not use Log::Write. The main thread is suspended while the report is written, and if
//     it happened to be inside the C runtime holding the lock on the log's FILE*, writing from here
//     would deadlock against a thread that cannot run to release it. The report goes to its own
//     file through raw WriteFile, which takes no lock this process owns.
//
//   * It does not try to be clever about walking frames. Esp is known, so the return addresses on
//     the stack really are above it - no guessing which cluster is live, which is the whole reason
//     the dumps were ambiguous.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <safetyhook.hpp>
#include "Watchdog.h"
#include "Settings.h"
#include "Log.h"

namespace {

volatile LONG gTick = 0;
HANDLE gMainThread = nullptr;
HANDLE gReport = INVALID_HANDLE_VALUE;

// Seconds a frame may take before the game is presumed stuck. Loading a region's collision blocks
// for a while quite legitimately, so this is well past anything normal.
const int kStallSeconds = 6;
const int kMaxReports = 40;

void Say(const char* fmt, ...) {
    if (gReport == INVALID_HANDLE_VALUE) return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\r';
    line[n++] = '\n';
    DWORD written = 0;
    WriteFile(gReport, line, (DWORD)n, &written, nullptr);
    FlushFileBuffers(gReport);
}

void Describe(uintptr_t address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module) {
        char path[MAX_PATH] = { 0 };
        GetModuleFileNameA(module, path, sizeof(path));
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        snprintf(out, size, "%s+0x%X", name, (unsigned)(address - (uintptr_t)module));
    } else {
        snprintf(out, size, "(no module)");
    }
}

// Everything on the stack above Esp that lands in a module's code. With a real Esp these are the
// live frames, in order, innermost first.
void ReportStack(const CONTEXT& c) {
    char where[160];
    Describe(c.Eip, where, sizeof(where));
    Say("  eip %08X  %s", (unsigned)c.Eip, where);
    Say("  esp %08X  ebp %08X  eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X",
        (unsigned)c.Esp, (unsigned)c.Ebp, (unsigned)c.Eax, (unsigned)c.Ebx,
        (unsigned)c.Ecx, (unsigned)c.Edx, (unsigned)c.Esi, (unsigned)c.Edi);

    const DWORD* stack = (const DWORD*)c.Esp;
    int shown = 0;
    for (int i = 0; i < 1024 && shown < 24; i++) {
        if (IsBadReadPtr(stack + i, sizeof(DWORD))) break;
        DWORD v = stack[i];
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)v, &module) || !module)
            continue;
        // Only bytes that a call could have pushed: the instruction before has to be one.
        const unsigned char* pre = (const unsigned char*)(uintptr_t)v;
        if (IsBadReadPtr(pre - 8, 8)) continue;
        bool isCall = pre[-5] == 0xE8;
        if (!isCall)
            for (int back = 2; back <= 7 && !isCall; back++)
                if (pre[-back] == 0xFF && ((pre[-back + 1] >> 3) & 7) == 2) isCall = true;
        if (!isCall) continue;
        Describe(v, where, sizeof(where));
        Say("    esp+%04X  %08X  %s", (unsigned)(i * 4), (unsigned)v, where);
        shown++;
    }
}

// CStreaming::RetryLoadFile (0x4076C0) loops until ms_channelError goes back to -1, reissuing the
// channel's read through CdStreamRead each time round. A read that cannot succeed therefore stops
// the game dead with no crash and no exception - which is the shape of every hang seen so far. When
// the watchdog catches a stall, write down what the streamer is actually asking for: the error
// flag, and each channel's state, the position it is reading from and how much of it.
//
// The position packs the archive index in its top byte and the sector in the rest, so it says which
// of the registered IMGs the failing read is aimed at - the game's own, or ours.
const uintptr_t kChannelError = 0x8E4B90;   // -1 when nothing has failed
const uintptr_t kChannelBase = 0x8E4AE0;    // state, then the read's position and size
const uintptr_t kChannelStride = 0x98;
const int kChannels = 2;

struct ReadRecord {
    int streamId;
    unsigned position;
    unsigned sectors;
};
const int kRing = 24;
ReadRecord gRing[kRing];
volatile LONG gRingHead = 0;      // total reads ever; index is head % kRing
volatile LONG gRetryCalls = 0;    // RetryLoadFile entries, to show whether it is spinning

// Everything worth knowing about the streamer, taken in one go while the game thread is stopped.
struct StreamSnapshot {
    int error;
    LONG retries;
    LONG head;
    ReadRecord ring[kRing];
};

void TakeStreamSnapshot(StreamSnapshot& s) {
    s.error = IsBadReadPtr((void*)kChannelError, 4) ? -2 : *(const int*)kChannelError;
    s.retries = gRetryCalls;
    s.head = gRingHead;
    memcpy(s.ring, gRing, sizeof(s.ring));
}

void ReportStreaming(const StreamSnapshot& s) {
    Say("  streaming: ms_channelError = %d%s, RetryLoadFile entered %ld time(s)", s.error,
        s.error == -1 ? " (nothing failed)" : "  <- the retry loop only ends when this is -1",
        s.retries);
    Say("  last read(s) issued, most recent first (archive 5 is the region's):");
    const int have = s.head < kRing ? (int)s.head : kRing;
    for (int i = 0; i < have; i++) {
        const ReadRecord& r = s.ring[(s.head - 1 - i) % kRing];
        Say("    stream %d: %u sector(s) at sector %u of ARCHIVE %u  (position %08X)",
            r.streamId, r.sectors, r.position & 0x00FFFFFFu, r.position >> 24, r.position);
    }
}

// Reading these globals from the watchdog thread gives an incoherent answer: the game is resumed
// before the report is written, so the values move under it - the first attempt read
// ms_channelError as -1 while the game was demonstrably inside the loop that only runs when it is
// not. Read them on the game's own thread instead, at the one moment they are guaranteed to mean
// something: the entry to CStreaming::RetryLoadFile, which is only ever reached once a read has
// already failed.
//
// The channel's position packs the archive index in its top byte and the sector in the rest, so
// this says outright whether the failing read is aimed at one of San Andreas' own archives or at
// ours - registered as archive 5 - and at which sector, which the region's IMG directory turns
// back into a file name.
typedef void(__cdecl* RetryLoadFileFn)(int channel);
static SafetyHookInline& RetryHook() { static SafetyHookInline h; return h; }

// Guessing at the channel struct's layout gave nonsense - the field eight bytes in held a pointer
// into this very module, not a disc position, so the offsets are wrong and the archive index
// derived from them (114) is meaningless. The arguments to CdStreamRead are not a guess: the
// retry loop passes the position and size straight to it at 0x40774F, so read them there.
//
//   CdStreamRead(int streamId, void* buffer, unsigned position, unsigned sectors)
//
// with the archive index in the position's top byte. Logged only while a channel error is
// outstanding, which is exactly when the read is one that keeps failing.
// Logging a read the moment a channel error is outstanding does not work: an error outstanding is
// ordinary during loading, and the whole budget was spent on start-up reads of San Andreas' own
// archives before the game had even been played. What distinguishes the hang is not that a read
// failed, it is that the same read keeps being reissued and the frame never ends.
//
// So record every read into a ring - four words, no logging, nothing that can block - and let the
// watchdog print the ring when it decides the game has stopped. The last few reads before a stall
// are the ones being retried, and the ring is snapshotted while the game thread is suspended, so
// unlike the first attempt at this the values cannot move while they are being written out.
typedef int(__cdecl* CdStreamReadFn)(int, void*, unsigned, unsigned);
static SafetyHookInline& ReadHook() { static SafetyHookInline h; return h; }


int __cdecl Hooked_CdStreamRead(int streamId, void* buffer, unsigned position, unsigned sectors) {
    const LONG slot = InterlockedIncrement(&gRingHead) - 1;
    ReadRecord& r = gRing[slot % kRing];
    r.streamId = streamId;
    r.position = position;
    r.sectors = sectors;
    return ReadHook().ccall<int>(streamId, buffer, position, sectors);
}

void __cdecl Hooked_RetryLoadFile(int channel) {
    InterlockedIncrement(&gRetryCalls);
    RetryHook().ccall<void>(channel);
}

DWORD WINAPI WatchdogProc(void*) {
    LONG last = -1;
    int stalledFor = 0, reports = 0, sample = 0;
    for (;;) {
        Sleep(1000);
        LONG now = gTick;
        if (now != last) {
            last = now;
            stalledFor = 0;
            continue;
        }
        if (++stalledFor < kStallSeconds || reports >= kMaxReports) continue;

        // A game that is not the foreground window stops finishing frames on purpose: San Andreas
        // drops into an input-poll loop inside WinMain, and Present blocks in the driver. Both look
        // exactly like a stall from here, and both happen every time the player alts away to say
        // the game has hung - which is when this is most likely to be running. Only report while
        // the game actually has the screen.
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        if (owner != GetCurrentProcessId()) {
            stalledFor = 0;
            continue;
        }

        // Sample it several times a second apart: the same eip every time is a spin, a moving one
        // is work that is merely enormous, and the difference decides where to look next.
        reports++;
        sample++;
        if (SuspendThread(gMainThread) == (DWORD)-1) continue;
        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        BOOL ok = GetThreadContext(gMainThread, &c);
        StreamSnapshot snap;
        TakeStreamSnapshot(snap);
        ResumeThread(gMainThread);      // resume before writing anything, so the report cannot hold it

        Say("");
        Say("--- sample %d: no frame finished for %d seconds ---", sample, stalledFor);
        if (ok) ReportStack(c);
        else Say("  could not read the thread context (error %u)", (unsigned)GetLastError());
        ReportStreaming(snap);
        stalledFor = 0;                 // wait out another window before the next sample
    }
}

}  // namespace

namespace Watchdog {

void Tick() { InterlockedIncrement(&gTick); }

void Install() {
    if (!Settings::world.debug) return;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &gMainThread, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log::Write("watchdog: could not take a handle to the game thread (error %u); stalls will "
                   "not be reported", (unsigned)GetLastError());
        return;
    }

    char path[MAX_PATH] = { 0 };
    GetModuleFileNameA(nullptr, path, sizeof(path));
    char* slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "MapExpander.stall.txt");
    gReport = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gReport == INVALID_HANDLE_VALUE) {
        Log::Write("watchdog: cannot write %s (error %u)", path, (unsigned)GetLastError());
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, &WatchdogProc, nullptr, 0, nullptr);
    if (!thread) {
        Log::Write("watchdog: could not start (error %u)", (unsigned)GetLastError());
        return;
    }
    CloseHandle(thread);
    ReadHook() = safetyhook::create_inline(reinterpret_cast<void*>(0x406A20),
                                           reinterpret_cast<void*>(&Hooked_CdStreamRead));
    if (!ReadHook())
        Log::Write("watchdog: could not hook CdStreamRead");

    RetryHook() = safetyhook::create_inline(reinterpret_cast<void*>(0x4076C0),
                                            reinterpret_cast<void*>(&Hooked_RetryLoadFile));
    if (!RetryHook())
        Log::Write("watchdog: could not hook CStreaming::RetryLoadFile; a stalled streaming read "
                   "will not say which archive it is aimed at");

    Log::Write("watchdog: watching for frames that take more than %d seconds; anything it catches "
               "goes to MapExpander.stall.txt", kStallSeconds);
}

}  // namespace Watchdog
