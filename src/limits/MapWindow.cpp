// One grid at a time: fit San Andreas' 120 x 120 sectors to the map the player is standing on,
// and re-file the world when they cross to another one.
//
// The sector grid is 120 cells whatever ground it covers, and this plugin cannot change that - the
// index that reaches CWorld::ms_aSectors is clamped with `cmp reg, imm8` at all 136 sites that do
// it, so no index above 127 can be expressed (tools/sector_array_sites.txt is the enumeration).
// The cell is therefore the world span over 120, and every per-frame collision and render scan
// walks a cell's worth of entities. Measured over San Andreas' own 45884 placements, the cell a
// player stands in holds 12.2 entities at vanilla's 50 units and 27.4 at the 83 that San Andreas
// plus a packed Vice City needs.
//
// Packing the maps into each other's corners (Settings' PackRegions) stops that number growing as
// maps are added - five maps cost what one does - but it cannot get back to 50, because 120 cells
// of 50 units only ever cover 6000 and San Andreas is already 6000 across. The only way to a
// vanilla cell without a bigger array is to stop covering everything at once.
//
// So: 120 cells of about 50 units over one map, and when the player crosses into another map, the
// grid moves. That needs no new sites - the conversions already read this plugin's four constants,
// and WorldSectors::Refit is six stores and seven immediates - but the constants are only half of
// it. An entity is filed into cells worked out from its position and those constants, and the code
// that later unlinks it works the cells out again the same way. Change the constants underneath a
// filed entity and its list entries are in cells nothing will ever look in again, and the pointers
// in them outlive the entity: a dangling pointer in a sector list, which is a crash with no
// obvious cause some minutes later.
//
// The pass therefore has to be exactly symmetric, and the way it is made symmetric here is by not
// guessing. The set of entities that are filed is read out of the sector lists themselves - it is,
// by definition, exactly what is in them - and each one is removed with the game's own
// CEntity::Remove while the old constants are still in place, which is the precise inverse of the
// CEntity::Add that put it there. Then the lists are checked: every one of the 14400 fine sectors,
// 900 LOD lists and 256 repeat sectors must be empty, and if any is not, the pass says so and
// unlinks what is left rather than leaving it to rot. Only then do the constants move, and only
// then is everything added back.
//
// Three things are worth knowing before turning it on.
//
//   - It is not free at the moment it happens. Around forty thousand entities are removed and
//     added again, each touching a handful of lists; expect a dropped frame. It happens where two
//     maps meet, which is open water or empty ground, and nowhere else.
//
//   - Peds, vehicles and objects have to be re-filed too, not just buildings. They live in
//     CWorld::ms_aRepeatSectors, a 16 x 16 grid that wraps rather than clamps, but the index it
//     wraps is computed from the same fine-grid constants (CPhysical::Add reads them at 0x544A7C
//     and three sites after it), so moving the grid moves them as well.
//
//   - An entity outside the new window is not filed at all. That is the point - a map five
//     thousand units away should cost nothing - but it also means the sector walks that delete
//     RwObjects will never reach it, so its model would stay referenced for ever. It is asked to
//     drop its RwObject on the way out, which is what those walks would have done.
//
// What cannot be checked without running the game is whether the removal really is symmetric in
// every case, which is why this is off by default and why the pass counts what it finds and says
// so in the log. If the "sector lists not empty after removal" line ever appears, something files
// an entity by a route this does not know about and the feature should go back off.
#include <plugin.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>

#include <CWorld.h>
#include <CEntity.h>
#include <CSector.h>
#include <CRepeatSector.h>
#include <CPtrNodeSingleLink.h>
#include <CPtrNodeDoubleLink.h>
#include <CRect.h>

#include <CPlayerPed.h>
#include "MapWindow.h"
#include "WorldSectors.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

const int kFineSide = 120;
const int kLodSide = 30;
const int kRepeatSide = 16;

