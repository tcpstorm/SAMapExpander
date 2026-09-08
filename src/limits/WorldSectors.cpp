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
// because 120 of those only ever cover 6000 units.
//
// Getting back to 50 would mean more cells, and that cannot be done. It is not the size of the job
// - the arrays are easy to find and the addressing is uniform - it is the instruction encoding.
// tools/scan_sector_array_sites.py enumerates everything a bigger grid would have to change and
// tools/sector_array_sites.txt is its report; the short version:
//
//     CWorld::ms_aSectors      0xB7D0B8   120 x 120 of 8 bytes   74 indexed accesses
//     CWorld::ms_aLodPtrLists  0xB99EB8    30 x 30  of 4 bytes    6 indexed accesses
//
// Every access has one shape - `imul reg, reg, 120`, add the column, `[reg*8 + 0xB7D0B8]` - and all
// 80 of them have their row-stride multiply in front of them, so nothing walks a sector pointer
// along a row instead and nothing forms the address another way. The base displacement would move
// the way ModelLimits moves the streaming arrays, and the row stride is not a problem either:
// `imul reg, reg, imm8` is three bytes and so is `shl reg, imm8`, so a power-of-two stride can be
// written straight over it.
//
// The clamp is the wall. Every index reaches the array through clamp(v, 0, N-1); the lower half is
// the sign-mask idiom and carries no constant, and the upper half is `cmp reg, imm8` at all 136
// fine and 13 coarse sites - three bytes, immediate sign-extended, largest value 127. Widening it
// to `cmp reg, imm32` takes six bytes and the three it would eat are the branch that reads its
// result. The reachable index range is therefore [0, 127] and the grid can be at most 128 a side:
// a 6.7% smaller cell, against the 307 a side that San Andreas plus two cities at 50 units needs.
//
// Which leaves two ways to a vanilla-sized cell, neither of them in this file yet. One is to stop
// covering the whole world at once and fit the 120 cells to the ground the player is standing on,
// re-filing every static entity when they cross into another map - no new sites, but a Remove-all
// and Add-all pass that has to be exactly symmetric or it leaves dangling pointers in sector lists.
// The other is to not spread the maps so far apart in the first place, which costs nothing at all
// and is what Settings' PackRegions does: the cell is the world span over 120, so a world packed
// to 10500 units gets 88-unit sectors where one spread over 15348 gets 128, and that is less than
// half the per-frame scan for the same two cities the same distance from Los Santos.
//
// The +/-3000 clamps in CEntity::Add and CEntity::Remove - the only two places that clamp a
// coordinate before converting it - move to the new bounds to match.
#include <plugin.h>
#include <windows.h>
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

// ------------------------------------------------------------- somebody else owning the grid
//
// This plugin cannot change how many sectors there are, only how much ground they cover, because
// every index reaches the array through `cmp reg, imm8` and no number above 127 fits in a byte.
// That is not the same as saying nobody can. fastman92's limit adjuster does it: it allocates a
// bigger CWorld::ms_aSectors, rewrites the base displacement of all 74 indexed accesses and the
// row-stride multiply of all 71, patches the clamps that can be patched, and for the ones that
// cannot - the imm8 sites - replaces the whole routine with hand-written assembly that reads the
// dimension from a variable (CWorld::GetIsLineOfSightClear and CRenderer's sector scan among
// them). Its [MAP LIMITS] section is three keys:
//
//     World map size      = 24000     ; +/-12000 instead of +/-3000
//     World sector size   = 50        ; leave this alone - it is the cell, and 50 is vanilla's
//     World LOD sector size = 200
//
// and the count falls out as map size / sector size, so 24000 / 50 is 480 x 480 sectors of exactly
// the ground a vanilla one covers. That is how GTA Underground carries Vice City, Liberty City,
// Carcer City, Bullworth and Cottonmouth over a world of +/-24000 without the cell growing, and it
// is the only shipped answer to this problem that anyone has.
//
// The trap in it is that fastman92's own example ini raises the sector size along with the map
// size, which is exactly what this file does and exactly what costs the frame rate. Anyone reading
// this log should be told the sector size is the one to leave alone.
//
// So: if the conversions no longer read San Andreas' own pooled constants, something else has
// already fitted the grid, and writing ours over the top would set 120 cells of our choosing while
// its array, its clamps and its row strides still say something else - a hole in the world at
// best. Stand down instead, say what cell size it chose, and leave it to do its job.
bool Readable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((LPCVOID)address, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(info.Protect & readable) || (info.Protect & PAGE_GUARD)) return false;
    return address + bytes <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

// The cell another mod has set, or 0 if the grid is still San Andreas' own. Established from the
// fine-scale conversions alone: they all read one pooled 0.02 in a stock executable, so if they
// have been repointed they will agree on wherever they were repointed to, and the float there is
// one over the cell size.
float ForeignCellSize() {
    uint32_t candidate = 0;
    int agree = 0, total = 0;
    for (const WorldSectors::Site& s : WorldSectors::kSites) {
        if (s.kind != WorldSectors::kFineScale) continue;
        total++;
        const uint32_t now = patch::GetUInt(s.operand);
        if (now == s.oldValue) continue;
        if (!candidate) candidate = now;
        if (now == candidate) agree++;
    }
    // A handful of odd sites is a build mismatch or a stray patch, not another world grid; the
    // whole set moving together is. Nine in ten is the line, so that a mod that leaves one or two
    // conversions alone is still recognised.
    if (!candidate || total == 0 || agree * 10 < total * 9) return 0.0f;
    if (!Readable(candidate, sizeof(float))) return -1.0f;   // owned, but the value is unreadable
    float scale = 0.0f;
    memcpy(&scale, (const void*)(uintptr_t)candidate, sizeof(scale));
    if (!(scale > 0.0f) || scale > 1.0f) return -1.0f;
    return 1.0f / scale;
}

