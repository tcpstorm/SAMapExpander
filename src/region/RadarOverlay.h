#pragma once

namespace RadarOverlay {
    void Install();
    // Patches the draw call sites once, after other mods have finished their own patching.
    // Safe to call every frame from any radar hook.
    void EnsureInstalled();
}