// 120 cells of San Andreas' own 50 units. A window smaller than this would give cells finer than
// vanilla, which is not a saving - an entity would then be filed into more cells than it used to
// be, costing pointer nodes for nothing - so a small map gets a 6000-unit window with room to
// spare rather than a tight one.
const float kSmallestWindow = 6000.0f;

int gActiveMap = -2;         // -2 nothing chosen yet, -1 San Andreas, 0.. a region
bool gInstalled = false;
int gSwitches = 0;

struct Window { float lo, hi; };

// The range one map needs.
//
// There is one SCALE and one CENTRE for the whole grid, not one per axis, so the ground it covers
// is the square [lo, hi] x [lo, hi] and both of the map's projections have to fit inside the same
// [lo, hi]. That is not the map's width: a map at x 3846..7282 and y -914..2767 is 3436 by 3681,
// but the range that holds both is -914..7282, which is 8196. A map whose x and y ranges sit on
// top of each other - San Andreas' own -3000..3000, or anything packed near the diagonal - gets a
// window as tight as its own size, and one out on a limb does not.
//
// Splitting the 268 fine conversions into the ones that convert an x and the ones that convert a y
// would end that, and give every map a window exactly its own size on each axis - but telling them
// apart needs dataflow analysis of each site, and a site classified the wrong way round files its
// entities in transposed cells. Not worth guessing at. See docs/known-limitations.md.
Window WindowFor(int map) {
    float minX, maxX, minY, maxY;
    Settings::MapExtent(map, minX, maxX, minY, maxY);
    float lo = minX < minY ? minX : minY;
    float hi = maxX > maxY ? maxX : maxY;
    // Never finer than vanilla: below 50 units a cell buys nothing and costs pointer nodes, since
    // every entity is then filed into more cells than San Andreas ever files one into.
    if (hi - lo < kSmallestWindow) {
        const float middle = (lo + hi) * 0.5f;
        lo = middle - kSmallestWindow * 0.5f;
        hi = middle + kSmallestWindow * 0.5f;
    }
    return Window{ lo, hi };
}

bool Inside(float x, float y, int map, float margin) {
    float minX, maxX, minY, maxY;
    Settings::MapExtent(map, minX, maxX, minY, maxY);
    return x >= minX - margin && x <= maxX + margin &&
           y >= minY - margin && y <= maxY + margin;
}

// ---------------------------------------------------------------- reading the lists back out

void CollectSingle(CPtrListSingleLink& list, std::vector<CEntity*>& out) {
    for (CPtrNodeSingleLink* n = list.GetNode(); n; n = n->pNext)
        if (n->pItem) out.push_back((CEntity*)n->pItem);
}

void CollectDouble(CPtrListDoubleLink& list, std::vector<CEntity*>& out) {
    for (CPtrNodeDoubleLink* n = list.GetNode(); n; n = n->pNext)
        if (n->pItem) out.push_back((CEntity*)n->pItem);
}

