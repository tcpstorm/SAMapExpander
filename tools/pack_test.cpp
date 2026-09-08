// Offline check of src/core/RegionPacking.h - the placement that decides the sector cell size, and
// so the per-frame cost of every collision and render scan in the game.
//
// The header is deliberately free of plugin.h, Settings and logging so this can build it on any
// host and check the arithmetic without the game anywhere near it. The map footprints below are
// the real ones, measured from the converted IPLs (tools/sector_cost.py prints them), so the spans
// asserted here are the spans the plugin will actually produce.
//
//     docker run --rm -v "<repo>:/work" -w /work gcc:13 \
//         sh -c "g++ -std=c++17 -O1 -I src/core tools/pack_test.cpp -o /tmp/t && /tmp/t"
#include "RegionPacking.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using RegionPacking::Box;

static int failures = 0;

static void Check(bool ok, const std::string& what) {
    printf("%-72s %s\n", what.c_str(), ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

struct Map { const char* name; float w, h; };

// Measured footprints. Vice City and Liberty City are what the shipped conversions occupy; the
// other three are plausible sizes for the maps this is meant to make room for, and only their
// size matters here.
static const Map kMaps[] = {
    { "Vice City",    3436.0f, 3681.0f },
    { "Liberty City", 3316.0f, 3062.0f },
    { "Carcer City",  2400.0f, 2400.0f },
    { "Bullworth",    1600.0f, 1600.0f },
    { "Warriors set", 1400.0f, 1400.0f },
};

// Where the maps sit before packing, which only affects tie-breaking: the shipped ini puts Vice
// City east and Liberty City north, and anything after that goes somewhere arbitrary.
static Box Loose(int i) {
    const float at[5][2] = { { 4000.0f, -900.0f }, { -1600.0f, 4000.0f }, { 9000.0f, 0.0f },
                             { -9000.0f, 0.0f },   { 0.0f, -9000.0f } };
    return Box{ at[i][0], at[i][0] + kMaps[i].w, at[i][1], at[i][1] + kMaps[i].h };
}

static float PackN(int n, float gap, std::vector<Box>& best, std::vector<size_t>& order) {
    std::vector<Box> want;
    std::vector<size_t> movable;
    for (int i = 0; i < n; i++) { want.push_back(Loose(i)); movable.push_back((size_t)i); }
    std::vector<Box> fixed{ Box{ -3000.0f, 3000.0f, -3000.0f, 3000.0f } };
    if (!RegionPacking::Pack(want, movable, fixed, gap, best, order)) return -1.0f;
    return RegionPacking::SquareSpan(best);
}

static bool AnyOverlap(const std::vector<Box>& boxes, float gap) {
    for (size_t i = 0; i < boxes.size(); i++)
        for (size_t j = i + 1; j < boxes.size(); j++)
            if (RegionPacking::Overlap(boxes[i], boxes[j], gap * 0.9f)) return true;
    return false;
}

int main() {
    const float gap = 500.0f;

    printf("San Andreas is 6000 square; the grid is 120 cells whatever the world spans.\n\n");
    printf("%-16s %8s %8s %10s %8s\n", "maps", "span", "cell", "cell area", "overlaps");

    float previous = 0.0f;
    for (int n = 1; n <= 5; n++) {
        std::vector<Box> best;
        std::vector<size_t> order;
        const float span = PackN(n, gap, best, order);
        const float cell = span / 120.0f;
        printf("%-16d %8.0f %8.1f %9.2fx %8s\n", n, span, cell, (cell * cell) / (50.0f * 50.0f),
               AnyOverlap(best, gap) ? "YES" : "none");

        Check(span > 0.0f, "packing " + std::to_string(n) + " map(s) finds a placement");
        Check(!AnyOverlap(best, gap),
              "packing " + std::to_string(n) + " map(s) leaves RegionGap between every pair");
        Check(best.size() == (size_t)n + 1,
              "packing " + std::to_string(n) + " map(s) places every one of them");
        // The whole point: adding a map must not widen the world, because the maps after the first
        // go in ground the first one already paid for.
        if (n > 1)
            Check(span <= previous + 0.5f,
                  "the " + std::to_string(n) + "th map does not widen the world");
        previous = span;
    }

    // The measured number, so a change of algorithm that quietly gives up ground is noticed. San
    // Andreas 6000 + RegionGap 500 + Vice City 3436 = 9936.
    {
        std::vector<Box> best;
        std::vector<size_t> order;
        Check(std::fabs(PackN(5, gap, best, order) - 9936.0f) < 1.0f,
              "five maps pack into 9936 units a side (82.8-unit sectors)");
    }

    // The old four-sides placement put five maps in 13936 units - 116-unit sectors - so this is
    // what the change is worth. Both numbers are in docs/known-limitations.md.
    {
        std::vector<Box> best;
        std::vector<size_t> order;
        const float span = PackN(5, gap, best, order);
        Check(span < 13936.0f * 0.8f, "five maps beat the old four-sides placement by over a fifth");
    }

    // Determinism, because a savegame made under packing has to still find its pickups next run.
    {
        std::vector<Box> a, b;
        std::vector<size_t> oa, ob;
        PackN(5, gap, a, oa);
        PackN(5, gap, b, ob);
        bool same = a.size() == b.size() && oa == ob;
        for (size_t i = 0; same && i < a.size(); i++)
            same = a[i].x0 == b[i].x0 && a[i].y0 == b[i].y0;
        Check(same, "the same maps always pack to the same world");
    }

    // A region already inside San Andreas is fixed ground, not something to shuffle: it goes into
    // `fixed` before the search and must come out where it went in.
    {
        std::vector<Box> want{ Box{ -500.0f, 500.0f, -500.0f, 500.0f },
                               Box{ 4000.0f, 7436.0f, -900.0f, 2781.0f } };
        std::vector<Box> fixed{ Box{ -3000.0f, 3000.0f, -3000.0f, 3000.0f }, want[0] };
        std::vector<size_t> movable{ 1 }, order;
        std::vector<Box> best;
        RegionPacking::Pack(want, movable, fixed, gap, best, order);
        Check(best.size() == 3 && best[1].x0 == -500.0f && best[1].y0 == -500.0f,
              "a region already inside San Andreas is left exactly where it is");
    }

    // A gap of zero is a legal setting and must not make every candidate overlap itself.
    {
        std::vector<Box> best;
        std::vector<size_t> order;
        const float span = PackN(5, 0.0f, best, order);
        Check(span > 0.0f && !AnyOverlap(best, 0.0f), "RegionGap = 0 packs without overlapping");
        Check(span < 9936.0f, "RegionGap = 0 packs tighter than RegionGap = 500");
    }

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
