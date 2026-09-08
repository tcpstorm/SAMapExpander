#pragma once
#include <plugin.h>

// The cars the source game parked at its own kerbs, put back where it left them.
namespace RegionParkedCars {
    void Install();

    // Once a frame, from the region pedestrian tick, with the player's position. Unlike the
    // traffic this is called whatever region the player is over, and when they are over none at
    // all, because a car of ours has to be tidied away when they drive out of the city as well as
    // when they walk out of the street.
    void Tick(const CVector& playerPosition);

    // Let go of every bay and every car - between games, when the vehicle pool and its handle
    // re-use counters are about to be reset under us. The memory of which bays the player has
    // emptied goes too: it is a session's memory, not a save's.
    void Forget();

    // How many are standing at the kerb, for the log and the F8 read-out.
    int Count();
}
