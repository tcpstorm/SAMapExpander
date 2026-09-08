#pragma once

namespace StreamRange {
    // Pins CStreaming::AddModelsToRequestList' near-field request radius to a fixed number of
    // WORLD units, so it stops moving when WorldSectors changes the cell size. Install after
    // WorldSectors::Install, which is where the cell is decided.
    void Install();

    // The sector grid has been re-fitted (MapWindow crossing a map boundary): the cell size has
    // changed, so the world-units-to-sectors factor this hands the function has to be recomputed.
    // Cheap - one float - and safe to call when nothing was patched.
    void GridChanged();

    // The radius now in force, in world units, or 0 if the function was left as San Andreas
    // shipped it.
    float Distance();
}
