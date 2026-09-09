// Finding the ground, and being told when there is none.
//
// THE ROUTINE NOT TO USE, and why it is the one everybody reaches for.
//
// CWorld::FindGroundZForCoord (0x569660) takes an x and a y, plants 1000.0 (the 0x447A0000 it
// writes to [esp+0x34]) as the top of a vertical line and calls CWorld::ProcessVerticalLine. It
// looks like exactly the right question - "what is the highest ground at this x and y", with no z
// to get wrong. But read its failure path at 0x5696A5:
//
//     test al, al                  ; did ProcessVerticalLine hit anything?
//     jne  0x5696B3                ; yes - return the hit
//     fld  dword ptr [0x858BA4]    ; no  - return the float at 0x858BA4
//     ret
//
// and the float at 0x858BA4 is **20.0**. When there is no collision under a point, this routine
// returns twenty, and twenty is a perfectly ordinary height: Vice City's streets sit around 12 to
// 20 in the region's own coordinates. So a failed probe is indistinguishable from a good answer,
// and a caller that trusts it puts a pedestrian several units above the road and watches them fall.
// That is exactly what happened, and it is why this file exists rather than a one-line call.
//
// THE ROUTINE TO USE. CWorld::FindGroundZFor3DCoord (0x5696C0) takes a starting z, casts down to
// -1000.0 (the 0xC47A0000 pushed at 0x5696F7) and takes a bool* it actually sets: at 0x569711 it
// writes 0 through it when nothing was hit. It reports failure instead of inventing an answer.
// src/region/FallRecovery.cpp already uses it, with a ring search around the point and the highest
// hit winning, and that code is confirmed working in game - this is the same question asked the
// same way, in one place, so that the population code and the fall recovery cannot drift apart.
//
// The cost of the honest routine is that it searches DOWNWARD from where you tell it, so it will
// not find a surface above the height you offered. Everything this project spawns stands on a
// graph the donor game wrote for its own collision, and measuring that graph against the imported
// map put the median disagreement at 1.3 units with a tail out to about three and a half. So
// Under() lifts the start of the search well clear of that tail before looking down - far enough
// to find a street the graph is under, near enough not to reach the floor above it.
#include <plugin.h>
#include <CWorld.h>
#include <cstdio>
#include "Ground.h"

using namespace plugin;

namespace {

// Twelve units. The measured graph-to-collision disagreement across a whole imported city is 1.3
// units median and about 3.5 at the tail, so twelve is roughly three times the worst of it - and
// it is still well under the height of one storey, which is the thing that must not be caught by
// accident. A node under a bridge stays under the bridge.
constexpr float kProbeLift = 12.0f;

// A hit this far below where we started looking is not the floor this point belongs to - it is the
// next one down, or the sea bed under a hole in the collision. Placing somebody there is worse
// than not placing them, so it counts as "nothing here".
constexpr float kMaxDrop = 60.0f;

}   // namespace

namespace Ground {

float ProbeLift() { return kProbeLift; }

bool Below(float x, float y, float fromZ, float& outZ) {
    bool found = false;
    float z = CWorld::FindGroundZFor3DCoord(x, y, fromZ, &found, nullptr);
    if (!found) return false;
    if (fromZ - z > kMaxDrop) return false;
    outZ = z;
    return true;
}

bool Under(float x, float y, float claimedZ, float& outZ) {
    return Below(x, y, claimedZ + kProbeLift, outZ);
}

// Counting the decks.
//
// FindGroundZFor3DCoord casts from the z it is given down to -1000 and stops at the first thing it
// hits, so asking it again from just under that hit finds the next one down. Repeat and you have
// every surface at that x and y, highest first. Half a unit of step is enough to clear a surface
// without skipping the next one - collision faces are not that close together - and the whole
// thing is bounded by `max` and by a floor of -300 so a point over open sea does not spin.
int Surfaces(float x, float y, float fromZ, float* out, int max) {
    int n = 0;
    float z = fromZ;
    while (n < max && z > -300.0f) {
        bool found = false;
        float g = CWorld::FindGroundZFor3DCoord(x, y, z, &found, nullptr);
        if (!found) break;
        out[n++] = g;
        z = g - 0.5f;
    }
    return n;
}

// Under the world, as opposed to under something.
//
// A crowd follows the player, which is right, and it kept following one who was flying a jetpack
// beneath the map - so people appeared around him down there, correctly by the rule and absurdly
// by any other measure, and were reported as "peds spawning under the map". That is worth a rule
// of its own rather than an accident, and the rule has to be careful: "there is collision above
// me" is true in every tunnel, under every bridge and inside every building, and a crowd should
// certainly carry on in those. What is never right is a player with NO floor beneath them at all.
//
// So this asks for every surface from well overhead and answers true only when all of them are
// above the point. A tunnel has a road under your feet and fails the test; open sky under the map
// has nothing under your feet and passes it.
constexpr float kUnderWorldSlack = 1.5f;
constexpr float kCeilingProbe = 400.0f;

bool BeneathEverything(float x, float y, float z) {
    float hits[8];
    int n = Surfaces(x, y, z + kCeilingProbe, hits, 8);
    if (n == 0) return false;   // nothing anywhere: not our business, and not a decision to make
    for (int i = 0; i < n; i++)
        if (hits[i] <= z + kUnderWorldSlack) return false;   // a floor at or below us
    return true;
}

bool Standing(float x, float y, float z, float& outZ) {
    float hits[8];
    int n = Surfaces(x, y, z + kCeilingProbe, hits, 8);
    for (int i = 0; i < n; i++) {
        if (hits[i] <= z + kUnderWorldSlack) { outZ = hits[i]; return true; }
    }
    return false;
}

void Describe(float x, float y, float fromZ, char* out, unsigned size) {
    float hits[8];
    int n = Surfaces(x, y, fromZ, hits, 8);
    int used = snprintf(out, size, "%d surface(s) from %.1f down:", n, fromZ);
    for (int i = 0; i < n && used > 0 && (unsigned)used < size; i++)
        used += snprintf(out + used, size - used, "%s %.2f", i ? "," : "", hits[i]);
    if (n == 0 && (unsigned)used < size) snprintf(out + used, size - used, " nothing at all");
}

}   // namespace Ground
