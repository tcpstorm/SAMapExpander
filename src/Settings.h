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

    // The region's own map assets, all in the region's coordinate system.
    std::vector<std::string> imgFiles;  // IMG archives registered with the streamer
    std::vector<std::string> ideFiles;  // IDE files; model ids are remapped into the free range
    std::vector<std::string> iplFiles;  // IPL files to load with the origin added to every coordinate
    std::vector<std::string> colFiles;  // .col archives loaded into permanent collision slots
    std::string namesFile;              // importer's names.txt: new name, original name, why - for the log
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
    int colModelPoolLimit;                 // CPools::ms_pColModelPool capacity; < 0 = vanilla + what the regions need
    int txdPoolLimit;                      // CTxdStore's dictionary pool; < 0 = vanilla + the regions' dictionary names
    int buildingPoolLimit, dummyPoolLimit; // CPools' entity pools; < 0 = vanilla + the regions' inst lines
    int ptrNodePoolLimit;                  // CPools' sector pointer-node pool; < 0 = vanilla + what the regions need
    bool extendWorldSectors;               // double what the static world grids cover, so a region beyond +/-3000 renders
    bool linkRegionLods;                   // rebuild HD->LOD links from the source game's naming rule; off = LODs draw as plain objects
    std::vector<int> skipRenderModels;     // remapped model ids the render guard refuses to draw (a crash workaround while a model is diagnosed)
    bool debug;                      // verbose log, F8 diagnostics, water render statistics
};

namespace Settings {
    extern std::vector<MapRegion> regions;
    extern WorldSettings world;
    void Load();
    // Bounding box of the vanilla map plus every region's tile grid, in SA world coordinates.
    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY);
    // Same, without the pause map's margin or custom bounds: what the world grid has to reach.
    void RegionExtent(float& minX, float& maxX, float& minY, float& maxY);
    // True when this process was started by an SA-MP launcher. samp.dll is injected after ASI
    // plugins have already run, so its presence cannot be relied on this early; the launcher's
    // command line (-c -h <host> -p <port> -n <nick>) can.
    bool RunningUnderSaMp();
}
