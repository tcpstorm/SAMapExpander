// CWaterLevel::RenderWater (0x6EF650) draws water in three passes, all sharing one set of render
// states and one Im3D buffer (vertices 0xC4D958, indices 0xC4B958, counters 0xC4B950/0xC4B954,
// flushed by RenderWater itself at 0x6F0064):
//   1. 0x6EF6E0  the sea bed for every block in m_BlocksToBeRenderedOutsideWorld
//                (X 0xC21560, Y 0xC214D0, count 0xC215EC, int16 block indices, at most 70),
//                via RenderSeaBedSegment 0x6E6870 / RenderDetailedSeaBedSegment 0x6E6A10.
//   2. 0x6EFBD0/0x6EFCD3  the vanilla triangle and quad lists inside the 12x12 grid.
//   3. 0x6EFE73  the endless flat ocean: for every block of that same list whose index is NOT in
//                0..11, one CWaterLevel::RenderWaterRectangle (0x6EC5D0) call at 0x6F004E covering
//                the whole 500x500 block (minX = bx*500-3000 .. minX+500, likewise Y) with the same
//                CRenPar {z = 0, bigWaves = 1, smallWaves = 0, flow = 0} on all four corners.
// That block list is filled per frame by CWaterLevel::BlockHit (0x6E6CA0), which the frustum
// rasteriser in ScanThroughBlocks (0x6E6D10) drives - so it already is "the blocks near the camera",
// with the same distance behaviour and the same 70-block budget vanilla water has.
//
// So the single call at 0x6F004E is exactly the right seam for region water: retargeting it lets us
// drop the flat ocean where a region's water map is authoritative and draw the region's own quads
// clipped to the same block instead, through the same helper. RenderWaterRectangle then applies the
// vanilla treatment - CameraRangeMin/Max splitting into high-detail rectangles, BigPolySize (168)
// subdivision, two texture layers - so the result tessellates and shades like stock water, and the
// buffer it fills is the one RenderWater flushes (an overflow mid-way auto-flushes with the same
// states via RenderBuffer::RenderIfDoesntFit).
//
// Corner order is P1=(minX,Y1) P2=(maxX,Y1) P3=(minX,Y2) P4=(maxX,Y2) - a Z, not a ring: confirmed
// both from RenderWater's quad loop (0x6EFD83 feeds vertex 0,1,2,3 of a CWaterQuad, which
// AddWaterLevelQuad sorts min/max the same way Water.cpp does) and from
// SplitWaterRectangleAlongXLine, which lerps P1->P2 and P3->P4 along X.
//
// The sea bed pass is deliberately left alone: it sits at z = -70 under everything and a region's
// own terrain occludes it, exactly as SA's does inside the map.
#include <plugin.h>
#include <CTimer.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>
#include "WaterRender.h"
#include "Water.h"
#include "Settings.h"
#include "MemPatch.h"
#include "Log.h"

using namespace plugin;
using Water::Quad;
using Water::RenPar;

namespace {

constexpr uintptr_t kOutsideOceanCall = 0x6F004E;    // RenderWater+0x9FE
constexpr uintptr_t kRenderWaterRectangle = 0x6EC5D0;
constexpr int kBlockSize = 500;
constexpr int kWorldEdge = 3000;
// One block can never legitimately need more than this; a water map pathological enough to try is a
// bug in the map, not something to spend a frame on.
constexpr int kMaxRectsPerBlock = 512;
// RenderWaterRectangle's detailed path divides by (width / cell) with no guard, so a rectangle
// thinner than one water cell in either direction is an integer divide by zero inside the game.
constexpr int kMinRect = 32;

typedef void(__cdecl* RectFn)(int, int, int, int, RenPar, RenPar, RenPar, RenPar);
const RectFn CallRenderWaterRectangle = reinterpret_cast<RectFn>(kRenderWaterRectangle);

struct IRect {
    int minX, minY, maxX, maxY;
};

std::vector<IRect> gCoverage;
// gOutside indices per 500-unit water block, so drawing a block only ever touches its own quads.
std::map<std::pair<int, int>, std::vector<uint32_t>> gByBlock;
// Per quad: the origin of the 32-unit grid its water map is laid out on (its coverage rect's
// corner), so block windows can be snapped onto the same grid.
std::vector<std::pair<int, int>> gQuadPhase;

std::vector<IRect> gPieces, gNextPieces;   // scratch for the coverage subtraction, reused per call

bool gDebug = false;
int gMode = 1;   // 1 full, 2 ocean suppression only, 3 region quads only
unsigned gFrame = 0xFFFFFFFF;
unsigned gLastReportMs = 0;
bool gReportedFirst = false;
int gRegionRects = 0;   // region water drawn this frame
int gOceanRects = 0;    // SA's own ocean left standing outside the coverage rects
int gClamped = 0;

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }

