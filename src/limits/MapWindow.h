#pragma once

namespace MapWindow {
    // Fits the 120 x 120 sector grid to the one map the player is standing on instead of to every
    // map at once, and re-files every entity in the world when they cross from one to another.
    // The only route to vanilla-sized sectors that needs no new instruction sites at all - see the
    // long note at the top of MapWindow.cpp for what it costs and what it risks. Off unless
    // [World] MapWindow says otherwise, because the re-file cannot be checked without running the
    // game.
    void Install();
}
