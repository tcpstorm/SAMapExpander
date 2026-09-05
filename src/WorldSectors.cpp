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
// the set of sites is findable by pattern and, more importantly, complete. Both halves are
// repointed at our own copies, and the two constants together place the grid: SCALE sets how much
// ground a cell covers, CENTRE sets where index 0 begins.
//
// Repointing SCALE alone - which is what this did first - leaves the grid centred on 0, so to reach
// Vice City at x 7998 it also has to reach x -7998, and 120 cells stretched over 16000 units are
// 133 units each. Rounded to the power of two the exponent trick needed, that came out at 200-unit
// cells: sixteen times the ground of a vanilla sector, and sixteen times the entities in it.
//
// That is not a rendering cost, it is a collision one. CCollision::BuildCacheOfCameraCollision
// walks every entity in the sectors around the camera each frame, so sixteen times the entities is
// sixteen times the work, and over dense city geometry the frame stops finishing at all - the game
// hangs with no crash and no exception, which is exactly what it did.
//
// So fit both constants to the ground actually in use instead. San Andreas' own map plus the
// region spans -3000..7998, which 120 cells cover at 92 units each rather than 200 - a third of the
// scan cost, and the smallest cell the array size allows. Cells stay larger than vanilla's 50
// because 120 of those only ever cover 6000 units; getting back to 50 means making the arrays
// bigger, which means repointing every instruction that indexes them, and that is a far larger job
// than repointing the two constants they are indexed by.
//
// The +/-3000 clamps in CEntity::Add and CEntity::Remove - the only two places that clamp a
// coordinate before converting it - move to the new bounds to match.
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
float gFineCentre = 60.0f;     // index of coordinate 0 in a 120-entry grid
float gCoarseCentre = 15.0f;   //                        30-entry grid
float gLowBound = -3000.0f;
float gHighBound = 3000.0f;
uint32_t gLowImmediateBits = 0xC53B8000;   // -3000.0f
uint32_t gHighImmediateBits = 0x453B7000;  //  2999.5f

// A cell this size means 64 times a vanilla sector's ground in every scan. Past that the game is
// unplayable rather than merely slower, so say so and leave the grid alone: moving the region
// nearer the middle of the map is the better answer, and a hole is easier to diagnose than a hang.
const float kWorstCell = 400.0f;

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

uint32_t NewValue(const WorldSectors::Site& s) {
    switch (s.kind) {
        case WorldSectors::kFineScale:     return (uint32_t)(uintptr_t)&gFineScale;
        case WorldSectors::kCoarseScale:   return (uint32_t)(uintptr_t)&gCoarseScale;
        case WorldSectors::kFineCentre:    return (uint32_t)(uintptr_t)&gFineCentre;
        case WorldSectors::kCoarseCentre:  return (uint32_t)(uintptr_t)&gCoarseCentre;
        case WorldSectors::kLowBound:      return (uint32_t)(uintptr_t)&gLowBound;
        case WorldSectors::kHighBound:     return (uint32_t)(uintptr_t)&gHighBound;
        case WorldSectors::kLowImmediate:  return gLowImmediateBits;
        case WorldSectors::kHighImmediate: return gHighImmediateBits;
    }
    return s.oldValue;
}

}  // namespace

namespace WorldSectors {

void Install() {
    // One pair of constants serves both axes, so the grid is square and has to span whichever of
    // the four edges is furthest out in either direction.
    float minX, maxX, minY, maxY;
    Settings::RegionExtent(minX, maxX, minY, maxY);
    const float lo = minX < minY ? minX : minY;
    const float hi = maxX > maxY ? maxX : maxY;

    if (lo >= -3000.0f && hi <= 3000.0f) {
        Log::Write("world sectors left as they are: everything is inside the vanilla +/-3000, "
                   "which the grids already address");
        WidenStoreQuadTrees();
        return;
    }

    const float span = hi - lo;
    const float cell = span / 120.0f;
    if (cell > kWorstCell) {
        Log::Write("ERROR: world sectors left at +/-3000: covering %.0f..%.0f needs %.0f-unit "
                   "sectors, and a sector that size holds enough entities that the per-frame "
                   "collision and render scans stop keeping up; move the region nearer the middle "
                   "of the map instead", lo, hi, cell);
        return;
    }

    // Check the whole table before touching any of it: a half-fitted world would file entities in
    // sectors nothing looks in, which is worse than leaving the grid alone.
    int wrong = 0;
    for (const Site& s : kSites)
        if (patch::GetUInt(s.operand) != s.oldValue) wrong++;
    if (wrong) {
        Log::Write("ERROR: world sectors left at +/-3000: %d of %d site(s) do not hold the value "
                   "this build of gta_sa.exe should have; another mod may have moved them",
                   wrong, (int)std::size(kSites));
        return;
    }

    gFineScale = 120.0f / span;
    gCoarseScale = 30.0f / span;
    gFineCentre = -lo * gFineScale;
    gCoarseCentre = -lo * gCoarseScale;
    gLowBound = lo;
    gHighBound = hi;
    // The clamp stores a value one hundredth of a cell inside the top edge, so that the coordinate
    // it writes converts to the last index rather than one past it - the same margin vanilla keeps
    // by storing 2999.5 against a bound of 3000.
    gLowImmediateBits = AsBits(lo);
    gHighImmediateBits = AsBits(hi - cell * 0.01f);

    int patched[kSiteKindCount] = {};
    for (const Site& s : kSites) {
        patch::SetUInt(s.operand, NewValue(s));
        patched[s.kind]++;
    }

    const float ratio = (cell / 50.0f) * (cell / 50.0f);
    Log::Write("world sectors fitted to %.0f..%.0f: 120x120 sectors of %.0f units and 30x30 of "
               "%.0f, index 0 at %.0f (%d conversion(s) repointed, %d clamp(s) moved)",
               lo, hi, cell, span / 30.0f, lo,
               patched[kFineScale] + patched[kCoarseScale] +
                   patched[kFineCentre] + patched[kCoarseCentre],
               patched[kLowBound] + patched[kHighBound] +
                   patched[kLowImmediate] + patched[kHighImmediate]);
    Log::Write("note: a sector covers %.1f times a vanilla one's ground, so every per-frame scan "
               "over it - the renderer's, the streamer's, and CCollision's camera cache - walks "
               "that much more of the world; the grids are the same size as before and savegames "
               "are unaffected", ratio);

    WidenStoreQuadTrees();
}

}  // namespace WorldSectors
