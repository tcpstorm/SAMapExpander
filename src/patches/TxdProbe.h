#pragma once

namespace TxdProbe {
    // Debug only: log every texture dictionary the streamer loads, by slot name, with how many
    // textures it came in with. Says whether a region's dictionary is loaded at all, and whether it
    // arrived empty - which is what "textured buildings, white leaves" comes down to.
    void Install();
}
