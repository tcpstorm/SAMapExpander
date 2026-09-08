#pragma once

// The scale a path node's compressed position is read at. Vanilla San Andreas stores one as three
// int16 multiplied by 0.125, which reaches +/-4095.875 and is why an imported city has no traffic.
namespace PathScale {
    void Install();

    // True when the sites were patched and the world is being read at the wider scale.
    bool Active();

    // int16 units per world unit: vanilla 8, and smaller means further but coarser. 4 reaches
    // +/-8191.75 in steps of 0.25.
    float UnitsPerMetre();
    // How far a path node can describe, in world units: 32767 / UnitsPerMetre().
    float Reach();
}
