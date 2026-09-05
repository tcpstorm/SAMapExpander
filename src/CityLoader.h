#pragma once

namespace CityLoader {
    void Install();
    // True once Install() has decided this process really is going to place a region's own
    // buildings: regions with assets configured, and not a SA-MP session where the server does it.
    bool PlacesOwnEntities();
}
