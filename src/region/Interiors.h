#pragma once
#include <string>

namespace Interiors {
    void Install();

    // What the area switch currently reads: the game's CGame::currArea, the area this module set
    // (0 when it is not driving it) and the name of the volume the player is standing in. For the
    // F8 read-out; safe to call before Install().
    void Describe(std::string& out);
}
