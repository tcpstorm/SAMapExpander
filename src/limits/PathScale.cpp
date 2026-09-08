// Widens what a path node can describe, which is the one thing standing between an imported city
// and traffic.
//
// San Andreas holds a path node's position as three int16 multiplied by 0.125, so a node reaches
// +/-4095.875 and no further. Vice City sits near San Andreas x 6000 and Liberty City near y 6000.
// That is the whole of it: population density is live wherever the player is, and there is no
// map-extent gate anywhere in the spawning code - both checked. Every route the game has to a
// moving vehicle goes through that graph, so an imported city was silent.
//
// The fix is the same one PickupFix has used for pickups and car generators since the beginning,
// and for the same reason. The multiply is inlined everywhere rather than called, but every site
// reads one shared float: 0.125 at 0x858C48 going out, 8.0 at 0x859000 coming back. So the path
// sites are pointed at a float of our own and everything else goes on reading the game's.
//
// What makes this bigger than PickupFix is that the shared constant has 89 readers and most of
// them are nothing to do with paths - CPickup, CTrainNode, CCarGenerator, COccluder, the audio
// boxes, several render pipelines. CCarGenerator in particular compresses at struct offsets
// +4/+6/+8, so its x load is byte-identical in shape to a path node's, and a scan by pattern alone
// would have moved San Andreas' own parked cars. tools/scan_path_scale_sites.py classifies all 491
// of them by the set of struct offsets their sibling 16-bit loads touch, by whether the base
// pointer can be traced to ThePaths' own arrays, and for the leaf accessors by finding their
// callers; tools/path_scale_sites.txt is the result and src/limits/PathScaleSites.h is generated
// from it. 187 decode a CPathNode, 94 decode a CCarPathLink - a second compressed path structure,
// 14 bytes with its coordinates at +0 and +2, and missing it would leave pedestrian routing
// correct while every car-AI routine read its links at the old scale - and 207 are left alone.
//
// Six sites compress rather than decompress. Those are found by shape rather than read from a
// list, because a list was wrong once: the first pass concluded CPathFind::CompleteNewInterior was
// the only encoder and missed 0x44CF40, an out-of-line CPathNode position setter that writes the
// same triple. Nothing calls 0x44CF40 - it has no relative callers and its address appears nowhere
// in the image - but it is patched anyway, because a dead function that is patched costs nothing
// and a live one that was missed costs a quarter of every position it writes.
//
// San Andreas' own shipped nodes are rescaled as they load rather than by editing data\paths. The
// loader copies the 28-byte records straight out of nodes<N>.dat with no transform, so both of its
// call sites are pointed at a hook that lets it run and then walks the area's arrays. There are
// two call sites and both matter: 0x452F75 is the synchronous one and 0x40CA2B is the one inside
// CStreaming::ConvertBufferToObject, which is the one that actually runs while you play.
//
// WHAT THIS COSTS SAN ANDREAS' OWN TRAFFIC, measured against all 64 shipped node files rather
// than argued from the arithmetic. 68237 nodes, 204711 coordinates, 31466 car path links.
//
// Re-expressing an int16 x 0.125 as an int16 x 0.25 loses the low bit and nothing else. A
// coordinate whose stored value is even survives exactly - 56.0% of node coordinates and 51.9% of
// link coordinates do - and an odd one moves by exactly 0.125 units. So:
//
//     worst error on any axis      0.125 units   -  exactly one step of the vanilla encoding
//     worst 3D displacement        0.2165 units
//     mean error per axis          0.055 units
//
// The bound is the point. Rockstar's own exporter snapped every one of these nodes to a 0.125
// grid, so the stored position was already up to 0.0625 from where the road really was; this moves
// it by at most one more of the same step. Whatever tolerance San Andreas' traffic has, it has to
// have had it already, because that is the resolution of its own data.
//
// Against the numbers that decide whether a car looks right: San Andreas' lane width is 5.4 units
// (the float at 0x858C50, read by 26 instructions across CCarCtrl's lane placement and steering
// and CAutoPilot::RemoveOnePathNode). The worst error is 2.3% of a lane on one axis and 4.0% as a
// displacement; the mean is 1.0%. The median distance between a node and its nearest neighbour is
// 7.7 units, so the worst error is 2.8% of a typical node spacing. The one place it is not small
// is the bottom percentile: the closest pair of nodes in the whole game is 0.125 apart, which is
// two nodes on the same spot, and after this they are on the same spot exactly. Nothing chooses
// between two coincident nodes on their separation.
//
// Rescaling also only ever shrinks the stored value, so it cannot overflow: San Andreas' widest
// coordinate is 23936, which becomes 11968.
//
// WHAT IS IN THOSE WORDS, since a position a quarter of a unit out is invisible and a flag that
// got rescaled is a car through a wall. Only the position is:
//
//   CPathNode      +8 / +0x0A / +0x0C are x, y, z and nothing else. The field after z is at +0x0E
//                  and holds the constant 0x7FFE in all 68237 shipped nodes, so z ends at +0x0D.
//                  Both parities occur freely in all three (56%, 53%, 60% even), which no packed
//                  low-bit flag would allow, and no bitwise or zero-extending access to +8, +0x0A
//                  or +0x0C exists anywhere in CPathFind, CPathNode, CCarCtrl, CAutoPilot,
//                  CTrafficLights, CRoadBlocks or Interior::SetupPaths.
//   CCarPathLink   +0 / +2 are x and y. The lane counts are a byte at +0x0B, two nibbles wide -
//                  `shr cl,3` for one side and `and cl,7` for the other; the direction is two
//                  signed bytes at +8 and +9 scaled by 0.01 at 0x858C58; the width and flags are
//                  at +0x0A and +0x0C. None of those share a word with a coordinate.
//
// +0 and +2 are also CNodeAddress' area and index, which is a different structure that happens to
// use the same offsets - so the rescale walks the m_pCarPathLinks array specifically and can never
// reach one. The census settles which is which: those two words in that array hold values spanning
// -23641..23443, which is a world coordinate and not an area id.
//
// Every site is checked to hold the six bytes it is supposed to before a single one is written,
// and a mismatch anywhere patches nothing at all and leaves the game vanilla with a line saying
// which site and what it read instead. That matters more here than usual: a path decode that was
// missed reads a widened node through the old 0.125 and silently produces a position four times
// too small, which puts cars in the sea rather than crashing.
#include <plugin.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "PathScale.h"
#include "PathScaleSites.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

