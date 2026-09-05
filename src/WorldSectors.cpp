// San Andreas keeps every static entity in two fixed grids - 120 x 120 sectors of 50 units, and
// 30 x 30 of 200 units for big buildings - both covering exactly +/-3000. CEntity::Add clamps an
// entity's bounding box into that box before working out which sectors it belongs to, so a region
// placed further out is filed against the edge of the map instead of where it is, and everything
// that later looks for geometry - the renderer's scan, the streamer's request scan, line of sight,
// the ped scanners - looks in the wrong place and finds nothing. The city loads, and there is a
// hole in the world where it should be.
//
// Every one of those lookups computes its index the same way:
//
//     index = (int)(coord * SCALE + CENTRE)
//
// reading SCALE and CENTRE from .rdata (0.02 / 60 for the fine grid, 0.005 / 15 for the coarse
// one). x87 cannot take a float immediate, so a conversion has to load those globals - which means
// the set of sites is findable by pattern and, more importantly, complete. Halving a scale doubles
// the ground its grid covers without changing the array at all: 120 sectors of 50 units become 120
// of 100, and the world reaches +/-6000. CENTRE stays as it is, because index 60 is still the
// middle of a 120-entry grid.
//
// How far to go is decided by the region rather than fixed. The grid has to reach past the furthest
// thing in it: a coordinate beyond the end indexes off the array and the game walks whatever floats
// lie past it, which surfaces as a wild call out of the renderer rather than as a hole. Because
// CENTRE is left alone the grid stays centred on 0 and so has to cover +/-reach in every direction,
// and because the clamps are rescaled by stepping the float exponent the factor has to be a whole
// power of two. Vice City at its real offset reaches x 6963, so one halving (+/-6000) is not enough
// and it takes two (+/-12000, sectors of 200 units).
//
// The constants cannot simply be edited in place - the compiler pooled one 0.02 and one 60.0 for
// the whole game, and 516 of those references have nothing to do with sectors - so each conversion
// is repointed at our own copy, and the +/-3000 clamps in CEntity::Add and CEntity::Remove (the
// only two places that clamp a coordinate before converting it) are widened to match.
//
// The cost is that a sector covers 4^steps times the area, so every scan walks more entities. That
// is the trade for leaving the arrays the size they are; growing them instead means re-pointing
// every instruction that indexes them, which is a far larger job.
#include <plugin.h>
#include <cstdint>
#include <cstring>
#include <iterator>
#include "WorldSectors.h"
#include "SectorGridSites.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// Read by the game once the sites below point at them, so they have to outlive Install(). Install
// sets them from the region's extent; the values here are the vanilla ones.
float gFineScale = 0.02f;      // 1/50  - 120 sectors of 50 units
float gCoarseScale = 0.005f;   // 1/200 -  30 sectors of 200 units
float gLowBound = -3000.0f;
float gHighBound = 3000.0f;
int gShrinkSteps = 0;

// Three halvings take the fine grid to 400-unit sectors and +/-24000. Past that a sector holds
// enough entities that every scan walking it is a problem of its own, so refuse rather than
// quietly making the game crawl.
const int kMaxShrinkSteps = 3;

// The sector grids are not the only thing that stops at +/-3000. CColStore and CIplStore each build
// a quad tree over a CRect(-3000, 3000, 3000, -3000) assembled from immediates in their Initialise,
// and CColStore::AddCollisionNeededAtPosn / CIplStore::AddIplsNeededAtPosn query it every frame
// from the player's position. A query outside the root rectangle descends on a child index the tree
// has no node for, so standing past the edge of the map walks a junk pointer - which is why getting
// out there dies a frame after arriving no matter what the region does or does not load.
// CEntryExitManager's tree has the same problem and IplLoader already widens it in place; these two
// are built later, so patch the immediates instead and let Initialise build them the right size.
//
// The rectangle is (left, bottom, right, top) with bottom holding the larger y, and each of the two
// functions writes it as four `mov dword ptr [esp + N], imm32`.
enum RectField { kLeft, kBottom, kRight, kTop };
struct RectSite {
    uint32_t operand;
    uint32_t oldValue;
    uint8_t  field;
    const char* what;
};
const RectSite kQuadTreeRects[] = {
    { 0x411413, 0xC53B8000, kLeft,   "CColStore" },
    { 0x41142B, 0x453B8000, kBottom, "CColStore" },
    { 0x411423, 0x453B8000, kRight,  "CColStore" },
    { 0x41141B, 0xC53B8000, kTop,    "CColStore" },
    { 0x405EE6, 0xC53B8000, kLeft,   "CIplStore" },
    { 0x405EFE, 0x453B8000, kBottom, "CIplStore" },
    { 0x405EF6, 0x453B8000, kRight,  "CIplStore" },
    { 0x405EEE, 0xC53B8000, kTop,    "CIplStore" },
};

uint32_t AsBits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

