// Falling out of an imported region puts you back on that region's ground, not in San Andreas.
//
//
// WHAT SAN ANDREAS DOES, AND WHY IT IS WRONG HERE
//
// Falling through the map is not handled by the death or arrest code and it has nothing to do with
// restart points. It is CWorld::RemoveFallenPeds (0x565CB0) and CWorld::RemoveFallenCars
// (0x565E80), called from the tail of CWorld::Process (0x568AB5 and 0x568AC5) on frames where
// CTimer::m_FrameCounter & 7 is 1 and 5 respectively - so each runs once every eight frames.
//
// Both walk their pool and pick out everything whose z has gone below the float at 0x859014,
// which is -100. An ordinary ped or a car nobody cares about is deleted. The player, the player's
// vehicle, and mission-owned entities are relocated instead, like this (read off the call at
// 0x565D74 for the ped and 0x565F91 for the car, and the branches under each):
//
//     CPathFind::FindNodeClosestToCoors(pos, type, 999999.9f, ...)   ecx = ThePaths (0x96F050)
//     if the search came back 0xFFFF:
//         ped: write -95.0 straight into the matrix's z and zero the move speed - so it falls to
//              -100 again next frame, and again, for ever
//         car: keep x and y, set z to 0
//     otherwise:
//         node = ThePaths.m_pPathNodes[area][index]
//         Teleport(node.x * 0.125, node.y * 0.125, node.z * 0.125 + 2.0, resetRotation)  vtable+0x38
//         zero m_vecMoveSpeed (entity + 0x44)
//
// The distance limit is 0x497423FE, which is 999999.875 - i.e. there is no limit, the whole path
// graph is eligible. FindNodeClosestToCoors (0x44F460) turns the coordinates into one of the 8x8
// path regions, clamps that region to 0..7, and then spirals outward over the rest of the grid, so
// "closest" really does mean closest anywhere in San Andreas.
//
// And every path node San Andreas has is in San Andreas. A region is thousands of units away and
// has none of its own - node positions are int16 * 0.125, which reaches +/-4095.875 and cannot
// describe a point out there at all (docs/known-limitations.md). So the nearest node to somebody
// who has just fallen through Ocean Drive is on the San Andreas mainland, and that is exactly what
// the player reported: fall through the imported map and you are put back in San Andreas.
//
//
// WHAT THIS DOES INSTEAD
//
// The same thing San Andreas is trying to do - put you back on solid ground near where you fell -
// but asking the region's own collision where the ground is, rather than a path graph that does
// not cover it.
//
// It hooks both functions and runs first. If the player (or the vehicle they are in) is below the
// fall floor AND standing over one of the regions, they are relocated here and are therefore above
// the floor by the time the game's own loop runs, so the game leaves them alone and every other
// ped and car in the pool is handled exactly as it always was. Outside a region nothing is touched
// at all: San Andreas' own recovery works perfectly well on San Andreas' own map and is what a
// player there expects.
//
// Finding the ground is CWorld::FindGroundZFor3DCoord (0x5696C0), the same call Teleport.cpp uses
// and for the same reason: it reports whether the downward line hit anything, where its simpler
// sibling FindGroundZForCoord (0x569660) returns a flat 20.0 for "nothing there", which is
// indistinguishable from a real answer. The search is rings of samples around the point the player
// fell from, widening until one of them finds something:
//
//   * The ring at radius zero is the fall point itself, which usually finds nothing - a hole in the
//     collision is why the player is here - so the first ring that answers is normally a few units
//     away. That is the "somewhere nearby" the recovery is supposed to be.
//   * Within a ring the sample whose ground is highest wins, because the line is cast downward from
//     just above the height the player was last at, so the highest hit is the floor they were
//     standing on rather than something in a basement under it.
//   * If no ring answers from that height it tries again from high above the region, which catches
//     a stale reference height, and only then falls back to the last place the player was known to
//     be standing on solid ground.
//
// That last one is the safety net rather than the first choice, deliberately. The last safe spot is
// by definition next to the hole, so putting the player straight back on it invites them to fall
// through again; a ring search around the fall point lands them on whatever is actually solid.
//
// Repeated falls are counted, and each one starts its search from a wider ring than the last, so a
// spot that swallows the player twice is not offered a third time.
//
//
// ONCE THE PLAYER IS OVER A REGION, THIS NEVER HANDS THEM BACK TO SAN ANDREAS
//
// The first version did, whenever every search failed - and that is the one thing the whole feature
// exists to prevent. Handing back to the game is only ever right OUTSIDE a region, where the game's
// own recovery is correct. Over a region the answer has to come from the region, so the chain now
// ends in places that need nothing streamed and cannot fail:
//
//   1. the ring search, which needs the region's collision to answer;
//   2. the last foothold - and a foothold is now recorded from PHYSICS as well as from a line test.
//      If the player's vertical speed is nil and their height has not dropped between two samples,
//      something is holding them up, whatever any line test says; the position they are standing at
//      is then by definition a place they can stand, and putting them back on it asks nothing of
//      the collision at all;
//   3. the region's own origin, if the player has never had a foothold in it at all - they arrived
//      and fell, or arrived already falling. It is a long way to be moved, but it is a completely
//      different piece of the same city and may have collision where the corner they fell into
//      does not;
//   4. failing even that, they are held just above the floor over their own x and y rather than
//      sent to another state, for a bounded number of tries, which is also as many chances for the
//      collision to arrive. Only when a region has answered nothing at all, that many times
//      running, does the game get the fall back - and the log says exactly that, because by then
//      the region has no collision anywhere the player has been and a loop over it would be worse.
//
// One more thing is tried before believing "no ground here": the collision at the fall point is
// forced into memory with CStreaming::LoadSceneCollision and the rings are walked once more. It
// costs a disc hitch, so it happens once per run of falls and only after the free searches have
// already failed - and whether it changes the answer is written to the log, because that is the
// difference between "the collision was not resident" and "there is genuinely nothing there".
//
// For the record, what was checked when this was suspected of being a streaming problem: a region's
// collision is loaded into col slot 0, and CColStore's loops all start at index 1 - `mov edi, 1` at
// 0x410E09 in RemoveAllCollision and 0x410E69 in LoadAllCollision - so it is never unloaded. What
// collision the streamer does move is chosen from the player's own position plus twenty times their
// velocity (0x410870-0x4108CD in CColStore::LoadCollision), with no camera term anywhere in it, and
// CWorld::ProcessVerticalLineSectorList (0x5632B0) skips an entity only for its scan code, its
// m_bUsesCollision bit, or CWorld::pIgnoreEntity. So none of this should depend on where anyone is
// looking. The forced load is kept anyway: it is free unless everything else has already failed,
// and its log line is what will settle the question if it ever comes up again.
//
//
// WHAT IT DOES NOT DO
//
// Dying and being arrested are a different mechanism and are not touched. Those go through
// CGameLogic::Update -> CRestart::FindClosestHospitalRestartPoint (0x460850) /
// FindClosestPoliceRestartPoint (0x460A50) -> CGameLogic::RestorePlayerStuffDuringResurrection
// (0x442060), and since every restart point San Andreas has is in San Andreas, dying in an
// imported city still wakes you in an All Saints or a Los Santos police cell. See
// docs/known-limitations.md for what giving a region its own restart points would cost.
#include <plugin.h>
#include <CWorld.h>
#include <CTimer.h>
#include <CStreaming.h>
#include <CPlayerPed.h>
#include <CVehicle.h>
#include <CCutsceneMgr.h>
#include <safetyhook.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "FallRecovery.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kRemoveFallenPeds = 0x565CB0;
constexpr uintptr_t kRemoveFallenCars = 0x565E80;