namespace {

// ThePaths and the per-area arrays inside it. Every one is 72 entries - 64 map areas then 8
// interiors - so an index in 0..63 is a map area and the loader is only ever given those.
constexpr uintptr_t kThePaths = 0x96F050;
constexpr uintptr_t kPathNodes = kThePaths + 0x804;        // CPathNode* per area
constexpr uintptr_t kCarPathLinks = kThePaths + 0x924;     // CCarPathLink* per area
constexpr uintptr_t kNumNodes = kThePaths + 0xFA4;
constexpr uintptr_t kNumCarPathLinks = kThePaths + 0x1304;
constexpr int kMapAreas = 64;
constexpr unsigned kNodeStride = 0x1C;      // sizeof(CPathNode)
constexpr unsigned kLinkStride = 0x0E;      // sizeof(CCarPathLink)
constexpr unsigned kNodeCoord[] = { 8, 0x0A, 0x0C };
constexpr unsigned kLinkCoord[] = { 0, 2 };

// CPathFind::LoadPathFindData(void* fileOrBuffer, int area), __thiscall on ThePaths, ret 8. Its
// entry at 0x4529F0 is a HOODLUM jump into the relocated body, which is why the two CALL SITES are
// retargeted rather than the function, and why calling 0x4529F0 from the hook still works.
constexpr uintptr_t kLoadPathFindData = 0x4529F0;
constexpr uintptr_t kLoadCallSites[] = {
    0x40CA2B,   // CStreaming::ConvertBufferToObject - the streamed load, the one that runs in play
    0x452F75,   // CPathFind::LoadPathFindData(int area) - the synchronous wrapper
};

// A node count this far out of range means the arrays are not what this thinks they are, so the
// area is left alone rather than walked. San Andreas' largest area holds a few thousand.
constexpr int kSaneNodeCount = 200000;

// Vanilla: 8 int16 units per world unit, so 32767/8 = +/-4095.875 in steps of 0.125.
constexpr float kVanillaUnitsPerMetre = 8.0f;
constexpr float kInt16Reach = 32767.0f;

float gUnitsPerMetre = kVanillaUnitsPerMetre;
float gDecode = 1.0f / kVanillaUnitsPerMetre;   // what the decode sites will read
float gEncode = kVanillaUnitsPerMetre;          // what the encode sites will read
bool gActive = false;

typedef void(__thiscall* LoadPathFindDataFn)(void* self, void* file, int area);

// Powers of two only. A scale that is not one turns the rescale below into a multiply that cannot
// round-trip, and the precision it buys between two powers is not worth the arithmetic.
float ScaleForReach(float wanted) {
    for (float scale = kVanillaUnitsPerMetre; scale >= 1.0f; scale *= 0.5f)
        if (kInt16Reach / scale >= wanted) return scale;
    return 1.0f;   // +/-32767, and nothing this project places is beyond that
}

// How far from the origin a path node has to be able to reach for the configured world.
//
// Derived from the region set rather than from a number written here, and specifically NOT from
// the order regions load in: RegionExtent is a min and a max over every region, so adding one,
// removing one or loading them in another order gives the same answer for the same set of maps.
// Packing moves regions about but does not change the extent of the whole, so a packed world and
// an unpacked one of the same maps agree.
float RequiredReach() {
    float minX, maxX, minY, maxY;
    Settings::RegionExtent(minX, maxX, minY, maxY);
    float reach = 0.0f;
    for (float v : { -minX, maxX, -minY, maxY }) reach = std::fmax(reach, v);
    // Height as well. San Andreas' own map runs to about 1200 at the top of Mount Chiliad and a
    // region adds its WorldOriginZ to that, so this is never the binding constraint - but it is
    // free to check and it would be a silly way to lose.
    for (const MapRegion& r : Settings::regions) reach = std::fmax(reach, std::fabs(r.originZ) + 2000.0f);
    return reach;
}

// Rescale one area's shipped nodes from the file's 0.125 to whatever is in force now.
//
// The record in nodes<N>.dat IS the in-memory CPathNode - the loader freads it verbatim and the
// only thing it does afterwards is a bit fixup on two flag bytes - so the shorts that arrive are
// world * 8 and have to become world * gUnitsPerMetre. That is a halving at scale 4, and the
// rounding is done properly rather than by shifting, because a node half a unit off its kerb is
// the difference between a car driving down a road and along it.
void RescaleArea(int area) {
    if (area < 0 || area >= kMapAreas) return;
    const float factor = gUnitsPerMetre / kVanillaUnitsPerMetre;

    uint8_t* nodes = *(uint8_t**)(kPathNodes + (uintptr_t)area * 4);
    int nodeCount = *(int*)(kNumNodes + (uintptr_t)area * 4);
    if (nodes && nodeCount > 0 && nodeCount < kSaneNodeCount) {
        for (int i = 0; i < nodeCount; i++) {
            uint8_t* node = nodes + (uintptr_t)i * kNodeStride;
            for (unsigned at : kNodeCoord) {
                int16_t* v = (int16_t*)(node + at);
                *v = (int16_t)lrintf((float)*v * factor);
            }
        }
    }

    uint8_t* links = *(uint8_t**)(kCarPathLinks + (uintptr_t)area * 4);
    int linkCount = *(int*)(kNumCarPathLinks + (uintptr_t)area * 4);
    if (links && linkCount > 0 && linkCount < kSaneNodeCount) {
        for (int i = 0; i < linkCount; i++) {
            uint8_t* link = links + (uintptr_t)i * kLinkStride;
            for (unsigned at : kLinkCoord) {
                int16_t* v = (int16_t*)(link + at);
                *v = (int16_t)lrintf((float)*v * factor);
            }
        }
    }
    Log::Debug("paths: area %d rescaled, %d node(s) and %d link(s)", area, nodeCount, linkCount);
}

void __fastcall LoadHook(void* self, void* /*edx*/, void* file, int area) {
    reinterpret_cast<LoadPathFindDataFn>(kLoadPathFindData)(self, file, area);
    // The one way an area could be rescaled twice is a call that read nothing and left the
    // previous arrays in place - so that is the case to refuse, and it is the only one. Every
    // other call has just freads the records out of the file, so what is in the arrays afterwards
    // is always vanilla-scaled and always wants exactly one pass.
    //
    // Remembering which areas have been done would be the obvious guard and would be wrong:
    // unloading an area frees its arrays and reloading it asks for the same size again, so the
    // allocator very plausibly hands back the same address with the same node count, and a guard
    // keyed on that would skip a rescale that was genuinely needed. An area left at the vanilla
    // scale while everything around it is widened would put that whole square of road at twice its
    // real size, which is a far worse failure than the one the guard was for.
    if (!file) {
        Log::Write("paths: area %d was asked for with nothing to read from; its nodes are left as "
                   "they are", area);
        return;
    }
    RescaleArea(area);
}

// Does every site still read as the instruction the generator recorded? Nothing is written until
// all of them do.
bool VerifySite(uintptr_t address, uint32_t expected) {
    uint8_t here[6];
    memcpy(here, (const void*)address, sizeof(here));
    return here[0] == PathScale::kInstructionBytes[0] &&
           here[1] == PathScale::kInstructionBytes[1] &&
           memcmp(here + PathScale::kDispOffset, &expected, 4) == 0;
}

template <size_t N>
bool VerifyAll(const uint32_t (&sites)[N], uint32_t expected, const char* what) {
    for (uint32_t site : sites) {
        if (VerifySite(site, expected)) continue;
        uint8_t here[6] = {};
        memcpy(here, (const void*)(uintptr_t)site, sizeof(here));
        Log::Write("paths: the %s site at 0x%X reads %02X %02X %02X %02X %02X %02X, not an fmul of "
                   "0x%X; nothing is patched and the path scale stays vanilla",
                   what, site, here[0], here[1], here[2], here[3], here[4], here[5], expected);
        return false;
    }
    return true;
}

template <size_t N>
int PatchAll(const uint32_t (&sites)[N], uint32_t expected, const float* target) {
    int done = 0;
    for (uint32_t site : sites)
        if (MemPatch::RetargetDisp32(site, PathScale::kDispOffset, expected, target)) done++;
    return done;
}

}   // namespace

