// How far the streamer asks for scenery, in world units rather than in sectors.
//
// CStreaming::AddModelsToRequestList (0x40D3F0) is the near-field pass: every frame it asks the
// streamer for the models standing round the camera. Disassembled, it is
//
//     r = 80.0f                                  ; 40.0f on the near call
//     R = (int)(r * FINE_SCALE)                  ; 0x40D444 - a DISTANCE, in sectors
//     box = pos +/- r                            ; world units
//     for every sector the box touches:          ; 0x40D451..0x40D57E - POSITIONS, in sectors
//         d2 = (sx - cx)^2 + (sy - cy)^2         ; in cells
//         if d2 <= (R - 1)^2:  request every entity in the sector, no distance test at all
//         elif d2 <= (R + 2)^2: request the entities inside the box and within
//                               min(r, drawDistance * lodScale) of the camera
//
// which is two different uses of the sector scale in one function. The four position conversions
// need the scale AND the centre, and SectorGridSites.h repoints both halves of all of them. The
// one at 0x40D444 converts a distance: it needs the scale and must not have the centre, and the
// site scan - which recognises a conversion by an fmul of the scale followed by an fadd of the
// centre - therefore never saw it. It was left reading San Andreas' pooled 0.02.
//
// On a vanilla 50-unit grid that is right by definition: R = (int)(80 * 0.02) = 1, the inner ring
// is d2 <= 0, and the only sector requested unconditionally is the one the player is standing in.
// Everything else within 80 units is requested with its own draw distance respected. Eighty units
// is what San Andreas means by "near field".
//
// Once WorldSectors stretches the cell, R stays 1 - it is still computed from vanilla's 0.02 - but
// the sector it names is no longer 50 units across. The unconditional ring is now the whole of a
// 121-unit cell, or an 87-unit one, or whatever the world span divided by 120 came to that run.
// So the ground the streamer covers without asking any distance question tracks the cell size, and
// every improvement to the grid packing - 128 units when Liberty City was off the west coast, 121
// with it north, 87 when FitSectorsToContent measured real extents - quietly shortened it. Nobody
// changed a streaming setting; the streaming distance was a side effect of the grid all along.
//
// Note which way that runs. The bug made this plugin stream FURTHER than San Andreas ever intended,
// and the "regression" was the accident being withdrawn as the grid got better. A strictly correct
// fix - pin the radius to the 80 units the game means - would land shorter than any build of this
// plugin has ever shipped, and read as a much worse regression than the one being fixed. So the
// distance becomes a number the ini owns, defaulting to something near what the stretched grid was
// giving by accident, and the mechanism is fixed so that it stays that number whatever the cell
// does next. That matters most for MapWindow, which fits 120 cells to one map for roughly vanilla
// cells: on the old behaviour switching it on would have cut the near field to its shortest ever.
//
// Four sites, all inside that one function, all byte-verified before anything is written:
//
//   0x40D404  imm32 80.0f   the far radius        -> the configured distance
//   0x40D40E  imm32 40.0f   the near radius       -> half of it, keeping vanilla's 2:1
//   0x40D446  disp32 0.02   the radius conversion -> our own cells-per-world-unit
//   0x40D63C  dec edx; imul edx, edx              -> xor edx, edx (two nops)
//
// The last one is what keeps the fix cheap. Raising r raises R with it, and (R - 1)^2 is the
// unconditional ring: at 200 units over 87-unit cells that would request every entity within two
// cells with no distance test and no draw-distance test, which is a great deal of streaming for
// models that will never be drawn - exactly the per-frame cost the sector-grid work exists to
// reduce. Zeroing the term pins the unconditional ring back to the player's own cell, which is
// precisely what vanilla does at its own 80-over-50, and leaves the extra distance to the outer
// ring, where every entity is still tested against the box and against its own draw distance.
//
// With the inner ring pinned, an over-large R costs nothing at all - the loop only ever walks the
// sectors the box touches, and R can only reject sectors inside it. So the conversion factor is
// set deliberately generous: R = ceil(distance / cell) + 3, which makes the outer ring reach
// distance + 3.5 cells and guarantees it can never reject a sector that could hold an entity
// inside the box. Erring the other way is what silently clips streaming.
//
// ------------------------------------------------------------------- paying for the wider box
//
// Widening the box is not free and the arithmetic is unforgiving: the pass visits every entity the
// box touches, so its cost goes as the square of the distance. Against the 80 units San Andreas
// asks for, 140 units is three times the ground and 200 is six and a quarter, every frame.
//
// Worth being exact about what the old, broken behaviour was doing, because it explains why simply
// asking for the same distance costs more than it used to. The box was always 80 units; the extra
// reach came from requesting the whole of the player's own cell without a distance test, and that
// cell was already the middle of the box. The reach was free because nothing extra was walked for
// it. A box that reaches the same distance in every direction has to walk the ground it reaches.
//
// So the distance is refreshed on a step rather than on a frame. After a pass at P with radius W,
// everything within W of P has been requested; while the player is still within S of P, everything
// within W - S of them is covered, whatever they do in between. Run the wide pass only when they
// have moved S since the last one, ask for W = distance + S, and the guarantee is the full distance
// on every frame while the pass itself runs about one frame in two hundred. Every other frame gets
// San Andreas' own 80, which is what the game shipped and what the near field actually needs.
//
// The amortised cost is (W^2 * step per frame) / S, which is smallest at S near the distance and
// flat for a good way either side; half the distance is the middle of that range and keeps the one
// wide frame from being much larger than it needs to be. At 140 units and 20 units a second that is
// a wide pass every three and a half seconds, and about 1.03 times the near-field cost of the
// broken build - for a uniform 140 units where it gave 121 in one direction and 80 in another.
//
// The failure mode is deliberately the safe one. If the player is somewhere the position cannot be
// read, or the camera has run away from them, the radius in force is vanilla's 80: the game's own
// behaviour, never worse than it.
#include <plugin.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "StreamRange.h"
#include "WorldSectors.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// What the ini asks for when it says nothing. The broken build reached the player's own cell in
// some directions and 80 units in others - 121 units on the current two-city fit, 128 at its most
// generous ever - so this is a little more than the best it ever gave, uniformly and in every
// direction, and it costs 3.1 times San Andreas' ground rather than the 6.25 that 200 costs.
const float kAutoDistance = 140.0f;

