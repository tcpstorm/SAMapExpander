// Makes a region's interiors enterable, using the mechanism San Andreas already has rather than
// anything ported from the source game.
//
// Vice City and San Andreas hide interiors identically, because San Andreas inherited the code.
// Every entity carries a one-byte area code - the third column of an IPL `inst` line, which
// CFileLoader::LoadObjectInstance stores at CEntity + 0x2F (0x538353: `mov dl, [edi+0x20]` /
// `mov [esi+0x2f], dl`, so only the low byte; the higher bits of that column are unrelated flags,
// which is why CityLoader can keep setting bit 512 there without disturbing the area). The renderer
// then draws an entity only when its code matches, in CRenderer::SetupEntityVisibility at 0x554476:
//
//     mov al, [esi+0x2F]        ; entity->m_nAreaCode
//     mov ecx, [0xB72914]       ; CGame::currArea
//     cmp edx, ecx / je visible
//     cmp al, 0xD  / jne hidden ; 13 means "everywhere", in both games
//
// CRenderer::SetupBigBuildingVisibility (0x55466A) and CStreaming::RemoveBuildingsNotInArea
// (0x4094EC) test the same two values, including the same 13.
//
// Vice City writes real area codes into its IPLs - 1 is the Ocean View hotel, 2 the Vercetti
// estate, 4 the North Point Mall, 17 the Malibu Club, and so on up to 18, with 13 reserved for
// "everywhere" exactly as San Andreas reserves it. CityLoader already copies that column through
// untouched, so the interiors are already loaded, already placed and already correctly hidden.
// That is what the black rectangles in the doorways are: the interiors are there, the shells around
// them are there, and the rooms behind the openings are switched off.
//
// So there is nothing to port and nothing to move. Vice City builds its interiors inside the real
// buildings at real world coordinates - the mall interior sits inside the mall, Ocean View's lobby
// inside Ocean View - which is precisely why they cannot be seen from outside: the shell is in the
// way, and the area code takes care of the doorway. The one piece San Andreas cannot supply is the
// switch. Vice City changed the area from its mission script when the player crossed a trigger at a
// door, and that script is not here; VC's data files record nothing about where its doorways are.
//
// This module is that switch, and nothing else. Each region's ini lists boxes in the region's own
// coordinates (Settings' InteriorVolume); when the player is inside one, currArea becomes that
// volume's area and the interior appears around them. It deliberately does not use entry/exit
// markers: San Andreas' `enex` exists to teleport the player to an interior that lives somewhere
// else in the world, and these interiors are already where the player is standing, so the teleport
// would be a fade to black that puts them back where they were - and it would need a hand-written
// position and heading for every door, which is the one way to get a player stuck inside geometry.
// A box that is a metre out shows the room a step early or late; the IPL `enex` section is still
// there in IplLoader for anyone who wants the marker-and-fade behaviour instead.
//
// Collision is not area-gated anywhere in San Andreas - only rendering and streaming read currArea
// - so an interior's collision is solid whether or not it is visible. That is also true in Vice
// City, and harmless for the same reason: the rooms are inside solid buildings, so the only way to
// reach their collision is to be inside, which is when they are visible.
#include <plugin.h>
#include <CPlayerPed.h>
#include <CVehicle.h>
#include <CWorld.h>
#include <CStreaming.h>
#include <CTimer.h>
#include <safetyhook.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include "Interiors.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// CGame::currArea, a dword. CGame::CanSeeOutSideFromCurrArea (0x53C4A0) is "currArea == 0" and
// CanSeeWaterFromCurrArea (0x53C4B0) is "currArea == 0 || currArea == 5", so setting it also stops
// the game drawing sky and sea around the player - which is what an interior wants.
int& CurrArea = *reinterpret_cast<int*>(0xB72914);

// Script command 04BB (SET_INTERIOR_VISIBLE) is two instructions at 0x48B996: store the new area,
// then hand it to this. It drops the RW object of every building whose area code is neither the new
// one nor 13, so the streamer can reuse the memory; it is what makes a whole city's worth of
// buildings actually leave when the player steps into a room.
typedef void(__cdecl* RemoveBuildingsNotInAreaFn)(int);
const RemoveBuildingsNotInAreaFn CallRemoveBuildingsNotInArea =
    reinterpret_cast<RemoveBuildingsNotInAreaFn>(0x4094B0);

// CEntryExitManager::Update, called once a frame from CGame::Process. Hooking it rather than a
// timer puts the volume test in the same place, and the same frame order, as San Andreas' own
// "has the player entered an interior" check - after it, so if one of SA's markers claimed the area
// this frame we can see that and stay out of the way. It also stops running in menus and loading
// screens, where there is no player to test.
constexpr uintptr_t kEntryExitManagerUpdate = 0x440D10;

