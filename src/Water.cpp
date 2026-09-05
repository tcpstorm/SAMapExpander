// SA stores water in a fixed 12x12 grid of 500-unit blocks covering -3000..3000. Two consequences
// shape this module:
//   * CWaterLevel::AddWaterLevelVertex (0x6E5A40) clamps every vertex to +/-3000 and zeroes its
//     wave data, and CWaterLevel::FillQuadsAndTrianglesList (0x6E7B30) only ever indexes quads into
//     that grid, so a region placed outside the vanilla map cannot use SA's quad storage at all.
//   * CWaterLevel::GetWaterLevelNoWaves (0x6E8580) returns *true* with level 0.0 whenever the block
//     index falls outside 0..11 - SA's endless ocean. Water physics therefore already "work" beyond
//     +/-3000, but always at z = 0 and everywhere, which is exactly what a region water map has to
//     override.
// So quads inside the vanilla map go through the vanilla adder and behave like stock water; quads
// outside live in our own store and are served by an inline hook on GetWaterLevelNoWaves, which
// every water query in the game funnels through (directly or via CWaterLevel::GetWaterLevel).
// WaterRender.cpp draws them; this module only owns the data.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Water.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr float kWorldEdge = 3000.0f;
constexpr unsigned kMaxQuads = 301;
constexpr unsigned kMaxVertices = 1021;

// waterpro.dat (GTA III / Vice City): int32 count, float levels[48], CRect rects[48],
// int8 coarse[64][64], int8 fine[128][128]. Fine cells are 32 units and the table is addressed
// [xSector][ySector] from WATER_START = -2048.
constexpr int kFineCells = 128;
constexpr float kFineCellSize = 32.0f;
constexpr float kWaterStart = -2048.0f;
constexpr int kNoWater = -128;
constexpr size_t kWaterProSize = 4 + 48 * 4 + 48 * 16 + 64 * 64 + 128 * 128;

using Water::Quad;
using Water::RenPar;
using Water::Rect;
using Water::Vert;

std::vector<Quad> gOutside;    // beyond +/-3000: served by our GetWaterLevelNoWaves hook
std::vector<Quad> gInside;     // within the vanilla map: handed to CWaterLevel::AddWaterLevelQuad
std::vector<Rect> gCoverage;   // where a region's water map is authoritative (outside the map only)

// Uniform bucket grid over gOutside so the level query stays O(few) per call.
float gGridMinX = 0.0f, gGridMinY = 0.0f, gGridCell = 512.0f;
int gGridCols = 0, gGridRows = 0;
std::vector<std::vector<uint32_t>> gBuckets;

typedef void(__cdecl* AddQuadFn)(int, int, RenPar, int, int, RenPar, int, int, RenPar,
                                 int, int, RenPar, unsigned);
const AddQuadFn CallAddWaterLevelQuad = reinterpret_cast<AddQuadFn>(0x6E7EF0);

uint32_t& NumWaterQuads = *reinterpret_cast<uint32_t*>(0xC22888);
uint32_t& NumWaterVertices = *reinterpret_cast<uint32_t*>(0xC2288C);

bool OutsideWorld(float x, float y) {
    return x < -kWorldEdge || x >= kWorldEdge || y < -kWorldEdge || y >= kWorldEdge;
}

// Fills a Quad from four arbitrary corners; fails unless they form an axis-aligned rectangle,
// which is all SA's quad test can represent.
bool MakeQuad(const Vert in[4], unsigned flags, Quad& out) {
    float minX = in[0].x, maxX = in[0].x, minY = in[0].y, maxY = in[0].y;
    for (int i = 1; i < 4; i++) {
        if (in[i].x < minX) minX = in[i].x;
        if (in[i].x > maxX) maxX = in[i].x;
        if (in[i].y < minY) minY = in[i].y;
        if (in[i].y > maxY) maxY = in[i].y;
    }
    if (minX == maxX || minY == maxY) return false;

    bool filled[4] = { false, false, false, false };
    for (int i = 0; i < 4; i++) {
        int slot;
        if (in[i].x == minX && in[i].y == minY) slot = 0;
        else if (in[i].x == maxX && in[i].y == minY) slot = 1;
        else if (in[i].x == minX && in[i].y == maxY) slot = 2;
        else if (in[i].x == maxX && in[i].y == maxY) slot = 3;
        else return false;
        if (filled[slot]) return false;
        filled[slot] = true;
        out.v[slot] = in[i];
    }
    out.flags = flags;
    return true;
}

void MakeFlatQuad(float minX, float minY, float maxX, float maxY, float z, Quad& out) {
    const float xs[4] = { minX, maxX, minX, maxX };
    const float ys[4] = { minY, minY, maxY, maxY };
    for (int i = 0; i < 4; i++)
        out.v[i] = { xs[i], ys[i], z, 0.0f, 0.0f, 0.0f, 0.0f };
    out.flags = 1;
}

