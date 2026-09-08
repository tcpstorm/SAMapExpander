#pragma once

namespace Watchdog {
    // Starts a thread that reports where the game is when a frame stops finishing. Debug only.
    void Install();
    // Called once per frame from the existing frame hook; the watchdog watches this stop moving.
    void Tick();
}
