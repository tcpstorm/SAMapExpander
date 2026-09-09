// See PathLoadProbe.h.
//
// CStreaming::ConvertBufferToObject (0x40C6B0, cdecl(char* buffer, int id)) is where a finished
// read becomes an object; for an id in the DAT range it opens a memory stream over the buffer and
// calls CPathFind::LoadPathFindData(stream, area), which RwStreamReads the file's five header
// counts straight into m_anNumNodes[area], m_anNumVehicleNodes[area], m_anNumPedNodes[area],
// m_anNumCarPathLinks[area] and m_anNumAddresses[area] (ThePaths + 0xFA4, 0x10C4, 0x11E4, 0x1304,
// 0x1424), mallocs from them, reads the arrays, and finally re-applies every stored switched-off
// road region to the new area through SwitchRoadsOffInAreaForOneRegion (0x452820) - whose loop
// bound at 0x45285A is those same counts, read back.
//
// So the first twenty bytes of the buffer, logged before the call, say whether the streamer
// delivered the path file at all; the counts logged after the call say whether they survived the
// load; and a mid-function hook on the loop bound stops the walk when the count is not one a path
// file could hold, which is the difference between a crash and a log line.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cstdint>
#include <cstring>
#include "PathLoadProbe.h"
#include "ModelLimits.h"
#include "Log.h"

namespace {

constexpr uintptr_t kConvertBufferToObject = 0x40C6B0;
constexpr uintptr_t kThePaths = 0x96F050;
constexpr uintptr_t kLoopBoundSite = 0x45285A;   // cmp ecx, eax in SwitchRoadsOffInAreaForOneRegion
constexpr int kAreas = 64;
// The largest of San Andreas' 64 files holds well under 5000 nodes; a count past this is not a
// path file's.
constexpr unsigned kCredibleNodes = 20000;

SafetyHookInline& ConvertHook() { static SafetyHookInline h; return h; }
SafetyHookMid& BoundHook() { static SafetyHookMid h; return h; }
int gSkippedAreas = 0;

unsigned Count(uintptr_t table, int area) { return *(unsigned*)(kThePaths + table + (uintptr_t)area * 4); }

void LogCounts(const char* when, int area) {
    Log::Write("paths: area %d %s: nodes %u, vehicle %u, ped %u, links %u, addresses %u, node array %08X",
               area, when, Count(0xFA4, area), Count(0x10C4, area), Count(0x11E4, area),
               Count(0x1304, area), Count(0x1424, area), *(unsigned*)(kThePaths + 0x804 + (uintptr_t)area * 4));
}

bool __cdecl Hooked_ConvertBufferToObject(char* buffer, int id) {
    int area = id - ModelLimits::DatBase();
    bool isPath = area >= 0 && area < kAreas;
    if (isPath) {
        unsigned h[5];
        memcpy(h, buffer, sizeof(h));
        unsigned char sig[4];
        memcpy(sig, buffer, 4);
        Log::Write("paths: area %d arriving as id %d, buffer %08X, header %u %u %u %u %u (bytes %02X %02X %02X %02X)",
                   area, id, (unsigned)(uintptr_t)buffer, h[0], h[1], h[2], h[3], h[4],
                   sig[0], sig[1], sig[2], sig[3]);
        LogCounts("before", area);
    }
    bool r = ConvertHook().ccall<bool>(buffer, id);
    if (isPath) LogCounts("after", area);
    return r;
}

void BoundGuard(SafetyHookContext& ctx) {
    unsigned start = ctx.ecx, end = ctx.eax;
    int area = (int)ctx.edx;
    if (end <= kCredibleNodes && start <= end) return;
    if (gSkippedAreas < 20) {
        gSkippedAreas++;
        Log::Write("paths: area %d has an impossible node count (%u..%u) when the switched-off "
                   "roads are re-applied; the walk is skipped rather than crash. This area's path "
                   "data is corrupt in memory - see the arriving/before/after lines above",
                   area, start, end);
    }
    ctx.eax = ctx.ecx;   // nothing to walk
}

}   // namespace

namespace PathLoadProbe {

void Install() {
    const uint8_t expectConvert[] = { 0x83, 0xEC, 0x20, 0x8B, 0x4C, 0x24, 0x24, 0x53 };
    const uint8_t expectBound[] = { 0x3B, 0xC8, 0x0F, 0x8D };
    if (memcmp((void*)kConvertBufferToObject, expectConvert, sizeof(expectConvert)) != 0 ||
        memcmp((void*)kLoopBoundSite, expectBound, sizeof(expectBound)) != 0) {
        Log::Write("paths: probe not installed - the code it hooks does not read as expected");
        return;
    }
    ConvertHook() = safetyhook::create_inline(reinterpret_cast<void*>(kConvertBufferToObject),
                                              reinterpret_cast<void*>(&Hooked_ConvertBufferToObject));
    BoundHook() = safetyhook::create_mid(reinterpret_cast<void*>(kLoopBoundSite), &BoundGuard);
    Log::Write("paths: probe installed - every path area load is logged with its buffer header, "
               "and a node count over %u stops the switched-off-roads walk instead of crashing",
               kCredibleNodes);
}

}   // namespace PathLoadProbe