// The renderer splits rectangles at the camera-range lines, which are even integers, and then
// subdivides the inner piece in 2-unit steps (width / 2 -> a 1-unit piece divides by zero). Even
// edges can never leave a 1-unit piece; adjacent cells round identically, so no seams appear.
int EvenRound(float v) { return (int)lrintf(v * 0.5f) * 2; }

std::pair<int, int> BlockOf(int worldX, int worldY) {
    return { FloorDiv(worldX + kWorldEdge, kBlockSize), FloorDiv(worldY + kWorldEdge, kBlockSize) };
}

RenPar ParOf(const Water::Vert& v) {
    return { v.z, v.bigWaves, v.smallWaves, (int8_t)(v.flowX * 64.0f), (int8_t)(v.flowY * 64.0f), { 0, 0 } };
}

float Bilinear(float p00, float p10, float p01, float p11, float fx, float fy) {
    return p00 + (p10 - p00) * fx + (p01 - p00) * fy + (p00 - p10 - p01 + p11) * fx * fy;
}

// Corner parameters of the sub-rectangle [tx0,tx1]x[ty0,ty1] of the rectangle
// [sx0,sx1]x[sy0,sy1] whose corners carry `src` in P1..P4 order.
void ClipParams(float sx0, float sy0, float sx1, float sy1, const RenPar src[4],
                int tx0, int ty0, int tx1, int ty1, RenPar out[4]) {
    const float spanX = sx1 - sx0;
    const float spanY = sy1 - sy0;
    const float xs[4] = { (float)tx0, (float)tx1, (float)tx0, (float)tx1 };
    const float ys[4] = { (float)ty0, (float)ty0, (float)ty1, (float)ty1 };
    for (int i = 0; i < 4; i++) {
        float fx = spanX > 0.0f ? (xs[i] - sx0) / spanX : 0.0f;
        float fy = spanY > 0.0f ? (ys[i] - sy0) / spanY : 0.0f;
        out[i].z = Bilinear(src[0].z, src[1].z, src[2].z, src[3].z, fx, fy);
        out[i].bigWaves = Bilinear(src[0].bigWaves, src[1].bigWaves, src[2].bigWaves, src[3].bigWaves, fx, fy);
        out[i].smallWaves = Bilinear(src[0].smallWaves, src[1].smallWaves, src[2].smallWaves, src[3].smallWaves, fx, fy);
        out[i].flowX = (int8_t)lrintf(Bilinear(src[0].flowX, src[1].flowX, src[2].flowX, src[3].flowX, fx, fy));
        out[i].flowY = (int8_t)lrintf(Bilinear(src[0].flowY, src[1].flowY, src[2].flowY, src[3].flowY, fx, fy));
        out[i].pad[0] = out[i].pad[1] = 0;
    }
}

void Draw(int minX, int minY, int maxX, int maxY, const RenPar p[4], int& counter) {
    minX &= ~1; minY &= ~1; maxX = (maxX + 1) & ~1; maxY = (maxY + 1) & ~1;
    if (minX >= maxX || minY >= maxY) return;
    if (maxX - minX < kMinRect) maxX = minX + kMinRect;
    if (maxY - minY < kMinRect) maxY = minY + kMinRect;
    CallRenderWaterRectangle(minX, maxX, minY, maxY, p[0], p[1], p[2], p[3]);
    counter++;
}

// Largest grid line at or below v, on the 32-unit grid anchored at `phase`. Adjacent blocks snap
// their shared edge to the same line, so the windows tile without gaps or overlaps.
int SnapDown(int v, int phase) {
    return phase + FloorDiv(v - phase, kMinRect) * kMinRect;
}

// Cuts every coverage rectangle out of `gPieces`, leaving the parts of the block where SA's flat
// ocean still belongs. Rectangles only, so a straight guillotine split is exact.
void SubtractCoverage() {
    for (const IRect& c : gCoverage) {
        gNextPieces.clear();
        for (const IRect& r : gPieces) {
            if (r.maxX <= c.minX || r.minX >= c.maxX || r.maxY <= c.minY || r.minY >= c.maxY) {
                gNextPieces.push_back(r);
                continue;
            }
            if (r.minY < c.minY) gNextPieces.push_back({ r.minX, r.minY, r.maxX, c.minY });
            if (r.maxY > c.maxY) gNextPieces.push_back({ r.minX, c.maxY, r.maxX, r.maxY });
            const int y0 = std::max(r.minY, c.minY);
            const int y1 = std::min(r.maxY, c.maxY);
            if (r.minX < c.minX) gNextPieces.push_back({ r.minX, y0, c.minX, y1 });
            if (r.maxX > c.maxX) gNextPieces.push_back({ c.maxX, y0, r.maxX, y1 });
        }
        gPieces.swap(gNextPieces);
        if (gPieces.empty()) return;
    }
}

void Report() {
    if (!gDebug) return;
    const unsigned now = CTimer::m_snTimeInMilliseconds;
    if (gReportedFirst && now - gLastReportMs < 1000) return;
    gReportedFirst = true;
    gLastReportMs = now;
    Log::Write("water render: last frame drew %d region rectangle(s) and left %d vanilla ocean "
               "rectangle(s)%s", gRegionRects, gOceanRects,
               gClamped ? " - a block hit the per-block rectangle cap" : "");
    gClamped = 0;
}