// Everything currently filed anywhere, once each. Taken from the lists rather than from the pools,
// because the lists are the thing that has to be emptied and a pool holds entities that were never
// added - a ped riding in a car is removed from the world and is still in the ped pool.
void CollectFiled(std::vector<CEntity*>& out) {
    out.clear();
    out.reserve(48000);
    for (int i = 0; i < kFineSide * kFineSide; i++) {
        CollectSingle(CWorld::ms_aSectors[i].m_buildingList, out);
        CollectDouble(CWorld::ms_aSectors[i].m_dummyList, out);
    }
    for (int i = 0; i < kLodSide * kLodSide; i++)
        CollectSingle(CWorld::ms_aLodPtrLists[i], out);
    for (int i = 0; i < kRepeatSide * kRepeatSide; i++)
        for (int l = 0; l < 3; l++)
            CollectDouble(CWorld::ms_aRepeatSectors[i].m_lists[l], out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// After the removal pass every list must be empty. Anything left is an entity filed by a route
// CEntity::Remove did not undo, and leaving it there would be a pointer into a cell that is about
// to mean somewhere else, so unlink it here and count it. Returns how many were left.
int DrainLeftovers() {
    int left = 0;
    auto drainSingle = [&](CPtrListSingleLink& list) {
        while (CPtrNodeSingleLink* n = list.GetNode()) {
            list.DeleteItem(n->pItem);
            left++;
            if (list.GetNode() == n) { list.Flush(); break; }   // refused to go: drop the lot
        }
    };
    auto drainDouble = [&](CPtrListDoubleLink& list) {
        while (CPtrNodeDoubleLink* n = list.GetNode()) {
            list.DeleteItem(n->pItem);
            left++;
            if (list.GetNode() == n) { list.Flush(); break; }
        }
    };
    for (int i = 0; i < kFineSide * kFineSide; i++) {
        drainSingle(CWorld::ms_aSectors[i].m_buildingList);
        drainDouble(CWorld::ms_aSectors[i].m_dummyList);
    }
    for (int i = 0; i < kLodSide * kLodSide; i++) drainSingle(CWorld::ms_aLodPtrLists[i]);
    for (int i = 0; i < kRepeatSide * kRepeatSide; i++)
        for (int l = 0; l < 3; l++) drainDouble(CWorld::ms_aRepeatSectors[i].m_lists[l]);
    return left;
}

// ---------------------------------------------------------------------------- the switch itself

double MilliesSince(LARGE_INTEGER start) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return freq.QuadPart ? 1000.0 * (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart
                         : 0.0;
}

void SwitchTo(int map) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);

    const Window w = WindowFor(map);

    std::vector<CEntity*> filed;
    CollectFiled(filed);

    // Removal, with the old constants still in place: this is the exact inverse of the Add that
    // filed each of them, because it works the same cells out from the same position and the same
    // four numbers.
    for (CEntity* e : filed) e->Remove();
    const int leftover = DrainLeftovers();

    WorldSectors::Refit(w.lo, w.hi);

    int refiled = 0, dropped = 0, unloaded = 0;
    for (CEntity* e : filed) {
        // Peds, vehicles and objects go back unconditionally: their grid wraps rather than clamps,
        // so there is no outside to be on the wrong side of, and dropping one would take the
        // player's own car out of the world.
        const bool physical = e->m_nType == ENTITY_TYPE_VEHICLE ||
                              e->m_nType == ENTITY_TYPE_PED ||
                              e->m_nType == ENTITY_TYPE_OBJECT;
        if (!physical) {
            // Which of top and bottom holds the larger y is not worth remembering - take both
            // either way round, so an entity straddling the edge is kept rather than dropped.
            const CRect r = e->GetBoundRect();
            const float x0 = r.left < r.right ? r.left : r.right;
            const float x1 = r.left > r.right ? r.left : r.right;
            const float y0 = r.top < r.bottom ? r.top : r.bottom;
            const float y1 = r.top > r.bottom ? r.top : r.bottom;
            if (x1 < w.lo || x0 > w.hi || y1 < w.lo || y0 > w.hi) {
                // Outside the window, so not filed - which is the whole point, a map on the other
                // side of the world costing nothing. But the sector walks that delete RwObjects
                // will not reach it either, so ask it to drop its model here instead; that is what
                // CStreaming::DeleteRwObjectsBehindCamera would have done on its way past.
                if (e->m_pRwObject) { e->DeleteRwObject(); unloaded++; }
                dropped++;
                continue;
            }
        }
        e->Add();
        refiled++;
    }

    gActiveMap = map;
    gSwitches++;

    float minX, maxX, minY, maxY;
    Settings::MapExtent(map, minX, maxX, minY, maxY);
    Log::Write("map window: fitted to %s (%.0f..%.0f, %.0f..%.0f) - grid now covers %.0f..%.0f at "
               "%.1f-unit sectors; %d entity(s) re-filed, %d left out of the window (%d asked to "
               "drop their model), %.1f ms",
               map < 0 ? "San Andreas" : Settings::regions[map].name.c_str(),
               minX, maxX, minY, maxY, w.lo, w.hi, (w.hi - w.lo) / 120.0f,
               refiled, dropped, unloaded, MilliesSince(start));
    if (Settings::world.debug)
        Log::Debug("map window: crossing %d, %d entity(s) were filed before the move",
                   gSwitches, (int)filed.size());
    // The symmetry check. CEntity::Remove is the exact inverse of the CEntity::Add that filed an
    // entity, so after removing everything the lists were holding, the lists must be empty. If they
    // are not, something files an entity by a route this does not know about, and every further
    // crossing would be another chance to leave a pointer behind - so stop here, with the world
    // consistent, rather than carrying on and dangling one.
    if (leftover) {
        Log::Write("ERROR: map window: %d sector list entry(s) survived CEntity::Remove and had to "
                   "be unlinked by hand. Something files entities by a route this does not know "
                   "about, so the window is now frozen where it is - no further crossing will move "
                   "it. Please report this, and turn [World] MapWindow back off meanwhile",
                   leftover);
        gInstalled = false;
    }
}

