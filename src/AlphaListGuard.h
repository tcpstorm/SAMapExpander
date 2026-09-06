#pragma once

namespace AlphaListGuard {
    // Debug only: checks the fading-entity list ahead of each render, logs the first node that is
    // not one, and cuts the list there so the game keeps running. See AlphaListGuard.cpp.
    void Install();
}