// The height both functions test against, read from the executable rather than written down here,
// so that if anything else has moved the floor we move with it.
const float* const kFallFloor = reinterpret_cast<const float*>(0x859014);   // -100.0 in vanilla

// Their prologues in San Andreas 1.0 US: `sub esp,0x2c / push ebx / mov ebx,[CPools::ms_pPedPool]`
// and the same with the vehicle pool. Checked before either hook goes in, so a differently
// patched executable - or another mod that got here first - leaves the game exactly as it found it.
const uint8_t kPedsPrologue[] = { 0x83, 0xEC, 0x2C, 0x53, 0x8B, 0x1D, 0x90, 0x44, 0xB7, 0x00 };
const uint8_t kCarsPrologue[] = { 0x83, 0xEC, 0x2C, 0x53, 0x8B, 0x1D, 0x94, 0x44, 0xB7, 0x00 };

// CEntity::Teleport's vtable slot, and the by-value-vector trap that goes with it. Both are
// explained at length in Teleport.cpp; the short version is that this is slot 0x38 in this
// executable, and that a plugin-sdk CVector passed by value from clang does not reach an MSVC
// function the way it expects, so the vector has to be a bare three-float struct. It is also the
// slot San Andreas itself calls through here - `call dword ptr [edx+0x38]` at 0x565E3D and
// 0x566069 - which is as good a confirmation as there is.
constexpr int kTeleportSlot = 0x38 / sizeof(void*);
constexpr uintptr_t kCodeLow = 0x401000, kCodeHigh = 0x2000000;
struct RawVector { float x, y, z; };
typedef void(__thiscall* TeleportFn)(void* entity, RawVector pos, bool resetRotation);

