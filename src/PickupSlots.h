#pragma once
#include <cstdint>

namespace PickupSlots {
    // Relocates CPickups::aPickUps to a longer heap array so a region's pickups fit beside San
    // Andreas' own, which take 551 of its 620. Must run before the game starts up. See the file.
    void Install();

    // How many entries the array has now, 0 when it was left alone.
    int Count();
    // Where it is - the vanilla address when it was left alone, so callers need no special case.
    uintptr_t Array();
}
