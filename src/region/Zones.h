#pragma once

namespace Zones {
    void Install();

    // Logs what the game's own lookups answer for a point: the zone FindSmallestZoneForPosition
    // picks (the one the HUD and the pause map name), its box and translated name, and how many of
    // our zones made it into the array. Does nothing useful before Install().
    void LogPlayerZone(float x, float y, float z);

    // The name the game itself would show for a point: the smallest navigation zone holding it,
    // through the game's own text table, so a region's district and San Andreas' own answer alike.
    // Null where no zone applies. Works whether or not Install() added any zones.
    const char* DisplayNameAt(float x, float y, float z);
}