// CEntity::GetDistanceFromCentreOfMassToBaseOfModel: ground level plus this is where the entity's
// origin belongs. Scripts::SetCharCoordinates uses it at 0x464E0B for exactly this.
typedef float(__thiscall* BaseOfModelFn)(void* entity);
const BaseOfModelFn CallDistanceToBaseOfModel = reinterpret_cast<BaseOfModelFn>(0x536BE0);

// How far outside its tile grid a region still counts as its ground. Walking off the edge of an
// imported map and dropping into the void is exactly the case this exists for, and at the moment
// of the fall the player is already past the last tile.
constexpr float kFootprintMargin = 300.0f;

// The rings the search walks, in world units, and how many samples go round each. The first is the
// fall point itself.
const float kRingRadius[] = { 0.0f, 4.0f, 10.0f, 20.0f, 40.0f, 80.0f, 160.0f };
constexpr int kRings = (int)(sizeof(kRingRadius) / sizeof(kRingRadius[0]));
constexpr int kSamplesPerRing = 8;

// How far above the reference height the downward line starts. Enough to clear a kerb or a step
// the player was standing on, not enough to reach the floor above.
constexpr float kProbeLift = 20.0f;
// And where the second pass starts from, measured off the region's own floor, so a reference height
// that is stale or nonsense still gets an answer. Same height San Andreas' own
// FindGroundZForCoord casts from.
constexpr float kProbeHeight = 1000.0f;

// How close to the ground the player has to be for this to count as "standing on it" and be worth
// remembering as the safety net.
constexpr float kStandingClearance = 3.0f;

// The physics half of "they are standing on something", for when no line test will answer. Speeds
// here are per frame, not per second: a ped or a car at rest on the ground sits at nil vertical
// speed because the collision cancels the gravity that was applied to it, and anything actually
// falling is well past this by the time a check four frames later sees it.
constexpr float kRestingSpeed = 0.04f;
// And the second sample that confirms it: two checks in a row with no drop between them. One
// sample cannot tell the top of a fall from standing still.
constexpr float kRestingDrop = 0.05f;

