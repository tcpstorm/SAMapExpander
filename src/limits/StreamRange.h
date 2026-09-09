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

    // The distance guaranteed covered on every frame, in world units, or 0 if the function was
    // left as San Andreas shipped it. The radius the game is actually holding at any instant is
    // either this plus the refresh step or vanilla's 80; the guarantee is the same either way.
    float Distance();
}
