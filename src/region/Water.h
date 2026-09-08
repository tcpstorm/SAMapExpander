#pragma once
#include <cstdint>
#include <vector>

namespace Water {

// CRenPar: the per-vertex water parameters every SA water quad/rectangle function takes.
struct RenPar {
    float z, bigWaves, smallWaves;
    int8_t flowX, flowY;   // fixed point, world flow * 64
    int8_t pad[2];
};
static_assert(sizeof(RenPar) == 16, "CRenPar is 16 bytes");

struct Vert {
    float x, y, z, bigWaves, smallWaves, flowX, flowY;
};

// v[0]=(minX,minY) v[1]=(maxX,minY) v[2]=(minX,maxY) v[3]=(maxX,maxY): the order
// CWaterLevel::AddWaterLevelQuad sorts into and TestQuadToGetWaterLevel (0x6E5BB0) assumes.
struct Quad {
    Vert v[4];
    unsigned flags;
};

struct Rect {
    float minX, minY, maxX, maxY;
};

void Install();

// Quads beyond the vanilla +/-3000 map and the rectangles where a region's water map is
// authoritative out there. Both are final once Install() has run.
const std::vector<Quad>& OutsideQuads();
const std::vector<Rect>& Coverage();

}