// --------------------------------------------------------------------------------- the trigger

void OnFrame() {
    if (!gInstalled) return;
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    const CVector p = player->GetPosition();
    // A dead reading - the player not placed yet - is zero, which is inside San Andreas anyway, so
    // there is nothing to guard against beyond the null above.

    if (gActiveMap != -2 && Inside(p.x, p.y, gActiveMap, Settings::world.mapWindowMargin))
        return;   // still on the map the grid is fitted to, margin and all

    // Which map they are actually on. The margin is deliberately not applied here: leaving a map
    // takes the margin, arriving at one does not, and the gap between the two is the hysteresis
    // that stops a player walking a boundary re-filing the world every frame.
    for (int m = -1; m < (int)Settings::regions.size(); m++) {
        if (m == gActiveMap) continue;
        if (!Inside(p.x, p.y, m, 0.0f)) continue;
        SwitchTo(m);
        return;
    }
    // On no map at all - out at sea between two of them, or falling. Keep the grid where it is;
    // the next map they touch will move it.
    if (gActiveMap == -2) SwitchTo(-1);
}

}  // namespace

namespace MapWindow {

void Install() {
    if (!Settings::world.mapWindow) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("map window: not installed - under SA-MP the server owns where every object is "
                   "and a client re-filing its own world would disagree with it");
        return;
    }
    if (!WorldSectors::Owns()) {
        Log::Write("map window: not installed - this plugin does not own the world grid (either "
                   "nothing needed moving, a site did not verify, or another mod got there first), "
                   "and moving a grid somebody else fitted would file entities in cells nothing "
                   "looks in");
        return;
    }
    if (Settings::regions.empty()) {
        Log::Write("map window: not installed - there is only San Andreas, and the grid already "
                   "fits it exactly");
        return;
    }

    // Say what it is worth before it does anything, so the log is useful even when it is off.
    float lo, hi;
    WorldSectors::CurrentFit(lo, hi);
    const float whole = (hi - lo) / 120.0f;
    float worst = 0.0f;
    for (int m = -1; m < (int)Settings::regions.size(); m++) {
        const Window w = WindowFor(m);
        const float cell = (w.hi - w.lo) / 120.0f;
        if (cell > worst) worst = cell;
    }
    Log::Write("map window: on. One grid at a time - the largest map needs %.1f-unit sectors "
               "against the %.1f a single grid over everything needs, so every per-frame sector "
               "scan walks %.0f%% of what it would. Crossing between maps re-files the world, "
               "which costs a frame",
               worst, whole, 100.0f * (worst * worst) / (whole * whole));
    Log::Write("map window: this is the one part of the world-grid work that cannot be checked "
               "without running the game. If the log ever says a sector list survived the removal "
               "pass, turn it off");

    Events::gameProcessEvent += OnFrame;
    gInstalled = true;
}

}  // namespace MapWindow