// What to put in fastman92limitAdjuster_GTASA.ini to get vanilla-sized cells over this world.
// Its map size is centred on zero, so it has to reach whichever edge is furthest out either way.
void SuggestLimitAdjuster(float lo, float hi) {
    const float reach = (lo < 0.0f ? -lo : lo) > hi ? (lo < 0.0f ? -lo : lo) : hi;
    int mapSize = (int)(2.0f * reach + 49.0f);
    mapSize -= mapSize % 50;
    Log::Write("note: a sector of vanilla's 50 units over this world needs %d x %d of them, and "
               "the grid cannot be grown here - every index is clamped with a three-byte "
               "`cmp reg, imm8` and 127 is the largest number that encodes. fastman92's limit "
               "adjuster does grow it, by relocating the array and rewriting the routines the "
               "clamps are in; with [MAP LIMITS] \"World map size = %d\" and \"World sector size\" "
               "left at its vanilla 50 it gives %d x %d cells of 50 units. Raising its sector size "
               "along with its map size - which its own example ini does - costs exactly what this "
               "note is about, so leave that one alone",
               (int)((hi - lo) / 50.0f + 1.0f), (int)((hi - lo) / 50.0f + 1.0f),
               mapSize, mapSize / 50, mapSize / 50);
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

// Set by Install once the whole table has been written; nothing may move the grid unless it is.
bool gOwns = false;
float gFitLo = -3000.0f, gFitHi = 3000.0f;

// The six numbers that place both grids. SCALE says how much ground a cell covers, CENTRE says
// where index 0 begins, and the two bounds are what CEntity::Add and CEntity::Remove clamp a
// coordinate to before converting it. The conversion sites read the first four through a pointer,
// so those are stores; the bounds appear twice each - once as a pointer to a float and once as an
// immediate inside the two functions - so the immediates have to be written into the code.
void SetFit(float lo, float hi) {
    const float span = hi - lo;
    const float cell = span / 120.0f;
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
    gFitLo = lo;
    gFitHi = hi;
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

    // Before anything else, in case another mod got here first: it will have repointed the same
    // conversions, and our values written over its would leave 120 cells of our choosing addressing
    // an array that is no longer that shape.
    const float foreign = ForeignCellSize();
    if (foreign != 0.0f) {
        if (foreign < 0.0f)
            Log::Write("world sectors left alone: the coordinate-to-sector conversions have been "
                       "repointed by something other than this plugin - a limit adjuster, most "
                       "likely - and its cell size could not be read. It owns the grid now");
        else
            Log::Write("world sectors left alone: another mod has already fitted them, to cells of "
                       "%.1f units (vanilla is 50). It owns the grid, including how many cells "
                       "there are, so nothing here is written", foreign);
        if (foreign > 60.0f)
            Log::Write("note: that is a larger cell than San Andreas' own 50, and the per-frame "
                       "collision and render scans cost the cell's ground. If that is fastman92's "
                       "limit adjuster, [MAP LIMITS] \"World sector size\" is the key to put back "
                       "to 50 - raise \"World map size\" alone and the count grows instead of the "
                       "cell");
        WidenStoreQuadTrees();
        return;
    }

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

    SetFit(lo, hi);

    int patched[kSiteKindCount] = {};
    for (const Site& s : kSites) {
        patch::SetUInt(s.operand, NewValue(s));
        patched[s.kind]++;
    }
    gOwns = true;

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
    // The cell is the world span over 120 and 120 is not negotiable - the index that reaches the
    // array is clamped with a three-byte `cmp reg, imm8` at every one of the 136 sites that do it,
    // so no index above 127 can be expressed. Say so once, here, rather than leaving the next
    // person to wonder why the arrays were not simply made bigger.
    if (ratio > 1.5f) {
        Log::Write("note: the only way to a smaller sector from here is a smaller world, because "
                   "this plugin cannot grow the grid past 128 a side (see "
                   "tools/sector_array_sites.txt for why). Placing the regions in each other's "
                   "corners instead of where their ini puts them is what shrinks the span - "
                   "[World] PackRegions - and it costs the same whether there are two maps or "
                   "five");
        SuggestLimitAdjuster(lo, hi);
    }

    WidenStoreQuadTrees();
}

bool Owns() { return gOwns; }

void CurrentFit(float& lo, float& hi) { lo = gFitLo; hi = gFitHi; }

void Refit(float lo, float hi) {
    if (!gOwns || !(hi > lo)) return;
    SetFit(lo, hi);
    // The four conversion constants are read through a pointer and so are already live; only the
    // two clamp bounds are baked into CEntity::Add and CEntity::Remove as immediates and have to
    // be written again. Seven sites, and they were verified against their vanilla values before
    // Install wrote them the first time.
    for (const Site& s : kSites)
        if (s.kind == kLowImmediate || s.kind == kHighImmediate)
            patch::SetUInt(s.operand, NewValue(s));
}

}  // namespace WorldSectors