// A relocation this soon after the last one is the same fall happening again, so the next search
// starts from a wider ring.
constexpr unsigned kRepeatWindowMs = 6000;
// Once every ring has been tried as a starting ring the search has nothing left to offer, so stop
// paying for it and go straight to the foothold.
constexpr int kRingsExhausted = kRings;
// Where to hold the player when a region has answered nothing at all: over their own x and y, this
// far above the floor the game tests against, which is another chance for its collision to arrive.
// Deliberately a short drop rather than a high one - the hold has to come round again well inside
// kRepeatWindowMs, or the run of falls would look like a new one every time and the count below
// would never be reached.
constexpr float kHoldHeight = 5.0f;
// How many times running that may happen before the region is taken to have no collision near the
// player at all and the game gets the fall back. This is the ONLY route out to San Andreas.
constexpr int kHoldAttempts = 8;

// A sanity bound on what GetDistanceFromCentreOfMassToBaseOfModel hands back, since a model whose
// collision is not in memory can answer with anything.
constexpr float kMaxBaseOffset = 5.0f;

SafetyHookInline& PedsHook() { static SafetyHookInline h; return h; }
SafetyHookInline& CarsHook() { static SafetyHookInline h; return h; }

bool gInstalled = false;

// The last place the player was seen standing on a region's own ground. It does two jobs: its
// height is the level the ring search casts down from, and the spot itself is the safety net for
// when the search finds nothing at all.
struct SafeSpot {
    const MapRegion* region = nullptr;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Whether z is ground level - from a line test, so the entity's own base offset still has to be
    // added - or the entity origin itself, taken from physics, which goes back exactly as it is.
    bool zIsGround = false;
};
SafeSpot gSafe;

// The previous sample, so "they did not drop between two checks" can be asked.
bool gHavePrev = false;
float gPrevZ = 0.0f;

int gRepeats = 0;                 // consecutive relocations inside kRepeatWindowMs of each other
int gHeld = 0;                    // consecutive times a region answered nothing and we held them up
bool gForcedCollision = false;    // the disc has already been hit once for this run of falls
unsigned gLastRecoveryMs = 0;
int gTotalRecoveries = 0;

const MapRegion* RegionAt(float x, float y, float margin) {
    for (const MapRegion& r : Settings::regions) {
        if (r.tilesX <= 0 || r.tilesY <= 0) continue;
        if (x < r.TileWest(0) - margin || x > r.TileWest(r.tilesX) + margin) continue;
        if (y < r.TileNorth(r.tilesY) - margin || y > r.TileNorth(0) + margin) continue;
        return &r;
    }
    return nullptr;
}

// The best ground under one ring, or false if nothing in it found any. "Best" is the highest hit:
// the line is cast downward from just above where the player was, so the highest thing it meets is
// the floor they were on rather than a basement or a tunnel below it.
bool ProbeRing(float x, float y, float fromZ, float radius, float& outX, float& outY, float& outZ) {
    const float floor = *kFallFloor;
    bool any = false;
    for (int i = 0; i < (radius <= 0.0f ? 1 : kSamplesPerRing); i++) {
        float angle = 6.2831853f * (float)i / (float)kSamplesPerRing;
        float px = x + radius * cosf(angle);
        float py = y + radius * sinf(angle);
        bool found = false;
        float g = CWorld::FindGroundZFor3DCoord(px, py, fromZ, &found, nullptr);
        // Ground below the floor would only put the player straight back into the fall.
        if (!found || g <= floor + 5.0f) continue;
        if (!any || g > outZ) { outX = px; outY = py; outZ = g; any = true; }
    }
    return any;
}

// Rings from `firstRing` outward, stopping at the first one that answers.
bool SearchGround(float x, float y, float fromZ, int firstRing,
                  float& outX, float& outY, float& outZ, float& outRadius) {
    if (firstRing < 0) firstRing = 0;
    for (int ring = firstRing; ring < kRings; ring++)
        if (ProbeRing(x, y, fromZ, kRingRadius[ring], outX, outY, outZ)) {
            outRadius = kRingRadius[ring];
            return true;
        }
    return false;
}