// Mirrors CWaterLevel::TestQuadToGetWaterLevel (0x6E5BB0): reject outside the rectangle, then
// interpolate over whichever of the two triangles the point falls in.
bool TestQuad(const Quad& q, float x, float y, float* level, float* big, float* small) {
    const Vert& a = q.v[0];
    const Vert& b = q.v[1];
    const Vert& c = q.v[2];
    const Vert& d = q.v[3];
    if (x < a.x || b.x < x || y < a.y || c.y < y) return false;

    float fx = (x - a.x) / (b.x - a.x);
    float fy = (y - a.y) / (c.y - a.y);
    if (fx + fy <= 1.0f) {
        if (level) *level = a.z + (b.z - a.z) * fx + (c.z - a.z) * fy;
        if (big)   *big = a.bigWaves + (b.bigWaves - a.bigWaves) * fx + (c.bigWaves - a.bigWaves) * fy;
        if (small) *small = a.smallWaves + (b.smallWaves - a.smallWaves) * fx + (c.smallWaves - a.smallWaves) * fy;
    } else {
        float gx = 1.0f - fx;
        float gy = 1.0f - fy;
        if (level) *level = d.z + (c.z - d.z) * gx + (b.z - d.z) * gy;
        if (big)   *big = d.bigWaves + (c.bigWaves - d.bigWaves) * gx + (b.bigWaves - d.bigWaves) * gy;
        if (small) *small = d.smallWaves + (c.smallWaves - d.smallWaves) * gx + (b.smallWaves - d.smallWaves) * gy;
    }
    return true;
}

std::vector<char> ReadWholeFile(const std::string& path) {
    std::vector<char> data;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize((size_t)size);
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return data;
}

// SA water.dat: one polygon per line, each vertex "x y z flowX flowY bigWaves smallWaves",
// optionally followed by a flag word. Everything but x/y is copied through unchanged.
int LoadSaWaterDat(const MapRegion& region, const std::vector<char>& data, std::vector<Quad>& out) {
    int added = 0, skipped = 0;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = pos;
        while (end < data.size() && data[end] != '\n') end++;
        std::string line(data.data() + pos, end - pos);
        pos = end + 1;

        size_t comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.erase(comment);

        float num[32];
        int count = 0;
        const char* p = line.c_str();
        while (count < 32) {
            char* next = nullptr;
            float value = strtof(p, &next);
            if (next == p) break;
            num[count++] = value;
            p = next;
        }
        if (count < 28) {
            if (count >= 21) skipped++;
            continue;
        }

        Vert in[4];
        for (int i = 0; i < 4; i++) {
            const float* f = num + i * 7;
            in[i] = { f[0] + region.originX, f[1] + region.originY, f[2] + region.originZ, f[5], f[6], f[3], f[4] };
        }
        unsigned flags = count > 28 ? (unsigned)num[28] : 0u;

        Quad q;
        if (!MakeQuad(in, flags, q)) { skipped++; continue; }
        out.push_back(q);
        added++;
    }
    if (skipped)
        Log::Write("[%s] water: %d line(s) skipped (triangles or non-rectangular quads)",
                   region.name.c_str(), skipped);
    return added;
}

