#pragma once
#include <string>

// Dying or being arrested inside an imported city wakes the player in that city, at one of its own
// hospitals or police stations, instead of throwing them thousands of units back to San Andreas.
//
// Nothing of a region's is ever put into San Andreas' own restart arrays - see the file for why.
namespace RegionRestarts {
    void Install();
    // One line for the F8 read-out: where the player would wake if they died, and if they were
    // arrested, standing where they are now.
    void Describe(std::string& out);
}
