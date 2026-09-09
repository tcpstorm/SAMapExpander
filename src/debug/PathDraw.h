#pragma once
#include <plugin.h>

// World-space debug lines for the imported cities' path graphs.
//
// Everything in this project that drives, walks or parks does so on a graph read out of a .dat
// file and offset into San Andreas' world. Every check on that graph so far has been a tally or a
// log line - counts of nodes, distances between a car and its target - and a tally cannot tell a
// graph that is in the wrong place from a graph that is in the right place and badly followed.
// Both read as "the numbers agree". This draws the graph in the world instead, so the two can be
// told apart by looking.
//
// The one rule the drawing follows: every coordinate on screen comes from the SAME accessor the
// spawner and the driver use, inside the module that owns it. Nothing here re-derives a node
// position, because a second derivation that happened to be wrong in the same way would hide the
// very fault this exists to find. That is why the actual enumeration lives in RegionTraffic.cpp,
// RegionPeds.cpp and RegionParkedCars.cpp - next to NodeWorld() and LanePoint() - and only the
// line drawing lives here.
namespace PathDraw {
    void Install();

    // A colour, packed the way CLines wants it. Verified against the game rather than assumed:
    // CLines::RenderLineNoClipping (0x6FF460) writes `(c << 24) | (c >> 8)` into the immediate
    // vertex at +0x18, which on the D3D build is a D3DCOLOR - 0xAARRGGBB - so the argument it
    // rotates from must be 0xRRGGBBAA. CLines::RenderLineWithClipping assembles the same word a
    // byte at a time and agrees.
    constexpr unsigned Rgba(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
        return (r << 24) | (g << 16) | (b << 8) | a;
    }

    // ---- what the graph modules draw with. Only meaningful inside a Draw* call, i.e. from
    // inside the render hook; anywhere else these are silent no-ops rather than a crash, because
    // the immediate-mode buffer is only valid between the camera's begin and end update.

    void Line(const CVector& a, const CVector& b, unsigned colour);
    void Line(const CVector& a, const CVector& b, unsigned colourA, unsigned colourB);

    // A three-axis tick at a point - the marker used for "a node is here", "a car is here", "the
    // target is here". Small enough to read as a point, big enough to find.
    void Cross(const CVector& at, float size, unsigned colour);

    // A flat arrow lying on the ground, pointing along `heading` radians (0 = +y, the game's own
    // convention). Used for a parked bay, which is a point plus a direction.
    void Arrow(const CVector& at, float heading, float length, unsigned colour);

    // True while there is still room in this frame's budget. A module drawing thousands of links
    // should test it in its loop and stop early rather than call Line eight thousand times.
    bool Budget();
}