void WidenStoreQuadTrees() {
    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);
    if (minX >= -3000.0f && maxX <= 3000.0f && minY >= -3000.0f && maxY <= 3000.0f) return;

    for (const RectSite& s : kQuadTreeRects) {
        if (patch::GetUInt(s.operand) == s.oldValue) continue;
        Log::Write("ERROR: %s's quad tree is not built over the vanilla +/-3000 (site %08X holds "
                   "%08X); left alone, and standing outside the map will still crash",
                   s.what, (unsigned)s.operand, patch::GetUInt(s.operand));
        return;
    }

    const float edge[] = { minX, maxY, maxX, minY };
    for (const RectSite& s : kQuadTreeRects) patch::SetUInt(s.operand, AsBits(edge[s.field]));
    Log::Write("col/ipl store quad trees widened to (%.0f, %.0f)-(%.0f, %.0f); the per-frame "
               "AddCollisionNeededAtPosn and AddIplsNeededAtPosn lookups can address the region now",
               minX, minY, maxX, maxY);
}

// How far from the origin the grids have to reach. The grid stays centred on 0, so what has to fit
// is the furthest corner of the region in any direction.
float RequiredReach() {
    float minX, maxX, minY, maxY;
    Settings::WorldExtent(minX, maxX, minY, maxY);
    float reach = 0.0f;
    const float corners[] = { -minX, maxX, -minY, maxY };
    for (float v : corners)
        if (v > reach) reach = v;
    return reach;
}

// Scaling a float by a power of two is one step of the exponent per doubling, sign and mantissa
// left alone: -3000 with two steps becomes -12000, and the 2999.5 the high clamp writes becomes
// 11998.
uint32_t ScaledBits(uint32_t bits, int steps) { return bits + ((uint32_t)steps << 23); }

uint32_t NewValue(const WorldSectors::Site& s) {
    switch (s.kind) {
        case WorldSectors::kFineScale:     return (uint32_t)(uintptr_t)&gFineScale;
        case WorldSectors::kCoarseScale:   return (uint32_t)(uintptr_t)&gCoarseScale;
        case WorldSectors::kLowBound:      return (uint32_t)(uintptr_t)&gLowBound;
        case WorldSectors::kHighBound:     return (uint32_t)(uintptr_t)&gHighBound;
        case WorldSectors::kLowImmediate:
        case WorldSectors::kHighImmediate: return ScaledBits(s.oldValue, gShrinkSteps);
    }
    return s.oldValue;
}

}  // namespace

namespace WorldSectors {

void Install() {
    const float reach = RequiredReach();
    int steps = 0;
    while (steps < kMaxShrinkSteps && 3000.0f * (float)(1 << steps) < reach) steps++;

    if (steps == 0) {
        Log::Write("world sectors left as they are: the region reaches %.0f, which the vanilla "
                   "+/-3000 grids already address", reach);
        WidenStoreQuadTrees();
        return;
    }
    if (3000.0f * (float)(1 << steps) < reach) {
        Log::Write("ERROR: world sectors left at +/-3000: the region reaches %.0f, and covering it "
                   "would need sectors larger than %d units, at which point every entity scan is a "
                   "problem of its own; move the region nearer the middle of the map instead",
                   reach, 50 << kMaxShrinkSteps);
        return;
    }

    // Check the whole table before touching any of it: a half-rescaled world would file entities
    // in sectors nothing looks in, which is worse than leaving the grid alone.
    int wrong = 0;
    for (const Site& s : kSites)
        if (patch::GetUInt(s.operand) != s.oldValue) wrong++;
    if (wrong) {
        Log::Write("ERROR: world sectors left at +/-3000: %d of %d site(s) do not hold the value "
                   "this build of gta_sa.exe should have; another mod may have moved them",
                   wrong, (int)std::size(kSites));
        return;
    }

    const float factor = (float)(1 << steps);
    gShrinkSteps = steps;
    gFineScale = 0.02f / factor;
    gCoarseScale = 0.005f / factor;
    gLowBound = -3000.0f * factor;
    gHighBound = 3000.0f * factor;

    int patched[kSiteKindCount] = {};
    for (const Site& s : kSites) {
        patch::SetUInt(s.operand, NewValue(s));
        patched[s.kind]++;
    }

    Log::Write("world sectors: 120x120 of 50 units -> %d, 30x30 of 200 -> %d; the static world now "
               "reaches +/-%.0f instead of +/-3000, for a region reaching %.0f (%d conversion(s) "
               "repointed, %d clamp(s) widened)",
               50 << steps, 200 << steps, 3000.0f * factor, reach,
               patched[kFineScale] + patched[kCoarseScale],
               patched[kLowBound] + patched[kHighBound] +
                   patched[kLowImmediate] + patched[kHighImmediate]);
    Log::Write("note: a sector now covers %d times the ground, so entity scans walk longer lists; "
               "savegames are unaffected, the grids are the same size as before", 1 << (2 * steps));

    WidenStoreQuadTrees();
}

}  // namespace WorldSectors
