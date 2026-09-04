#pragma once
#include <string>
#include <vector>

// A custom map (e.g. Vice City) placed somewhere in the SA world.
// Tiles are addressed in the map's OWN coordinate system; originX/Y say where its (0,0) sits in SA.
struct MapRegion {
    std::string name;
    std::string tileFolder;   // absolute path
    std::string filePattern;  // one %d -> row-major index, two %d -> column,row
    std::string waterFile;    // absolute path to a SA water.dat or III/VC waterpro.dat, or empty
    float waterXOffset;       // waterpro.dat only: VC's WATER_X_OFFSET (400), 0 for GTA III

    std::vector<std::string> iplFiles;  // IPL files to load with the origin added to every coordinate

    float originX, originY;   // SA world coords of the region's local (0,0)
    float tileSize;           // world units per tile
    float gridMinX;           // local X of the west edge of column 0
    float gridMaxY;           // local Y of the north edge of row 0
    int tilesX, tilesY;

    std::vector<std::string> zoneFiles;  // III/VC .zon or SA IPL files, in the region's own coords
    std::string zoneNamesFile;           // "GXTKEY = Display Name" lines; empty if none

    float TileWest(int col) const  { return originX + gridMinX + col * tileSize; }
    float TileNorth(int row) const { return originY + gridMaxY - row * tileSize; }
};

struct WorldSettings {
    bool fixPickupPositions;
    bool fixCarGeneratorPositions;
    float compressedPositionScale;   // int16 units per world unit; vanilla 8 => +/-4096
    bool extendPauseMap;
    float pauseMapMargin;            // world units of free scrolling past the map/regions on every side
    bool pauseMapCustomBounds;       // if set, PauseMapMin/Max override the computed extent
    float pauseMapMinX, pauseMapMaxX, pauseMapMinY, pauseMapMaxY;
};

namespace Settings {
    extern std::vector<MapRegion> regions;
    extern WorldSettings world;
    void Load();
    // Bounding box of the vanilla map plus every region's tile grid, in SA world coordinates.
    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY);
}