// Vanilla's own radii, which every frame that is not a refresh gets. These are what San Andreas
// shipped and what the near field is actually for; the distance above is carried by the refresh.
const float kVanillaFar = 80.0f;
const float kVanillaNear = 40.0f;

// Below vanilla's own 80 there is nothing to gain and a lot to lose, and past this the per-frame
// sector walk grows as the square of it for scenery that the entities' own draw distances will
// refuse to render anyway.
const float kMinDistance = 80.0f;
const float kMaxDistance = 1000.0f;

struct Immediate {
    uintptr_t address;
    uint32_t  oldValue;
};

// `mov dword ptr [esp+0x10], imm32` twice: 80.0f then 40.0f.
const Immediate kFarRadius  = { 0x40D404, 0x42A00000 };
const Immediate kNearRadius = { 0x40D40E, 0x42200000 };
// `fmul dword ptr [0x858B38]` - the radius, in world units, becoming a count of sectors.
const uintptr_t kRadiusConversion = 0x40D444;
const uint32_t  kVanillaFineScale = 0x00858B38;
// `dec edx; imul edx, edx` - (R - 1)^2, the unconditional ring.
const uintptr_t kInnerRing = 0x40D63C;
const uint8_t kInnerRingWas[4]  = { 0x4A, 0x0F, 0xAF, 0xD2 };  // dec edx; imul edx, edx
const uint8_t kInnerRingNow[4]  = { 0x33, 0xD2, 0x90, 0x90 };  // xor edx, edx; nop; nop

// Read by the game once 0x40D446 points at it, so it has to outlive Install().
float gRadiusToSectors = 0.02f;

float gDistance = 0.0f;   // world units guaranteed covered, 0 when nothing was patched
float gStep = 0.0f;       // world units the player moves before the wide pass runs again; 0 = every frame
float gWide = 0.0f;       // gDistance + gStep, the radius a refresh frame asks for
bool  gPatched = false;
bool  gWideNow = false;   // which pair of radii the two immediates currently hold
bool  gHaveLast = false;
float gLastX = 0.0f, gLastY = 0.0f, gLastZ = 0.0f;   // where the last refresh ran
unsigned gRefreshes = 0;

uint32_t AsBits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// The cell the sector grid is currently using, in world units.
float CellSize() {
    if (!WorldSectors::Owns()) return 50.0f;   // San Andreas' own
    float lo = 0.0f, hi = 0.0f;
    WorldSectors::CurrentFit(lo, hi);
    const float cell = (hi - lo) / 120.0f;
    return cell > 1.0f ? cell : 50.0f;
}

