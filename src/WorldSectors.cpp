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
// The constants cannot simply be edited in place - the compiler pooled one 0.02 and one 60.0 for
// the whole game, and 516 of those references have nothing to do with sectors - so each conversion
// is repointed at our own copy, and the +/-3000 clamps in CEntity::Add and CEntity::Remove (the
// only two places that clamp a coordinate before converting it) are widened to match.
//
// The cost is that a sector is four times the area, so every scan walks more entities. That is the
// trade for leaving the arrays the size they are; growing them instead means re-pointing every
// instruction that indexes them, which is a far larger job.
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

// Read by the game once the sites below point at them, so they have to outlive Install().
const float kOurFineScale = 0.01f;      // 1/100 - 120 sectors of 100 units
const float kOurCoarseScale = 0.0025f;  // 1/400 -  30 sectors of 400 units
const float kOurLowBound = -6000.0f;
const float kOurHighBound = 6000.0f;

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

// Doubling a float is one step of the exponent, and the sign and mantissa are left alone: -3000
// becomes -6000, and the 2999.5 the high clamp writes becomes 5999.
uint32_t Doubled(uint32_t bits) { return bits + (1u << 23); }

uint32_t NewValue(const WorldSectors::Site& s) {
    switch (s.kind) {
        case WorldSectors::kFineScale:     return (uint32_t)(uintptr_t)&kOurFineScale;
        case WorldSectors::kCoarseScale:   return (uint32_t)(uintptr_t)&kOurCoarseScale;
        case WorldSectors::kLowBound:      return (uint32_t)(uintptr_t)&kOurLowBound;
        case WorldSectors::kHighBound:     return (uint32_t)(uintptr_t)&kOurHighBound;
        case WorldSectors::kLowImmediate:
        case WorldSectors::kHighImmediate: return Doubled(s.oldValue);
    }
    return s.oldValue;
}

}  // namespace

namespace WorldSectors {

void Install() {
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

    int patched[kSiteKindCount] = {};
    for (const Site& s : kSites) {
        patch::SetUInt(s.operand, NewValue(s));
        patched[s.kind]++;
    }

    Log::Write("world sectors: 120x120 of 50 units -> 100, 30x30 of 200 -> 400; the static world "
               "now reaches +/-6000 instead of +/-3000 (%d conversion(s) repointed, %d clamp(s) "
               "widened)",
               patched[kFineScale] + patched[kCoarseScale],
               patched[kLowBound] + patched[kHighBound] +
                   patched[kLowImmediate] + patched[kHighImmediate]);
    Log::Write("note: a sector now covers four times the ground, so entity scans walk longer "
               "lists; savegames are unaffected, the grids are the same size as before");

    WidenStoreQuadTrees();
}

}  // namespace WorldSectors
