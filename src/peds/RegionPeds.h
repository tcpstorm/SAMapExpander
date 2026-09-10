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

    // A donor pedestrian created at `at` to drive a traffic car, chosen from the district the
    // nearest pavement node belongs to; null when no district model is loaded yet or there is no
    // ped graph here. The traffic module seats it with CCarEnterExit::SetPedInCarDirect and owns
    // its life through the car; RegionPeds does not track it.
    CPed* MakeDriverPed(const CVector& at);

    // How many peds this module currently has walking, for the log and the F8 read-out.
    int Count();

    // Whether the ped pool has room. CPopulation::AddPed cannot report a full pool - when the
    // allocation fails it carries on with a null pointer and faults - so everything that calls it
    // has to ask first. The police module calls it too.
    bool SafeToAddPed();

    // ---- the debug overlay (drawn by src/debug/PathDraw.cpp). Same rule as the traffic's: drawn
    // from this module's own NodeWorld(), because pednodes.dat is a different file with a
    // different derivation and the only useful comparison is between two graphs that each drew
    // themselves. `eye` is the camera; culled to `radius`.
    void DrawGraph(const CVector& eye, float radius);
}
