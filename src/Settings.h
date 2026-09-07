#pragma once
#include <string>
#include <vector>

// One box, in the region's OWN coordinates, that means "the player is standing in this interior".
//
// Vice City and San Andreas hide interiors the same way: every entity carries an area code (the
// third column of an IPL inst line) and the renderer only draws entities whose code equals
// CGame::currArea, or is 13, which both games read as "everywhere". Vice City's interiors are
// built inside the real buildings at real world coordinates, so once the area codes are carried
// across - which they already are - the geometry is correctly hidden and there is nothing to
// relocate. What is missing is the other half: Vice City switched the area from its own mission
// script when the player walked through a door, and that script is not here. These boxes are that
// switch, one per room, written out because VC's data does not record where its doorways are.
struct InteriorVolume {
    int area;                 // the area code its entities carry, i.e. the IPL "interior" column
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    std::string name;         // shown in the log and by the F8 read-out; nothing reads it
};

// A custom map (e.g. Vice City) placed somewhere in the SA world.
// Tiles are addressed in the map's OWN coordinate system; originX/Y say where its (0,0) sits in SA.
struct MapRegion {
    std::string name;
    std::string tileFolder;   // absolute path
    std::string filePattern;  // one %d -> row-major index, two %d -> column,row
    std::string waterFile;    // absolute path to a SA water.dat or III/VC waterpro.dat, or empty
    float waterXOffset;       // waterpro.dat only: VC's WATER_X_OFFSET (400), 0 for GTA III

    // The region's own map assets, all in the region's coordinate system.
    std::vector<std::string> imgFiles;  // IMG archives registered with the streamer
    std::vector<std::string> ideFiles;  // IDE files; model ids are remapped into the free range
    std::vector<std::string> iplFiles;  // IPL files to load with the origin added to every coordinate
    std::vector<std::string> colFiles;  // .col archives loaded into permanent collision slots
    std::string zoneInfoLabel;          // existing zone whose CZoneInfo slot region zones share
    std::string regionName;             // area name over the whole tile grid, empty for none

    float originX, originY;   // SA world coords of the region's local (0,0)
    float originZ;            // added to every region height (water levels, zone boxes, IPL/inst z)
    float tileSize;           // world units per tile
    float gridMinX;           // local X of the west edge of column 0
    float gridMaxY;           // local Y of the north edge of row 0
    int tilesX, tilesY;

    std::vector<std::string> zoneFiles;  // III/VC .zon or SA IPL files, in the region's own coords
    std::string zoneNamesFile;           // "GXTKEY = Display Name" lines; empty if none

    // Interior<N> lines from this region's ini section, in the region's own coordinates. Several
    // may name the same area: an interior split over rooms that are not one box (Vercetti's house
    // and its tower) needs one box each, and any of them makes the whole area visible.
    std::vector<InteriorVolume> interiors;

    float TileWest(int col) const  { return originX + gridMinX + col * tileSize; }
    float TileNorth(int row) const { return originY + gridMaxY - row * tileSize; }
};

struct WorldSettings {
    bool fixPickupPositions;
    bool fixCarGeneratorPositions;
    float compressedPositionScale;   // int16 units per world unit; vanilla 8 => +/-4096
    bool fixMeleeBeyondMap;          // let CEntityScanner see peds and vehicles outside +/-3000
    bool extendPauseMap;
    float pauseMapMargin;            // world units of free scrolling past the map/regions on every side
    bool pauseMapCustomBounds;       // if set, PauseMapMin/Max override the computed extent
    float pauseMapMinX, pauseMapMaxX, pauseMapMinY, pauseMapMaxY;
    std::vector<std::string> gxtFiles;  // GXT files patched into the game's text lookup
    bool gxtLogMisses;                  // log keys neither the game nor we can answer
    int extendWaterRendering;        // 0 off, 1 full, 2 only hide SA's ocean in coverage, 3 only draw region quads
    int modelIdLimit;                // number of DFF ids; 20000 leaves the game alone
    int freeModelIdStart, freeModelIdEnd;  // inclusive range of model ids region IDEs may claim; end < 0 = up to the model id limit
    int atomicModelInfoLimit;              // CModelInfo::ms_atomicModelInfoStore capacity; < 0 = ask ModelLimits
    bool debug;                      // verbose log, F8 diagnostics, water render statistics
};

namespace Settings {
    extern std::vector<MapRegion> regions;
    extern WorldSettings world;
    void Load();
    // Bounding box of the vanilla map plus every region's tile grid, in SA world coordinates.
    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY);
    // True when this process was started by an SA-MP launcher. samp.dll is injected after ASI
    // plugins have already run, so its presence cannot be relied on this early; the launcher's
    // command line (-c -h <host> -p <port> -n <nick>) can.
    bool RunningUnderSaMp();
}