// waterpro.dat: greedy-merge runs of fine cells that share a water level into rectangles, widest
// first then as tall as the full width allows. Levels are flat - the format carries no wave data.
int LoadWaterPro(const MapRegion& region, const std::vector<char>& data, std::vector<Quad>& out,
                 Rect& coverage) {
    if (data.size() < kWaterProSize) {
        Log::Write("[%s] water: %s is %u bytes, expected at least %u for waterpro.dat",
                   region.name.c_str(), region.waterFile.c_str(), (unsigned)data.size(),
                   (unsigned)kWaterProSize);
        return -1;
    }

    float levels[48];
    memcpy(levels, data.data() + 4, sizeof(levels));
    const int8_t* fine = reinterpret_cast<const int8_t*>(data.data() + 4 + 48 * 4 + 48 * 16 + 64 * 64);

    // Vice City's own lookup adds WATER_X_OFFSET before indexing, so cell x sits at
    // 32*x - 2048 - WaterXOffset in world space. GTA III has no such shift.
    const float baseX = region.originX + kWaterStart - region.waterXOffset;
    const float baseY = region.originY + kWaterStart;
    coverage = { baseX, baseY, baseX + kFineCells * kFineCellSize, baseY + kFineCells * kFineCellSize };

    std::vector<char> used((size_t)kFineCells * kFineCells, 0);
    auto cell = [&](int ix, int iy) { return fine[ix * kFineCells + iy]; };

    int added = 0;
    for (int ix = 0; ix < kFineCells; ix++) {
        for (int iy = 0; iy < kFineCells; iy++) {
            size_t here = (size_t)ix * kFineCells + iy;
            int8_t idx = cell(ix, iy);
            if (used[here] || idx == kNoWater || idx < 0 || idx >= 48) continue;

            int w = 1;
            while (ix + w < kFineCells && !used[(size_t)(ix + w) * kFineCells + iy] &&
                   cell(ix + w, iy) == idx)
                w++;

            int h = 1;
            for (;;) {
                int ny = iy + h;
                if (ny >= kFineCells) break;
                bool wholeRow = true;
                for (int k = 0; k < w && wholeRow; k++)
                    wholeRow = !used[(size_t)(ix + k) * kFineCells + ny] && cell(ix + k, ny) == idx;
                if (!wholeRow) break;
                h++;
            }

            for (int a = 0; a < w; a++)
                for (int b = 0; b < h; b++)
                    used[(size_t)(ix + a) * kFineCells + iy + b] = 1;

            Quad q;
            MakeFlatQuad(baseX + ix * kFineCellSize, baseY + iy * kFineCellSize,
                         baseX + (ix + w) * kFineCellSize, baseY + (iy + h) * kFineCellSize,
                         levels[idx] + region.originZ, q);
            out.push_back(q);
            added++;
        }
    }
    return added;
}

void BuildLookupGrid() {
    gBuckets.clear();
    gGridCols = gGridRows = 0;
    if (gOutside.empty()) return;

    float minX = gOutside[0].v[0].x, minY = gOutside[0].v[0].y;
    float maxX = gOutside[0].v[3].x, maxY = gOutside[0].v[3].y;
    for (const Quad& q : gOutside) {
        if (q.v[0].x < minX) minX = q.v[0].x;
        if (q.v[0].y < minY) minY = q.v[0].y;
        if (q.v[3].x > maxX) maxX = q.v[3].x;
        if (q.v[3].y > maxY) maxY = q.v[3].y;
    }
    gGridMinX = minX;
    gGridMinY = minY;
    gGridCols = (int)((maxX - minX) / gGridCell) + 1;
    gGridRows = (int)((maxY - minY) / gGridCell) + 1;
    gBuckets.resize((size_t)gGridCols * gGridRows);

    for (uint32_t i = 0; i < gOutside.size(); i++) {
        const Quad& q = gOutside[i];
        int x0 = (int)((q.v[0].x - gGridMinX) / gGridCell);
        int x1 = (int)((q.v[3].x - gGridMinX) / gGridCell);
        int y0 = (int)((q.v[0].y - gGridMinY) / gGridCell);
        int y1 = (int)((q.v[3].y - gGridMinY) / gGridCell);
        if (x1 >= gGridCols) x1 = gGridCols - 1;
        if (y1 >= gGridRows) y1 = gGridRows - 1;
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                gBuckets[(size_t)x * gGridRows + y].push_back(i);
    }
}

bool QueryOutside(float x, float y, float* level, float* big, float* small, bool& covered) {
    covered = false;
    for (const Rect& r : gCoverage)
        if (x >= r.minX && x <= r.maxX && y >= r.minY && y <= r.maxY) covered = true;

    if (gBuckets.empty()) return false;
    int cx = (int)((x - gGridMinX) / gGridCell);
    int cy = (int)((y - gGridMinY) / gGridCell);
    if (cx < 0 || cy < 0 || cx >= gGridCols || cy >= gGridRows) return false;

    for (uint32_t i : gBuckets[(size_t)cx * gGridRows + cy])
        if (TestQuad(gOutside[i], x, y, level, big, small)) return true;
    return false;
}

SafetyHookInline gLevelHook;
SafetyHookInline gFillHook;

char __cdecl Hooked_GetWaterLevelNoWaves(float x, float y, float z, float* level, float* big, float* small) {
    if (!OutsideWorld(x, y))
        return gLevelHook.ccall<char>(x, y, z, level, big, small);

    bool covered = false;
    if (QueryOutside(x, y, level, big, small, covered)) return 1;
    // Inside a region's water map but on a dry cell: suppress SA's endless ocean, which would
    // otherwise flood everything the region draws below z = 0.
    if (covered) return 0;
    return gLevelHook.ccall<char>(x, y, z, level, big, small);
}

