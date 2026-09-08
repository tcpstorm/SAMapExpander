// Melee never picks its victims out of the world at hit time: CTaskSimpleFight's hit routine
// (0x6240B0) walks CPedIntelligence's ped scanner (+0x130) and vehicle scanner (+0xE0) arrays,
// both filled by CEntityScanner::ScanForEntitiesInRange (0x5FFA20). Only the objects it can hit
// come from CWorld::FindObjectsInRange, which is why punching a lamp post still works out there.
//
// The scanner turns the ped's position into sector indices with the usual floor(c * 0.02 + 60),
// but clamps them into the vanilla 0..119 grid *before* masking them down to the 16x16 repeat
// sector array - while CPhysical::Add (0x544A30) masks the raw index. Past +3000 the clamped
// upper bound drops below the unclamped lower bound and the scan loop body never runs at all, so
// both scanners stay empty and every swing passes through. Past -3000 the lower bound is raised
// to 0 above the upper bound with the same result.
//
// Removing the four clamps leaves the loop bounded by the scan range as before (its span is
// 2 * range / 50, independent of position) and makes the search wrap exactly like the insert.
#include <plugin.h>
#include <cstdint>
#include "MeleeFix.h"
#include "MemPatch.h"
#include "Log.h"

// The clamp branches inside CEntityScanner::ScanForEntitiesInRange. Each is a short conditional
// jump over the "use the computed index" path and into a "use 0 / use 119" store.
struct ClampSite {
    uintptr_t address;
    uint8_t expected[2];
    const char* what;
};

static const ClampSite CLAMP_SITES[] = {
    { 0x5FFAEE, { 0x7E, 0x2B }, "min x" },   // jle -> store 0
    { 0x5FFB4B, { 0x7E, 0x2A }, "min y" },   // jle -> store 0
    { 0x5FFBA5, { 0x7D, 0x2B }, "max x" },   // jge -> store 119
    { 0x5FFC03, { 0x7D, 0x2C }, "max y" },   // jge -> store 119
};

static const uint8_t TWO_NOPS[2] = { 0x90, 0x90 };

namespace MeleeFix {
    void Install() {
        int patched = 0;
        for (const ClampSite& site : CLAMP_SITES) {
            if (MemPatch::ReplaceBytes(site.address, site.expected, TWO_NOPS, sizeof(TWO_NOPS)))
                patched++;
            else
                Log::Write("melee beyond map: %s clamp at 0x%X left alone", site.what, (unsigned)site.address);
        }
        Log::Write("melee beyond map: %d/%u sector clamps removed from the ped/vehicle scanner",
                   patched, (unsigned)(sizeof(CLAMP_SITES) / sizeof(CLAMP_SITES[0])));
    }
}