// Put `mover` at (x, y, z), the way the game does it: through the entity's own Teleport override,
// then with its speed zeroed. `zIsGround` says whether z is ground level, in which case the
// entity's own base offset is added, or already an entity origin, in which case it is not - a
// foothold taken from physics is the second kind, and adding an offset to it would lift the player
// a little further off the floor every time they used it.
bool PlaceOnGround(CEntity* mover, float x, float y, float z, bool zIsGround, bool resetRotation) {
    void** vtable = *reinterpret_cast<void***>(mover);
    void* slot = vtable ? vtable[kTeleportSlot] : nullptr;
    uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    if (slotAddr < kCodeLow || slotAddr >= kCodeHigh) {
        Log::Write("fall recovery: the Teleport slot holds %08X, which is not code; leaving the "
                   "fall to the game", (unsigned)slotAddr);
        return false;
    }

    float base = 0.0f;
    if (zIsGround) {
        base = CallDistanceToBaseOfModel(mover);
        if (!(base >= 0.0f) || base > kMaxBaseOffset) base = 1.0f;   // also catches a NaN
    }

    RawVector at = { x, y, z + base };
    reinterpret_cast<TeleportFn>(slot)(mover, at, resetRotation);
    static_cast<CPhysical*>(mover)->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
    return true;
}

