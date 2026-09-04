#pragma once
#include <string>
#include <vector>

// A custom map (e.g. Vice City) placed somewhere in the SA world.
// Tiles are addressed in the map's OWN coordinate system; originX/Y say where its (0,0) sits in SA.
struct MapRegion {
    std::string name;
    std::string tileFolder;   // absolute path
    std::string filePattern;  // one %d -> row-major index, two %d -> column,row

    float originX, originY;   // SA world coords of the region's local (0,0)
    float tileSize;           // world units per tile
    float gridMinX;           // local X of the west edge of column 0
    float gridMaxY;           // local Y of the north edge of row 0
    int tilesX, tilesY;

    float TileWest(int col) const  { return originX + gridMinX + col * tileSize; }
    float TileNorth(int row) const { return originY + gridMaxY - row * tileSize; }
};

struct WorldSettings {
    bool fixPickupPositions;
    bool fixCarGeneratorPositions;
    float compressedPositionScale;   // int16 units per world unit; vanilla 8 => +/-4096
    bool extendPauseMap;
};

namespace Settings {
    extern std::vector<MapRegion> regions;
    extern WorldSettings world;
    void Load();
    // Bounding box of the vanilla map plus every region's tile grid, in SA world coordinates.
    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY);
}
