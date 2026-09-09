// Fitting several imported maps into as small a square of world as they will go into.
//
// San Andreas files every static entity into a grid of exactly 120 x 120 sectors and this plugin
// cannot change that count - the index that reaches the array is clamped with `cmp reg, imm8` at
// every one of the 136 sites that do it, so no index above 127 can be expressed. All it can do is
// stretch those 120 cells over whatever ground is in use, which makes the cell the world span over
// 120, and every per-frame scan then walks a cell's worth of entities. Measured against San
// Andreas' own 45884 placements, the cell a player is standing in holds 12.2 entities at vanilla's
// 50-unit cell, 27.4 at 83 units and 46.7 at 116. So the span is the whole of the frame cost, and
// the span is decided by where the maps are put.
//
// The old placement walked the regions in order and put each one against whichever of the four
// sides of the ground already in use left the world squarest. That is right for one region and
// wrong for five, because the used box only ever grows: the second map goes beside the first, the
// third beside both, and the span climbs with the number of maps. Five came out at 13936 a side.
//
// A map does not have to go beside the used box - it has to go in a corner of it. San Andreas is
// 6000 square and Vice City is 3436 wide, so the moment Vice City is east the world is 9936 a side
// and an entire 3436 x 9936 column and a 6000 x 3436 strip inside that square are empty. Liberty
// City, Carcer City, Bullworth and a Warriors level all fit in there without the square growing at
// all. So try every corner formed by every box already placed, keep whichever leaves the smallest
// square, and try the maps in every order while that stays cheap. Five maps then come out at 9936
// a side - what one map costs - because the fifth went in ground the first had already paid for.
// The span grows as the square root of the total map area rather than the sum of the widths, which
// is the best a square grid can do.
//
// Kept free of everything - no plugin.h, no logging, no Settings - so that tools/pack_test.cpp can
// build it on the host and check the arithmetic without the game anywhere near it.
#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace RegionPacking {

struct Box {
    float x0, x1, y0, y1;
    float Width()  const { return x1 - x0; }
    float Height() const { return y1 - y0; }
    float Area()   const { return Width() * Height(); }
};

// The range the sector grid would need to cover this box on its own, which is what MapWindow fits
// the 120 cells to when the player is standing on that map.
//
// There is one SCALE and one CENTRE for the whole grid rather than one per axis, so the ground it
// covers is a square [lo, hi] x [lo, hi] and BOTH of a map's projections have to fit in the same
// [lo, hi]. A map is therefore cheap to window when its x range and its y range sit on top of each
// other - out on the diagonal from San Andreas - and dear when it is out along one axis: Vice City
// is 3436 by 3681 but at x 3846..7282, y -914..2767 the range holding both is 8196. Never smaller
// than 6000, because 120 cells finer than vanilla's 50 units buy nothing and cost pointer nodes.
inline float WindowSpan(const Box& b) {
    const float lo = b.x0 < b.y0 ? b.x0 : b.y0;
    const float hi = b.x1 > b.y1 ? b.x1 : b.y1;
    return hi - lo > 6000.0f ? hi - lo : 6000.0f;
}

inline bool Overlap(const Box& a, const Box& b, float gap) {
    return a.x0 < b.x1 + gap && b.x0 < a.x1 + gap &&
           a.y0 < b.y1 + gap && b.y0 < a.y1 + gap;
}

// The side of the smallest square containing all of them. Square, because one pair of constants
// serves both axes of the sector grid, so the world pays for whichever axis is widest twice.
inline float SquareSpan(const std::vector<Box>& boxes) {
    if (boxes.empty()) return 0.0f;
    float x0 = boxes[0].x0, x1 = boxes[0].x1, y0 = boxes[0].y0, y1 = boxes[0].y1;
    for (const Box& b : boxes) {
        if (b.x0 < x0) x0 = b.x0;
        if (b.x1 > x1) x1 = b.x1;
        if (b.y0 < y0) y0 = b.y0;
        if (b.y1 > y1) y1 = b.y1;
    }
    return (x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0);
}

