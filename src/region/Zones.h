#pragma once

namespace Zones {
    void Install();

    // Logs what the game's own lookups answer for a point: the zone FindSmallestZoneForPosition
    // picks (the one the HUD and the pause map name), its box and translated name, and how many of
    // our zones made it into the array. Does nothing useful before Install().
    void LogPlayerZone(float x, float y, float z);
}