// R has to be at least the radius in cells, and is deliberately three cells more: it is only ever
// used to reject sectors the loop has already reached, so too large is free and too small clips
// the near field without a word. The +0.5 is so the game's truncating float-to-int lands on the
// number meant rather than one below it.
//
// One factor serves all four radii the function can be given - the wide pair and vanilla's 80/40 -
// because it is a scale rather than a count, and the margin is generous enough that the smaller
// ones stay safe: a radius r gets R = trunc(r * f) >= r/cell + 3.5*r/W - 1, and the ring reaches
// (R + 2 - sqrt2) cells, so it covers r as long as r >= 0.118 * W. At W = 210 that is 25 units and
// the smallest radius in play is 40.
void SetFactor() {
    const float cell = CellSize();
    float ring = std::ceil(gWide / cell) + 3.0f;
    if (ring < 4.0f) ring = 4.0f;
    gRadiusToSectors = (ring + 0.5f) / gWide;
}

// The two `mov dword ptr [esp+0x10], imm32` that choose the radius. Written between frames, never
// during one - this runs from gameProcessEvent, and the streaming pass reads them afterwards.
void SetRadii(bool wide) {
    if (gWideNow == wide) return;
    // Not named `far`/`near`: windows.h still defines both as empty macros.
    const float farRadius = wide ? gWide : kVanillaFar;
    patch::SetUInt(kFarRadius.address, AsBits(farRadius));
    patch::SetUInt(kNearRadius.address, AsBits(wide ? gWide * 0.5f : kVanillaNear));
    gWideNow = wide;
    if (wide) {
        gRefreshes++;
        Log::Debug("stream range: refresh %u at %.0f units, radius %.0f", gRefreshes, gStep, gWide);
    }
}

// Refresh when the player has moved the step since the last one, and on the first frame, and after
// anything that moves them a long way - a teleport, an interior, a new game - because that is just
// a large step. Nothing here reads the camera: if it has run away from the player the radius in
// force is vanilla's own 80, which is what the game would have done anyway.
void OnFrame() {
    if (!gPatched || gStep <= 0.0f) return;
    CPlayerPed* player = FindPlayerPed();
    if (!player) { SetRadii(false); return; }
    const CVector p = player->GetPosition();
    if (gHaveLast) {
        const float dx = p.x - gLastX, dy = p.y - gLastY, dz = p.z - gLastZ;
        if (dx * dx + dy * dy + dz * dz < gStep * gStep) { SetRadii(false); return; }
    }
    gLastX = p.x; gLastY = p.y; gLastZ = p.z;
    gHaveLast = true;
    SetRadii(true);
}

}  // namespace

