#pragma once
#include <plugin.h>

// The police answering a wanted level in an imported city.
namespace RegionPolice {
    void Install();
    bool Active();
    // Once a frame, from the region pedestrian tick, with the player's position. Police ride on
    // the pedestrian graph, so they are driven from the same place and never run without it.
    void Tick(const CVector& playerPosition);
    int Count();
}