namespace PathScale {

bool Active() { return gActive; }
float UnitsPerMetre() { return gUnitsPerMetre; }
float Reach() { return kInt16Reach / gUnitsPerMetre; }

void Install() {
    if (!Settings::world.widenPathNodes) return;
    if (Settings::regions.empty()) return;
    // Widening the scale is a means, not an end: on its own it changes how San Andreas reads its
    // own path data and gives nothing back, because a region's roads are not in CPathFind until
    // RegionPaths puts them there. Shipping the risk without the payoff would be strictly worse
    // than vanilla, so this engages only when there is something to use it for.
    if (!Settings::world.regionPaths) {
        Log::Debug("paths: RegionPaths is off, so the node scale is left vanilla - widening it "
                   "alone would change San Andreas' own paths and buy nothing");
        return;
    }
    bool anyRoads = false;
    for (const MapRegion& r : Settings::regions) anyRoads |= !r.carNodeFile.empty();
    if (!anyRoads) {
        Log::Write("paths: no region supplies a CarNodes file, so there are no roads to widen the "
                   "node scale for; it is left vanilla");
        return;
    }
    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the path node scale is left vanilla, the server owns traffic");
        return;
    }

    const float wanted = RequiredReach();
    float scale = Settings::world.pathNodeScale > 0.0f ? Settings::world.pathNodeScale
                                                       : ScaleForReach(wanted);
    if (scale >= kVanillaUnitsPerMetre) {
        Log::Write("paths: the configured world reaches %.0f units, which vanilla's +/-%.0f "
                   "already covers; the path scale is left alone",
                   wanted, kInt16Reach / kVanillaUnitsPerMetre);
        return;
    }
    if (kInt16Reach / scale < wanted)
        Log::Write("paths: WARNING - PathNodeScale %.3g reaches only +/-%.0f and the world wants "
                   "%.0f. Nodes past that will wrap round to the other side of the map",
                   scale, kInt16Reach / scale, wanted);

