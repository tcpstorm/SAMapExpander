#pragma once
#include <plugin.h>

// The police answering a wanted level in an imported city.
namespace RegionPolice {
    void Install();
    bool Active();
    // Once a frame, from the region pedestrian tick, with the player's position. Police ride on
    // the pedestrian graph, so they are driven from the same place and never run without it.
    void Tick(const CVector& playerPosition);
    // Let go of every cop being tracked - between games, when the ped pool and its handle re-use
    // counters are about to be reset under us.
    void Forget();
    int Count();
}