// Everything below the fall floor that is over a region. Returns true when the player was moved.
bool Recover(CEntity* mover, CPlayerPed* player, const MapRegion& r, bool isVehicle) {
    unsigned now = CTimer::m_snTimeInMilliseconds;
    if (gTotalRecoveries && now - gLastRecoveryMs <= kRepeatWindowMs) {
        gRepeats++;
    } else {                            // a new run of falls: the disc may be hit again for it
        gRepeats = 0;
        gHeld = 0;
        gForcedCollision = false;
    }

    CVector fell = mover->GetPosition();
    // The height to look from is the last ground the player was standing on in this region - which
    // is the floor they have just gone through, so it is the level the replacement ground should be
    // at. Deliberately not "the last height they were seen at": this check runs while they are
    // falling too, so that would be about -100 by the time it mattered.
    float referenceZ = gSafe.region == &r ? gSafe.z : r.originZ;

    // Each repeat of the same fall starts a ring wider than the last, so a hole that has already
    // swallowed the player is not offered back to them.
    int firstRing = gRepeats < kRings ? gRepeats : kRings - 1;

    float x = 0.0f, y = 0.0f, z = 0.0f, radius = 0.0f;
    const char* how = nullptr;
    bool zIsGround = true;

    // Once every ring has been a starting ring the search has run out of things to say; skip
    // straight to the foothold rather than pay for it again.
    if (gRepeats < kRingsExhausted) {
        if (SearchGround(fell.x, fell.y, referenceZ + kProbeLift, firstRing, x, y, z, radius)) {
            how = "the ground where you fell";
        } else if (SearchGround(fell.x, fell.y, r.originZ + kProbeHeight, firstRing, x, y, z,
                                radius)) {
            how = "the ground below it, found from above";
        } else if (!gForcedCollision) {
            // Nothing answered. Before believing there is nothing there, make sure the region's
            // collision really is in memory at this point - and say in the log whether that was the
            // difference, because it is the difference between a streaming problem and a hole.
            // Once per run of falls: it blocks on the disc, and the clock is stopped around it so
            // the pause does not land on the physics as one enormous time step.
            gForcedCollision = true;
            CVector at(fell.x, fell.y, referenceZ);
            CTimer::Suspend();
            CStreaming::LoadSceneCollision(&at);
            CTimer::Resume();
            if (SearchGround(fell.x, fell.y, referenceZ + kProbeLift, firstRing, x, y, z, radius))
                how = "the ground where you fell, which was not in memory until it was asked for";
            else if (SearchGround(fell.x, fell.y, r.originZ + kProbeHeight, firstRing, x, y, z,
                                  radius))
                how = "the ground below it, which was not in memory until it was asked for";
            Log::Write("[%s] fall recovery: nothing answered under (%.1f, %.1f), so the collision "
                       "there was forced into memory - and %s. %s",
                       r.name.c_str(), fell.x, fell.y,
                       how ? "that found ground" : "there is still nothing",
                       how ? "So this was the region's collision not being resident."
                           : "So this is a real hole, not a streaming problem.");
        }
    }

    if (!how && gSafe.region == &r) {
        // The safety net: the last place the player was held up by something. If it came from a
        // line test, probe it again for a current ground level; if it came from physics, it is an
        // entity origin they were resting at and needs nothing confirmed at all.
        x = gSafe.x;
        y = gSafe.y;
        z = gSafe.z;
        zIsGround = gSafe.zIsGround;
        if (zIsGround) {
            bool found = false;
            float g = CWorld::FindGroundZFor3DCoord(x, y, z + kProbeLift, &found, nullptr);
            if (found) z = g;
        }
        radius = sqrtf((x - fell.x) * (x - fell.x) + (y - fell.y) * (y - fell.y));
        how = "the last ground you stood on";
    }

    if (!how) {
        // Still nothing, and the player has never had a foothold in this region - they arrived and
        // fell, or arrived already falling. Try the region's own origin, which is a completely
        // different piece of its map and may well have collision when the corner they fell into
        // does not. It is a long way to be moved, but it is somewhere in the same city, which is
        // the whole point.
        if (SearchGround(r.originX, r.originY, r.originZ + kProbeHeight, 0, x, y, z, radius)) {
            radius = sqrtf((x - fell.x) * (x - fell.x) + (y - fell.y) * (y - fell.y));
            how = "the middle of the region, nothing at all having answered where you fell";
        }
    }

    if (!how) {
        // The region has answered nothing: no ground within reach at two heights, its collision
        // forced in, and no foothold ever recorded in it. Hold the player in the air over their own
        // x and y rather than send them to another state - which is also another chance for the
        // collision to arrive - and only give the fall back to the game once that has failed this
        // many times running, because by then the region genuinely has nothing under the player and
        // an endless loop over it would be worse than being on the mainland.
        gHeld++;
        if (gHeld > kHoldAttempts) {
            Log::Write("[%s] fall recovery: %d times running, this region has had no ground within "
                       "%.0f units of (%.1f, %.1f) at any height, none at its own origin, its "
                       "collision forced in, and no foothold ever recorded in it - so it has no "
                       "collision anywhere the player has been. Letting San Andreas take this one, "
                       "which means the mainland; check that the region's Col files loaded.",
                       r.name.c_str(), gHeld - 1, kRingRadius[kRings - 1], fell.x, fell.y);
            return false;
        }
        x = fell.x;
        y = fell.y;
        z = *kFallFloor + kHoldHeight;
        zIsGround = false;
        radius = 0.0f;
        how = "held just above the floor while the region's collision catches up";
    } else {
        gHeld = 0;
    }

    // A car gets its rotation reset and a man does not, which is what the game does at 0x566012
    // (push 1) and 0x565DE6 (push 0).
    if (!PlaceOnGround(mover, x, y, z, zIsGround, isVehicle)) return false;
    // The man in the car is carried by the car's own matrix from next frame, but the game's ped
    // pass may run before that happens and would see him still under the world. Put him where the
    // car now is so nothing else has an opinion about it.
    if (isVehicle && player) {
        CVector to(x, y, z + 1.0f);
        player->SetPosn(to);
    }

    gLastRecoveryMs = now;
    gTotalRecoveries++;
    Log::Write("[%s] fall recovery: %s went under the world at (%.1f, %.1f, %.1f) and is now at "
               "(%.1f, %.1f, %.1f), %.0f units away - %s%s",
               r.name.c_str(), isVehicle ? "the player's vehicle" : "the player", fell.x, fell.y,
               fell.z, x, y, z, radius, how,
               gRepeats ? " (repeat fall, searching wider)" : "");
    return true;
}