void RegisterInsideQuads() {
    if (gInside.empty()) return;

    unsigned added = 0, refused = 0;
    for (const Quad& q : gInside) {
        if (NumWaterQuads + 1 > kMaxQuads || NumWaterVertices + 4 > kMaxVertices) { refused++; continue; }
        RenPar p[4];
        int px[4], py[4];
        for (int i = 0; i < 4; i++) {
            const Vert& v = q.v[i];
            p[i] = { v.z, v.bigWaves, v.smallWaves, (int8_t)(v.flowX * 64.0f), (int8_t)(v.flowY * 64.0f), { 0, 0 } };
            px[i] = (int)v.x;
            py[i] = (int)v.y;
        }
        CallAddWaterLevelQuad(px[0], py[0], p[0], px[1], py[1], p[1], px[2], py[2], p[2],
                              px[3], py[3], p[3], q.flags);
        added++;
    }
    Log::Write("water: %u quad(s) registered with the game (quads %u/%u, vertices %u/%u)%s",
               added, NumWaterQuads, kMaxQuads, NumWaterVertices, kMaxVertices,
               refused ? " - ARRAYS FULL, rest dropped" : "");
    if (refused)
        Log::Write("water: %u quad(s) dropped; SA's quad/vertex arrays are packed solid at "
                   "0xC21C90/0xC22910 with no spare room", refused);
}

// Only register once per build of the vanilla tables: if the counter is still at or past where we
// left it, nothing was reset and adding again would double the quads.
uint32_t gRegisteredUpTo = 0;

void __cdecl Hooked_FillQuadsAndTrianglesList() {
    if (gRegisteredUpTo == 0 || NumWaterQuads < gRegisteredUpTo) {
        RegisterInsideQuads();
        gRegisteredUpTo = NumWaterQuads;
    }
    gFillHook.ccall<void>();
}

void LoadRegion(const MapRegion& region) {
    std::vector<char> data = ReadWholeFile(region.waterFile);
    if (data.empty()) {
        Log::Write("[%s] water file not found or empty: %s", region.name.c_str(), region.waterFile.c_str());
        return;
    }

    std::vector<Quad> quads;
    Rect coverage = { 0, 0, 0, 0 };
    bool haveCoverage = false;
    int added;

    if (data.size() >= kWaterProSize && memchr(data.data(), 0, 64) != nullptr) {
        added = LoadWaterPro(region, data, quads, coverage);
        haveCoverage = added >= 0;
    } else {
        added = LoadSaWaterDat(region, data, quads);
    }
    if (added <= 0) {
        Log::Write("[%s] water: no usable quads in %s", region.name.c_str(), region.waterFile.c_str());
        return;
    }

    unsigned inside = 0, outside = 0;
    for (const Quad& q : quads) {
        if (OutsideWorld(q.v[0].x, q.v[0].y) || OutsideWorld(q.v[3].x, q.v[3].y)) {
            if (!haveCoverage) {
                Rect r = { q.v[0].x, q.v[0].y, q.v[3].x, q.v[3].y };
                if (!outside) coverage = r;
                if (r.minX < coverage.minX) coverage.minX = r.minX;
                if (r.minY < coverage.minY) coverage.minY = r.minY;
                if (r.maxX > coverage.maxX) coverage.maxX = r.maxX;
                if (r.maxY > coverage.maxY) coverage.maxY = r.maxY;
            }
            gOutside.push_back(q);
            outside++;
        } else {
            gInside.push_back(q);
            inside++;
        }
    }
    if (outside) gCoverage.push_back(coverage);

    if (outside)
        Log::Write("[%s] water: %d quad(s) from %s - %u inside the vanilla map, %u outside and "
                   "authoritative over x %.0f..%.0f, y %.0f..%.0f",
                   region.name.c_str(), added, region.waterFile.c_str(), inside, outside,
                   coverage.minX, coverage.maxX, coverage.minY, coverage.maxY);
    else
        Log::Write("[%s] water: %d quad(s) from %s, all inside the vanilla map",
                   region.name.c_str(), added, region.waterFile.c_str());
}

}  // namespace

namespace Water {
    void Install() {
        for (const MapRegion& r : Settings::regions)
            if (!r.waterFile.empty()) LoadRegion(r);

        if (gInside.empty() && gOutside.empty()) return;

        BuildLookupGrid();

        if (!gInside.empty())
            gFillHook = safetyhook::create_inline(reinterpret_cast<void*>(0x6E7B30),
                                                  reinterpret_cast<void*>(&Hooked_FillQuadsAndTrianglesList));
        if (!gOutside.empty())
            gLevelHook = safetyhook::create_inline(reinterpret_cast<void*>(0x6E8580),
                                                   reinterpret_cast<void*>(&Hooked_GetWaterLevelNoWaves));

        Log::Write("water: %u quad(s) queued for the game, %u served outside the vanilla map",
                   (unsigned)gInside.size(), (unsigned)gOutside.size());
    }

    const std::vector<Quad>& OutsideQuads() { return gOutside; }
    const std::vector<Rect>& Coverage() { return gCoverage; }
}
