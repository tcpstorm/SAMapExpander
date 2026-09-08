#pragma once
#include <string>

// Puts a player who has fallen out of an imported region back on that region's own ground, instead
// of letting San Andreas throw them across the world to the nearest vehicle path node.
namespace FallRecovery {
    void Install();
    // One line for the F8 read-out: whether it is installed, and where it would put you.
    void Describe(std::string& out);
    // F6, with [World] Debug=1: drop the player straight through the world, so the recovery can be
    // tried on purpose instead of waiting to find a hole. Works in San Andreas too, where it shows
    // the game's own recovery for comparison.
    void DebugDrop();
}
