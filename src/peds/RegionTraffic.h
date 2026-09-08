#pragma once
#include <plugin.h>

// Traffic in an imported city, driven along the source game's own carriageways.
namespace RegionTraffic {
    void Install();

    // Once a frame, from the region pedestrian tick, with the player's position. Traffic rides on
    // the same graph machinery and is driven from the same place, so it never runs without it.
    void Tick(const CVector& playerPosition);

    // Let go of every vehicle being tracked - between games, when the vehicle pool and its handle
    // re-use counters are about to be reset under us.
    void Forget();

    int Count();
}
