#pragma once

namespace PathLoadProbe {
    // Says what the streamer hands CPathFind when a path area loads, and refuses to let the game
    // walk a node array whose count is not credible. Written for a crash in
    // CPathFind::SwitchRoadsOffInAreaForOneRegion over area 55 with a node count in the millions,
    // where the file on disk says 1695: the log line here tells whether the buffer was ever the
    // path file, and the guard turns the crash into a logged, skipped area.
    void Install();
}
