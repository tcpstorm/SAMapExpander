#pragma once

namespace WorldSectors {
    // Widens what San Andreas' static world grids cover, so a region placed beyond +/-3000 is
    // filed in the sector it really sits in. Only worth installing when this plugin is the one
    // placing the region's entities.
    void Install();
}