namespace StreamRange {

float Distance() { return gDistance; }

void GridChanged() {
    if (!gPatched) return;
    const float before = gRadiusToSectors;
    SetFactor();
    if (before != gRadiusToSectors)
        Log::Debug("stream range: cell is now %.0f units, radius factor %.6f (%d sectors for %.0f "
                   "units)", CellSize(), gRadiusToSectors,
                   (int)(gDistance * gRadiusToSectors), gDistance);
}

void Install() {
    // Under SA-MP the server owns the objects and does its own distance work; nothing here.
    if (Settings::RunningUnderSaMp()) return;

    float want = Settings::world.streamDistance;
    if (want == 0.0f) return;                       // asked to leave San Andreas alone
    if (want < 0.0f) {                              // auto
        // The compensation only exists because our grid stretched the cell. If the grid is still
        // San Andreas' own - no region out past +/-3000, or another mod owning it - then 80 units
        // over 50-unit cells is what the game was built for and there is nothing to put right.
        if (!WorldSectors::Owns()) return;
        want = kAutoDistance;
    }
    if (want < kMinDistance) want = kMinDistance;
    if (want > kMaxDistance) want = kMaxDistance;

    // Check every site before writing any of them: half of this applied is a streaming radius and
    // a rejection ring that disagree, which drops geometry rather than merely misplacing it.
    if (patch::GetUInt(kFarRadius.address) != kFarRadius.oldValue ||
        patch::GetUInt(kNearRadius.address) != kNearRadius.oldValue ||
        patch::GetUInt(kRadiusConversion + 2) != kVanillaFineScale) {
        Log::Write("ERROR: streaming distance left at San Andreas' own: "
                   "CStreaming::AddModelsToRequestList does not hold the values this build of "
                   "gta_sa.exe should have (0x40D404=%08X 0x40D40E=%08X 0x40D446=%08X); another "
                   "mod may have changed it",
                   patch::GetUInt(kFarRadius.address), patch::GetUInt(kNearRadius.address),
                   patch::GetUInt(kRadiusConversion + 2));
        return;
    }
    uint8_t ring[4] = {};
    patch::GetRaw(kInnerRing, ring, sizeof(ring));
    if (memcmp(ring, kInnerRingWas, sizeof(ring)) != 0) {
        Log::Write("ERROR: streaming distance left at San Andreas' own: the unconditional ring at "
                   "0x40D63C reads %02X %02X %02X %02X, not the expected %02X %02X %02X %02X",
                   ring[0], ring[1], ring[2], ring[3],
                   kInnerRingWas[0], kInnerRingWas[1], kInnerRingWas[2], kInnerRingWas[3]);
        return;
    }

    gDistance = want;
    // How far the player moves before the wide pass is run again. Negative asks for the default,
    // half the distance, which is near the flat bottom of the amortised-cost curve; 0 turns the
    // refresh off and pays the wide box on every frame, which is what to set while diagnosing
    // whether the scan or the streamer is the thing costing frames.
    gStep = Settings::world.streamRefreshStep;
    if (gStep < 0.0f) gStep = want * 0.5f;
    gWide = want + gStep;
    SetFactor();

    const float cell = CellSize();
    const uint32_t farBits = AsBits(gWide);
    const uint32_t nearBits = AsBits(gWide * 0.5f);
    gWideNow = true;   // Install writes the wide pair, so the first frames cover the full distance
    MemPatch::ReplaceBytes(kFarRadius.address, (const uint8_t*)&kFarRadius.oldValue,
                           (const uint8_t*)&farBits, sizeof(farBits));
    MemPatch::ReplaceBytes(kNearRadius.address, (const uint8_t*)&kNearRadius.oldValue,
                           (const uint8_t*)&nearBits, sizeof(nearBits));
    MemPatch::RetargetDisp32(kRadiusConversion, 2, kVanillaFineScale, &gRadiusToSectors);
    MemPatch::ReplaceBytes(kInnerRing, kInnerRingWas, kInnerRingNow, sizeof(kInnerRingWas));
    gPatched = true;

    if (gStep > 0.0f) {
        Events::gameProcessEvent += OnFrame;
        Log::Write("streaming distance pinned to %.0f world units, refreshed at %.0f units of "
                   "travel with a %.0f-unit pass and San Andreas' own 80 in between; the full "
                   "%.0f is covered on every frame either way. This build was asking for 80 plus "
                   "the whole of the player's %.0f-unit cell, which moved every time the grid was "
                   "re-fitted", want, gStep, gWide, want, cell);
        Log::Write("note: the wide pass costs %.1f times the ground San Andreas walks, and running "
                   "it on a step rather than on a frame is what makes that affordable - about one "
                   "frame in two hundred at walking pace. [World] StreamRefreshStep = 0 turns the "
                   "step off and pays it every frame, which is the way to tell whether it is the "
                   "scan or the streamer costing frames",
                   (gWide / 80.0f) * (gWide / 80.0f));
    } else {
        Log::Write("streaming distance pinned to %.0f world units (%.0f on the near pass), on "
                   "every frame - StreamRefreshStep is 0. That walks %.1f times the ground San "
                   "Andreas does, every frame; this build was asking for 80 plus the whole of the "
                   "player's %.0f-unit cell, which moved every time the grid was re-fitted",
                   want, want * 0.5f, (want / 80.0f) * (want / 80.0f), cell);
    }
    Log::Write("note: every entity the box finds is still tested against the box and against its "
               "own draw distance, so what is requested grows by much less than the ground does. "
               "Lower [World] StreamDistance if the frame rate matters more than the pop-in; 80 is "
               "vanilla, 0 leaves the function alone");
    if (cell > want)
        Log::Write("note: the sector cell (%.0f units) is larger than the streaming distance, so "
                   "the entities in the player's own cell are still requested from further out "
                   "than that; a smaller world span or a larger StreamDistance closes the gap",
                   cell);
}

}  // namespace StreamRange
