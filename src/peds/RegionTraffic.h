#pragma once
#include <plugin.h>

// Traffic in an imported city, driven along the source game's own carriageways.
namespace RegionTraffic {
    void Install();

    // Once a frame, from the region pedestrian tick, with the player's position. Traffic rides on
    // the same graph machinery and is driven from the same place, so it never runs without it.
    void Tick(const CVector& playerPosition, bool playerHasGround);

    // Let go of every vehicle being tracked - between games, when the vehicle pool and its handle
    // re-use counters are about to be reset under us.
    void Forget();

    int Count();

    // For F8. The budget comes from San Andreas' own popcycle row for the zone the player is in,
    // so it says nothing until somebody is standing somewhere; and the count of placeholder models
    // actually in memory is the one number that separates "no room for a car" from "no model to
    // make one out of", which is how the traffic failed silently once already.
    int Budget();
    int LoadedModelCount(int* outOf);

    // For F8: how far the nearest carriageway node is from a point, and how far that point is from
    // the region's own zero. Those two numbers together are the only way to tell a graph that is
    // SCALED from one that is merely offset - a scale error is small near the origin and grows
    // with distance from it, and a module driving on its own graph cannot see its own scale.
    void DescribeNearestNode(const CVector& at, char* out, unsigned size);

    // ---- the debug overlay (drawn by src/debug/PathDraw.cpp).
    //
    // Implemented in RegionTraffic.cpp rather than in the renderer, and deliberately so: every
    // point drawn comes out of NodeWorld() and LanePoint(), the same two functions the spawner and
    // the driver use. A renderer that worked out node positions for itself could be wrong in the
    // same way twice and agree with itself, which is precisely the failure this overlay exists to
    // break.
    //
    // `eye` is the camera and not the player, so flying up shows a district. Both cull to `radius`
    // and both stop early when PathDraw::Budget() runs out.
    void DrawGraph(const CVector& eye, float radius, bool groundProbes);
    void DrawDrivers(const CVector& eye, float radius);
}