// Place want[order[k]] one at a time, each at whichever corner of the ground already placed leaves
// the smallest square. `placed` comes in holding the fixed ground - San Andreas, and any region
// already inside it - and goes out with one box appended per entry of `order`, in that order.
// False means one of them had nowhere to go, which cannot happen with a whole plane to work in but
// is reported rather than assumed.
inline bool PackOrder(const std::vector<Box>& want, const std::vector<size_t>& order,
                      float gap, std::vector<Box>& placed) {
    for (size_t i : order) {
        const float w = want[i].Width(), h = want[i].Height();
        // The candidates are the positions a corner can be at: flush with an existing edge, or a
        // gap beyond it. Anything between those is either an overlap or wasted ground.
        std::vector<float> xs, ys;
        xs.reserve(placed.size() * 4);
        ys.reserve(placed.size() * 4);
        for (const Box& b : placed) {
            xs.push_back(b.x0);            xs.push_back(b.x1 - w);
            xs.push_back(b.x0 - gap - w);  xs.push_back(b.x1 + gap);
            ys.push_back(b.y0);            ys.push_back(b.y1 - h);
            ys.push_back(b.y0 - gap - h);  ys.push_back(b.y1 + gap);
        }
        bool found = false;
        Box best{};
        float bestSpan = 0.0f, bestWindow = 0.0f, bestAway = 0.0f;
        for (float x0 : xs)
        for (float y0 : ys) {
            Box p{ x0, x0 + w, y0, y0 + h };
            // A hair under the gap, so a placement sitting at exactly the gap is not rejected by
            // its own rounding.
            bool clear = true;
            for (const Box& b : placed)
                if (Overlap(p, b, gap * 0.999f)) { clear = false; break; }
            if (!clear) continue;

            placed.push_back(p);
            const float span = SquareSpan(placed);
            placed.pop_back();
            // Among placements that leave the same square, prefer the one whose own window is
            // narrowest - a corner on the diagonal from San Andreas rather than one out along an
            // axis. It costs the default nothing, because it only ever chooses between placements
            // that were already equal, and it is what makes MapWindow worth having: on five maps
            // it takes the worst window from 10317 units to 8217, which is 86-unit sectors down
            // to 68 while the world stays exactly 10236 a side.
            const float windowSpan = WindowSpan(p);
            // Last, the placement nearest where the author put the map, so a region already
            // sitting where packing would put it anyway is not shuffled, and so the same ini
            // always produces the same world.
            const float dx = p.x0 - want[i].x0, dy = p.y0 - want[i].y0;
            const float away = dx * dx + dy * dy;
            const bool better =
                !found || span < bestSpan - 0.5f ||
                (span < bestSpan + 0.5f &&
                 (windowSpan < bestWindow - 0.5f ||
                  (windowSpan < bestWindow + 0.5f && away < bestAway - 0.5f)));
            if (better) {
                found = true;
                best = p;
                bestSpan = span;
                bestWindow = windowSpan;
                bestAway = away;
            }
        }
        if (!found) return false;
        placed.push_back(best);
    }
    return true;
}

// Every ordering while that is a few hundred of them, largest-area-first beyond that. Returns the
// order that won in `bestOrder` and the boxes - fixed ground first, then one per entry of
// bestOrder - in `best`. False if nothing could be placed at all.
inline bool Pack(const std::vector<Box>& want, const std::vector<size_t>& movable,
                 const std::vector<Box>& fixed, float gap,
                 std::vector<Box>& best, std::vector<size_t>& bestOrder) {
    std::vector<size_t> order = movable;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (want[a].Area() != want[b].Area()) return want[a].Area() > want[b].Area();
        return a < b;   // deterministic when two maps are the same size
    });

    best.clear();
    bestOrder.clear();
    float bestSpan = 0.0f;
    // 5 regions is 120 orderings of a few hundred candidates each - microseconds. 6 would be 720
    // and 8 would be 40320, which is still fast but no longer obviously so, and largest-first is
    // within a whisker of optimal once there are that many boxes to fill the corners with.
    const bool tryAll = movable.size() <= 5;
    do {
        std::vector<Box> placed = fixed;
        if (PackOrder(want, order, gap, placed)) {
            const float span = SquareSpan(placed);
            if (best.empty() || span < bestSpan - 0.5f) {
                best = placed;
                bestOrder = order;
                bestSpan = span;
            }
        }
        if (!tryAll) break;
    } while (std::next_permutation(order.begin(), order.end()));
    return !best.empty();
}

}  // namespace RegionPacking