// How far outside a volume the player has to get before the interior is switched off again.
// Entering uses the box as written and leaving uses the box plus this, so standing in a doorway
// cannot flicker the whole world in and out.
constexpr float kLeaveMargin = 2.0f;
// How far inside a box the player has to be before the interior switches on. The boxes are the
// extent of a room's placements plus slack, and a placement's coordinates are its model's origin,
// so a box reaches a little past the wall it was measured from - far enough, in testing, to switch
// the world off while the player was still outside the door. Requiring them to be properly inside
// costs nothing at a doorway and is what keeps the street from going black. Together with the
// margin above it also gives the transition hysteresis, so standing in a doorway cannot flicker.
constexpr float kEnterShrink = 3.0f;

SafetyHookInline& UpdateHook() { static SafetyHookInline h; return h; }

const MapRegion* gRegion = nullptr;       // region owning gVolume
const InteriorVolume* gVolume = nullptr;  // the volume the player was last found inside
int gOurArea = 0;                         // the area we set, 0 when we are not driving currArea
bool gInstalled = false;

// The horizontal and vertical margins are separate on purpose. Sideways, a box stops short of the
// walls it was measured from, so entering demands a few units inside and leaving allows a few
// outside - that is what stops a doorway flickering and what keeps the pavement lit. Up and down
// there is nothing to be short of: the floor and the ceiling already are the bounds, and a room is
// only as tall as it is. Shrinking those too asked the player to be three units above the floor of
// the mall's car park before it would let them in.
bool Inside(const MapRegion& r, const InteriorVolume& v, const CVector& p, float grow, float zGrow) {
    // The volume is written in the region's own coordinates, like its zones and its water map, so
    // the same origin that moved the buildings moves the box.
    float x = p.x - r.originX, y = p.y - r.originY, z = p.z - r.originZ;
    return x >= v.minX - grow && x <= v.maxX + grow &&
           y >= v.minY - grow && y <= v.maxY + grow &&
           z >= v.minZ - zGrow && z <= v.maxZ + zGrow;
}

void SetArea(int area, CPlayerPed* player, const CVector& pos) {
    CurrArea = area;
    CallRemoveBuildingsNotInArea(area);

    // Coming back out, put the world back before the frame is drawn.
    //
    // RemoveBuildingsNotInArea throws away everything that does not belong to the area being
    // entered, which on the way out means the room goes and the street has to be fetched again. The
    // streamer would get there on its own, but it fetches over several seconds and in the meantime
    // the player is standing in an empty world watching their city arrive - which is what leaving
    // an interior looked like. San Andreas' own transitions do not have this problem because
    // CEntryExit::TransitionFinished ends in a blocking scene load, so do the same: ask for the
    // collision first, then the scene, with the clock stopped so the pause does not land on the
    // physics as one enormous time step. Only on the way out, and only on the frame of the
    // transition, so the cost is one hitch where there was several seconds of empty ground.
    if (!area) {
        CTimer::Suspend();
        CVector at = pos;
        CStreaming::LoadSceneCollision(&at);
        CStreaming::LoadScene(&at);
        CTimer::Resume();
    }

    // The player carries an area code of their own, and the game reads it as much as it reads
    // currArea. CEntryExit::TransitionFinished stamps it on the player and on whatever they are
    // driving at 0x4406BB and 0x4406CE, and it has to be done: CRenderer::SetupEntityVisibility
    // gates the player's own model on it, so leaving it at 0 would make them invisible in the room,
    // and worse, CColStore::EnsureCollisionIsInMemory (0x410AF5) and CIplStore::SetIplsRequired
    // read the PLAYER's code and give up when it does not match currArea - collision would stop
    // streaming and the player would drop through the world.
    player->m_nAreaCode = (unsigned char)area;
    if (CVehicle* vehicle = FindPlayerVehicle(-1, false))
        vehicle->m_nAreaCode = (unsigned char)area;
}

void Forget() {
    gRegion = nullptr;
    gVolume = nullptr;
    gOurArea = 0;
}