void Check() {
    CPlayerPed* player = FindPlayerPed();
    if (!player || !player->m_pRwObject) return;
    // Dead or arrested is the resurrection code's business, not ours, and two things moving the
    // player in the same frame is how you end up inside geometry.
    if (CWorld::Players[0].m_nPlayerState != PLAYERSTATE_PLAYING) return;
    if (CCutsceneMgr::ms_running || CCutsceneMgr::ms_cutsceneProcessing) return;

    // Move the car, not the man sitting in it: a passenger's matrix is written from the vehicle's
    // every frame, so moving him alone is undone immediately. Same rule as Teleport.cpp.
    CVehicle* vehicle = FindPlayerVehicle(-1, false);
    CEntity* mover = vehicle ? static_cast<CEntity*>(vehicle) : static_cast<CEntity*>(player);
    CVector pos = mover->GetPosition();

    const MapRegion* r = RegionAt(pos.x, pos.y, kFootprintMargin);

    if (pos.z > *kFallFloor) {
        if (!r) {                       // on San Andreas' own map; none of this is our business
            gHavePrev = false;
            return;
        }
        // Are they standing on something? Two ways to know, and only one of them asks anything of
        // the collision - which is the whole point, because if the line test could always be
        // trusted there would be no fall to recover from in the first place.
        bool found = false;
        float g = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found, nullptr);
        if (found && pos.z - g <= kStandingClearance) {
            gSafe = { r, pos.x, pos.y, g, true };
        } else {
            // Nothing answered, but physics can still say it: nil vertical speed, and no drop
            // since the previous check. Something is holding them up whatever the line test
            // thinks, so where they are standing is a place they can be put back.
            const CVector& speed = static_cast<CPhysical*>(mover)->m_vecMoveSpeed;
            bool still = speed.z > -kRestingSpeed && speed.z < kRestingSpeed;
            bool onFoot = !vehicle && player->bIsStanding;
            if ((still || onFoot) && gHavePrev && pos.z >= gPrevZ - kRestingDrop)
                gSafe = { r, pos.x, pos.y, pos.z, false };
        }
        gPrevZ = pos.z;
        gHavePrev = true;
        return;
    }
    gHavePrev = false;   // falling; the next sample must not be compared against a mid-air one

    // Under the world. Over San Andreas that is the game's own business and it does it well.
    if (!r) return;
    Recover(mover, player, *r, vehicle != nullptr);
}

void __cdecl HookedRemoveFallenPeds() {
    Check();
    PedsHook().ccall<void>();
}

void __cdecl HookedRemoveFallenCars() {
    Check();
    CarsHook().ccall<void>();
}

bool BytesMatch(uintptr_t at, const uint8_t* expected, size_t length) {
    return memcmp(reinterpret_cast<const void*>(at), expected, length) == 0;
}

}   // namespace

namespace FallRecovery {

void Install() {
    if (Settings::regions.empty() || !Settings::world.fallRecovery) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the fall recovery is off, the server owns where the player is");
        return;
    }

    if (!BytesMatch(kRemoveFallenPeds, kPedsPrologue, sizeof(kPedsPrologue)) ||
        !BytesMatch(kRemoveFallenCars, kCarsPrologue, sizeof(kCarsPrologue))) {
        Log::Write("fall recovery: CWorld::RemoveFallenPeds/RemoveFallenCars do not start with the "
                   "bytes San Andreas 1.0 US ships, so something else has them; nothing patched, "
                   "and falling out of an imported region will still put you in San Andreas");
        return;
    }

