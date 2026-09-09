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
}