    // Verify everything first, patch nothing until it all passes.
    if (!VerifyAll(kNodeDecodeSites, kVanillaDecode, "path node decode") ||
        !VerifyAll(kLinkDecodeSites, kVanillaDecode, "car path link decode") ||
        !VerifyAll(kEncodeSites, kVanillaEncode, "path node encode"))
        return;

    gUnitsPerMetre = scale;
    gDecode = 1.0f / scale;
    gEncode = scale;

    int patched = PatchAll(kNodeDecodeSites, kVanillaDecode, &gDecode)
                + PatchAll(kLinkDecodeSites, kVanillaDecode, &gDecode)
                + PatchAll(kEncodeSites, kVanillaEncode, &gEncode);
    const int total = (int)(sizeof(kNodeDecodeSites) + sizeof(kLinkDecodeSites) +
                            sizeof(kEncodeSites)) / (int)sizeof(uint32_t);
    if (patched != total) {
        // Verification passed and a write still failed, which means the page could not be made
        // writable. Half a patch is worse than none, so say so loudly; there is no going back from
        // here without keeping every original byte, and this has never happened.
        Log::Write("paths: ERROR - only %d of %d sites could be written. The path scale is now "
                   "inconsistent; turn WidenPathNodes off", patched, total);
        return;
    }

    // San Andreas' own nodes are written for 0.125, so they are rescaled as each area arrives.
    int hooked = 0;
    for (uintptr_t site : kLoadCallSites)
        if (MemPatch::RetargetCall(site, kLoadPathFindData, reinterpret_cast<void*>(&LoadHook)))
            hooked++;
    if (hooked != (int)(sizeof(kLoadCallSites) / sizeof(uintptr_t))) {
        Log::Write("paths: ERROR - %d of %u call sites of CPathFind::LoadPathFindData could be "
                   "hooked. San Andreas' own paths will be read at the wrong scale; turn "
                   "WidenPathNodes off",
                   hooked, (unsigned)(sizeof(kLoadCallSites) / sizeof(uintptr_t)));
        return;
    }

    gActive = true;
    Log::Write("paths: node scale %.4g int16 per unit (reach +/-%.0f, step %.4g), chosen for a "
               "world wanting %.0f; %d site(s) repointed, %d loader call site(s) hooked",
               scale, kInt16Reach / scale, 1.0f / scale, wanted, patched, hooked);
}

}   // namespace PathScale