void __cdecl RenderOutsideWorldBlock(int minX, int maxX, int y1, int y2,
                                     RenPar p1, RenPar p2, RenPar p3, RenPar p4) {
    if (CTimer::m_FrameCounter != gFrame) {
        if (gFrame != 0xFFFFFFFF) Report();
        gFrame = CTimer::m_FrameCounter;
        gRegionRects = gOceanRects = 0;
    }

    const RenPar flat[4] = { p1, p2, p3, p4 };

    gPieces.clear();
    gPieces.push_back({ minX, y1, maxX, y2 });
    if (gMode != 3) SubtractCoverage();
    for (const IRect& r : gPieces) {
        RenPar p[4];
        ClipParams((float)minX, (float)y1, (float)maxX, (float)y2, flat,
                   r.minX, r.minY, r.maxX, r.maxY, p);
        Draw(r.minX, r.minY, r.maxX, r.maxY, p, gOceanRects);
    }
    if (gMode == 2) return;

    auto bucket = gByBlock.find(BlockOf(minX, y1));
    if (bucket == gByBlock.end()) return;

    const std::vector<Quad>& quads = Water::OutsideQuads();
    int drawn = 0;
    for (uint32_t index : bucket->second) {
        const Quad& q = quads[index];
        const auto [phaseX, phaseY] = gQuadPhase[index];
        // Clip to the block window snapped onto the quad's own cell grid, so a quad edge can never
        // leave a sliver thinner than a cell next to a block edge.
        const int cx0 = std::max(SnapDown(minX, phaseX), EvenRound(q.v[0].x));
        const int cy0 = std::max(SnapDown(y1, phaseY), EvenRound(q.v[0].y));
        const int cx1 = std::min(SnapDown(maxX, phaseX), EvenRound(q.v[3].x));
        const int cy1 = std::min(SnapDown(y2, phaseY), EvenRound(q.v[3].y));
        if (cx0 >= cx1 || cy0 >= cy1) continue;
        if (++drawn > kMaxRectsPerBlock) { gClamped++; break; }

        const RenPar src[4] = { ParOf(q.v[0]), ParOf(q.v[1]), ParOf(q.v[2]), ParOf(q.v[3]) };
        RenPar p[4];
        ClipParams(q.v[0].x, q.v[0].y, q.v[3].x, q.v[3].y, src, cx0, cy0, cx1, cy1, p);
        Draw(cx0, cy0, cx1, cy1, p, gRegionRects);
    }
}

void BuildIndex() {
    for (const Water::Rect& r : Water::Coverage())
        gCoverage.push_back({ EvenRound(r.minX), EvenRound(r.minY), EvenRound(r.maxX), EvenRound(r.maxY) });

    const std::vector<Quad>& quads = Water::OutsideQuads();
    gQuadPhase.assign(quads.size(), { 0, 0 });
    for (uint32_t i = 0; i < quads.size(); i++) {
        const Quad& q = quads[i];
        for (const IRect& c : gCoverage) {
            if (q.v[0].x >= c.minX && q.v[3].x <= c.maxX && q.v[0].y >= c.minY && q.v[3].y <= c.maxY) {
                gQuadPhase[i] = { c.minX, c.minY };
                break;
            }
        }
        // Snapped windows reach up to a cell past the block edge, so register the quad with the
        // neighbouring blocks as well.
        auto lo = BlockOf(EvenRound(q.v[0].x) - kMinRect, EvenRound(q.v[0].y) - kMinRect);
        auto hi = BlockOf(EvenRound(q.v[3].x) - 1 + kMinRect, EvenRound(q.v[3].y) - 1 + kMinRect);
        for (int bx = lo.first; bx <= hi.first; bx++)
            for (int by = lo.second; by <= hi.second; by++)
                gByBlock[{ bx, by }].push_back(i);
    }
}

}  // namespace

namespace WaterRender {
    void Install() {
        if (Settings::world.extendWaterRendering <= 0) return;
        if (Water::OutsideQuads().empty() || Water::Coverage().empty()) return;

        BuildIndex();
        gDebug = Settings::world.debug;
        gMode = Settings::world.extendWaterRendering;
        if (gMode != 2 && gMode != 3) gMode = 1;
        gPieces.reserve(16);
        gNextPieces.reserve(16);

        if (!MemPatch::RetargetCall(kOutsideOceanCall, kRenderWaterRectangle,
                                    reinterpret_cast<const void*>(&RenderOutsideWorldBlock)))
            return;

        Log::Write("water render: %u quad(s) over %u block(s) will replace SA's flat ocean inside "
                   "%u coverage rect(s) (RenderWater+0x9FE -> RenderWaterRectangle retargeted)",
                   (unsigned)Water::OutsideQuads().size(), (unsigned)gByBlock.size(),
                   (unsigned)gCoverage.size());
    }
}
