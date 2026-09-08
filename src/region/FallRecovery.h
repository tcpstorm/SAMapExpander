#pragma once
#include <string>

// Puts a player who has fallen out of an imported region back on that region's own ground, instead
// of letting San Andreas throw them across the world to the nearest vehicle path node.
namespace FallRecovery {
    void Install();
    // One line for the F8 read-out: whether it is installed, and where it would put you.
    void Describe(std::string& out);
}
