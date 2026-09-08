#pragma once
#include <plugin.h>

// Pedestrians in an imported city, walked along the source game's own pavement.
namespace RegionPeds {
    void Install();

    // True while the player stands over a region that has a pedestrian graph loaded. The police
    // module asks, because a wanted level inside San Andreas' own map is San Andreas' business.
    bool PlayerOverGraph();

    // A point on the region's pedestrian graph between minDist and maxDist of `at`, preferring one
    // the camera cannot see. False when the graph has nothing there. Police spawning uses this for
    // the same reason pedestrians do: it is the only description of walkable ground a region has.
    bool FindSpawnPoint(const CVector& at, float minDist, float maxDist, bool avoidCamera,
                        CVector& out);

    // Put a ped on the pavement and keep them walking: the ped is added to the same list the
    // pedestrians are managed in, so it is given a new leg of its walk whenever it finishes one,
    // and is forgotten when the game removes it. Returns null if the model is not in memory yet.
    CPed* Spawn(int modelId, const CVector& at, bool wander);

    // How many peds this module currently has walking, for the log and the F8 read-out.
    int Count();
}