    PedsHook() = safetyhook::create_inline(reinterpret_cast<void*>(kRemoveFallenPeds),
                                           reinterpret_cast<void*>(&HookedRemoveFallenPeds));
    CarsHook() = safetyhook::create_inline(reinterpret_cast<void*>(kRemoveFallenCars),
                                           reinterpret_cast<void*>(&HookedRemoveFallenCars));
    if (!PedsHook() && !CarsHook()) {
        Log::Write("fall recovery: neither CWorld::RemoveFallenPeds nor RemoveFallenCars could be "
                   "hooked; falling out of an imported region will still put you in San Andreas");
        return;
    }
    if (!PedsHook())
        Log::Write("fall recovery: CWorld::RemoveFallenPeds could not be hooked; on foot the fall "
                   "is still caught, one frame later, by the vehicle pass");
    if (!CarsHook())
        Log::Write("fall recovery: CWorld::RemoveFallenCars could not be hooked; in a vehicle the "
                   "fall is still caught, one frame earlier, by the ped pass");

    gInstalled = true;
    Log::Write("fall recovery: below z %.0f inside a region the player is put back on the region's "
               "own ground, searching out to %.0f units; outside a region San Andreas' own recovery "
               "is left alone", *kFallFloor, kRingRadius[kRings - 1]);
}

// F6: put the player under the world where they stand, which is the one thing that is otherwise
// hard to arrange on purpose - a hole in the collision has to be found first. The drop itself is
// deliberately not special-cased anywhere above: what follows is exactly what follows a real fall,
// including in San Andreas, where the game's own recovery answers it and you can see the difference.
void DebugDrop() {
    CPlayerPed* player = FindPlayerPed();
    if (!player || !player->m_pRwObject) {
        Log::Write("F6: no player to drop");
        return;
    }
    CVehicle* vehicle = FindPlayerVehicle(-1, false);
    CEntity* mover = vehicle ? static_cast<CEntity*>(vehicle) : static_cast<CEntity*>(player);
    CVector from = mover->GetPosition();

    void** vtable = *reinterpret_cast<void***>(mover);
    void* slot = vtable ? vtable[kTeleportSlot] : nullptr;
    uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    if (slotAddr < kCodeLow || slotAddr >= kCodeHigh) {
        Log::Write("F6: the Teleport slot holds %08X, which is not code; not dropping",
                   (unsigned)slotAddr);
        return;
    }

    // Fifty units under the floor the game tests against, so the very next pass over the pool sees
    // it however the frame counter happens to land.
    RawVector to = { from.x, from.y, *kFallFloor - 50.0f };
    reinterpret_cast<TeleportFn>(slot)(mover, to, false);
    static_cast<CPhysical*>(mover)->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);

    const MapRegion* r = RegionAt(from.x, from.y, kFootprintMargin);
    Log::Write("F6: dropped %s from (%.1f, %.1f, %.1f) to z %.1f, %s",
               vehicle ? "the player's vehicle" : "the player", from.x, from.y, from.z, to.z,
               !gInstalled ? "with the recovery not installed - San Andreas will answer this"
                           : r ? "inside a region - the recovery should answer this"
                               : "outside every region - San Andreas' own recovery should answer "
                                 "this, and put you on its path graph");
}

void Describe(std::string& out) {
    char buffer[256];
    if (!gInstalled) {
        out = "not installed";
        return;
    }
    if (gSafe.region)
        snprintf(buffer, sizeof(buffer),
                 "%d recovered fall(s); last foothold [%s] (%.1f, %.1f) z %.1f, %s",
                 gTotalRecoveries, gSafe.region->name.c_str(), gSafe.x, gSafe.y, gSafe.z,
                 gSafe.zIsGround ? "from a line test" : "from physics, no line test needed");
    else
        snprintf(buffer, sizeof(buffer),
                 "%d recovered fall(s); no foothold remembered in any region yet", gTotalRecoveries);
    out = buffer;
}

}   // namespace FallRecovery