void Update() {
    // Somebody else now owns the area: one of San Andreas' own entry/exit markers, a script, a
    // loaded save, or a new game resetting it. Drop our claim. If they have put the player in an
    // interior of their own we do nothing at all until they let go of it again - two things fighting
    // over currArea would strand the player in an empty world.
    if (CurrArea != gOurArea) {
        Forget();
        if (CurrArea != 0) return;
    }

    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    CVector pos = player->GetPosition();

    const MapRegion* region = gRegion;
    const InteriorVolume* found = gVolume;
    if (!found || !Inside(*region, *found, pos, kLeaveMargin, kLeaveMargin)) {
        found = nullptr;
        region = nullptr;
        for (const MapRegion& r : Settings::regions) {
            for (const InteriorVolume& v : r.interiors) {
                if (!Inside(r, v, pos, -kEnterShrink, 0.0f)) continue;
                found = &v;
                region = &r;
                break;
            }
            if (found) break;
        }
    }

    int want = found ? found->area : 0;
    if (found == gVolume && want == gOurArea) return;

    gVolume = found;
    gRegion = region;
    if (want == gOurArea) return;   // a second box for the same interior; the area is already right

    SetArea(want, player, pos);
    gOurArea = want;
    if (want)
        Log::Write("[%s] interior: area %d (%s) at SA (%.1f, %.1f, %.1f)", region->name.c_str(),
                   want, found->name.empty() ? "unnamed" : found->name.c_str(), pos.x, pos.y, pos.z);
    else
        Log::Write("interior: back to the outside world (area 0) at SA (%.1f, %.1f, %.1f)",
                   pos.x, pos.y, pos.z);
}

void __cdecl HookedUpdate() {
    UpdateHook().ccall<void>();
    Update();
}

// A volume that covers nothing the player can stand in, or that reaches outside the region's own
// tile grid, is almost always a typo in a coordinate; say so at start-up rather than leaving the
// user to work out why an interior never appears or why the world vanishes in the middle of a road.
void Check(const MapRegion& r, const InteriorVolume& v) {
    float west = r.TileWest(0), east = r.TileWest(r.tilesX);
    float north = r.TileNorth(0), south = r.TileNorth(r.tilesY);
    float minX = v.minX + r.originX, maxX = v.maxX + r.originX;
    float minY = v.minY + r.originY, maxY = v.maxY + r.originY;
    if (minX < west || maxX > east || minY < south || maxY > north)
        Log::Write("[%s]   ^ reaches outside the region's tile grid (SA x %.0f..%.0f, y %.0f..%.0f)",
                   r.name.c_str(), west, east, south, north);
    if (v.maxX - v.minX <= 0.0f || v.maxY - v.minY <= 0.0f || v.maxZ - v.minZ <= 0.0f)
        Log::Write("[%s]   ^ has no volume; the player can never be inside it", r.name.c_str());
}

}  // namespace

namespace Interiors {

void Install() {
    int total = 0;
    for (const MapRegion& r : Settings::regions) total += (int)r.interiors.size();
    if (!total) return;

    for (const MapRegion& r : Settings::regions) {
        if (r.interiors.empty()) continue;
        Log::Write("[%s] interiors: %d volume(s); inside one of them the game shows only that "
                   "area's entities and the ones marked 13 (\"everywhere\")",
                   r.name.c_str(), (int)r.interiors.size());
        for (const InteriorVolume& v : r.interiors) {
            Log::Write("[%s]   area %-3d SA x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f  %s",
                       r.name.c_str(), v.area,
                       v.minX + r.originX, v.maxX + r.originX,
                       v.minY + r.originY, v.maxY + r.originY,
                       v.minZ + r.originZ, v.maxZ + r.originZ,
                       v.name.empty() ? "" : v.name.c_str());
            Check(r, v);
        }
    }

    // Overlapping boxes for different interiors would make which one you get depend on the order
    // they were listed in, so it is worth one O(n^2) pass over a handful of boxes to say so.
    for (const MapRegion& r : Settings::regions)
        for (size_t i = 0; i < r.interiors.size(); i++)
            for (size_t j = i + 1; j < r.interiors.size(); j++) {
                const InteriorVolume& a = r.interiors[i];
                const InteriorVolume& b = r.interiors[j];
                if (a.area == b.area) continue;
                if (a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY &&
                    a.minZ <= b.maxZ && b.minZ <= a.maxZ)
                    Log::Write("[%s] interiors: areas %d and %d overlap; the one listed first wins",
                               r.name.c_str(), a.area, b.area);
            }

    UpdateHook() = safetyhook::create_inline(reinterpret_cast<void*>(kEntryExitManagerUpdate),
                                             reinterpret_cast<void*>(&HookedUpdate));
    if (!UpdateHook()) {
        Log::Write("could not hook CEntryExitManager::Update; region interiors will never be entered");
        return;
    }
    gInstalled = true;
}

void Describe(std::string& out) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "currArea %d, ours %d, %s%s", CurrArea, gOurArea,
             gVolume ? (gVolume->name.empty() ? "an unnamed volume" : gVolume->name.c_str())
                     : "no interior volume",
             gInstalled ? "" : " (interior volumes not installed)");
    out = buffer;
}

}  // namespace Interiors
