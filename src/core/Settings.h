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
    std::string objectDataFile;         // SA-format object.dat: which of the region's models are
                                        // dynamic objects rather than static scenery, and how they
                                        // break. Empty leaves every inst line a building.
    std::string namesFile;              // importer's names.txt: new name, original name, why - for the log
    std::string zoneInfoLabel;          // existing zone whose CZoneInfo slot region zones share
    std::string regionName;             // area name over the whole tile grid, empty for none

    float originX, originY;   // SA world coords of the region's local (0,0)
    float originZ;            // added to every region height (water levels, zone boxes, IPL/inst z)
    float tileSize;           // world units per tile
    float gridMinX;           // local X of the west edge of column 0
    float gridMaxY;           // local Y of the north edge of row 0
    int tilesX, tilesY;

    // The ground the region's `inst` lines actually stand on, in the region's own coordinates,
    // measured from its IPLs at load. This is what the world sector grid has to reach and what
    // packing moves about; the tile grid above is a radar artefact and is routinely bigger (Vice
    // City declares 4000 x 4000 of radar and occupies 3436 x 3681 of it), so fitting the grid to
    // the tiles rather than the buildings buys a bigger sector for nothing. False when the region
    // has no readable IPL - under SA-MP the server places the buildings - and the tile grid is
    // then all there is to go on.
    bool  hasContent = false;
    float contentMinX = 0.0f, contentMaxX = 0.0f, contentMinY = 0.0f, contentMaxY = 0.0f;

    float TileWest(int col) const  { return originX + gridMinX + col * tileSize; }
    float TileNorth(int row) const { return originY + gridMaxY - row * tileSize; }

    // The same box in SA world coordinates, falling back to the tile grid when nothing was read.
    float ContentWest() const  { return hasContent ? originX + contentMinX : TileWest(0); }
    float ContentEast() const  { return hasContent ? originX + contentMaxX : TileWest(tilesX); }
    float ContentSouth() const { return hasContent ? originY + contentMinY : TileNorth(tilesY); }
    float ContentNorth() const { return hasContent ? originY + contentMaxY : TileNorth(0); }

    std::vector<std::string> zoneFiles;  // III/VC .zon or SA IPL files, in the region's own coords
    std::string zoneNamesFile;           // "GXTKEY = Display Name" lines; empty if none
    std::string weaponFile;              // importer's weapons.dat: the source game's guns over SA's weapon types
    std::string pickupFile;              // importer's pickups.dat: the world pickups its main script placed
    std::string pedNodeFile;             // importer's pednodes.dat: the source game's pedestrian path graph
    std::string pedGroupFile;            // importer's pedgrp.dat: who walks in which of its districts
    std::string carNodeFile;             // importer's carnodes.dat: the source game's carriageways
    std::string parkedCarFile;           // importer's parkedcars.dat: where its own script parked a car

    // Interior<N> lines from this region's ini section, in the region's own coordinates. Several
    // may name the same area: an interior split over rooms that are not one box (Vercetti's house
    // and its tower) needs one box each, and any of them makes the whole area visible.
    std::vector<InteriorVolume> interiors;
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
    bool fitSectorsToContent;              // fit the grid to where the buildings are rather than to the radar tile grids
    float sectorContentMargin;             // world units of slack left round the measured content box
    bool packRegions;                      // move each region up against the map instead of leaving it where its ini put it
    float regionGap;                       // world units of clear ground packing leaves between a region and its neighbour
    bool mapWindow;                        // fit the 120 cells to the map the player is on and re-file on crossing
    float mapWindowMargin;                 // how far past a map's edge the player goes before the window follows
    float streamDistance;                  // world units the near-field streaming pass asks for; < 0 auto, 0 leaves San Andreas alone
    float streamRefreshStep;               // world units the player moves before the wide pass runs again; < 0 half the distance, 0 every frame
    bool linkRegionLods;                   // rebuild HD->LOD links from the source game's naming rule; off = LODs draw as plain objects
    bool dropUnlinkedLods;                 // leave out LODs no HD hides - orphans and whole-district island meshes - which SA would draw through the real geometry
    std::vector<int> skipRenderModels;     // remapped model ids the render guard refuses to draw (a crash workaround while a model is diagnosed)
    bool regionWeapons;                    // let a region's weapons.dat give San Andreas its guns
    int regionWeaponMode;                  // 0 Add: new weapon types, SA's own untouched; 1 Replace: written over SA's
    bool regionPickups;                    // place the world pickups a region's pickups.dat lists
    int pickupReserve;                     // CPickup slots left for San Andreas' own script, of its 620
    bool regionPeds;                       // walk pedestrians on a region's own pednodes.dat graph
    float regionPedDensity;                // multiplier on how many of them, 1.0 being San Andreas' own
    float regionPedRadius;                 // world units around the player region peds live in
    bool regionPolice;                     // answer a wanted level in a region with cops on foot
    bool regionTraffic;                    // drive cars on a region's own carriageways, in our own memory
    float regionTrafficDensity;            // multiplier on how many, 1.0 being what San Andreas asks for
    float regionTrafficRadius;             // world units around the player traffic lives in
    bool regionParkedCars;                 // stand cars in the kerbside spaces a region's own script used
    float regionParkedCarsDensity;         // multiplier on the ceiling of how many exist at once
    float regionParkedCarsRadius;          // world units around the player a space is worth filling in
    float regionParkedCarsRespawn;         // seconds a space the player emptied stays empty; 0 = for ever
    bool regionPaths;                      // put a region's own carriageways into CPathFind, i.e. traffic
    bool widenPathNodes;                   // repoint the path coordinate scale so a node reaches a region
    float pathNodeScale;                   // int16 units per world unit; <= 0 = derive from the world
    bool debug;                      // verbose log, F8 diagnostics, water render statistics
};

namespace Settings {
    extern std::vector<MapRegion> regions;
    extern WorldSettings world;
    void Load();
    // Bounding box of the vanilla map plus every region's tile grid, in SA world coordinates.
    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY);
    // Same, without the pause map's margin or custom bounds: what the world grid has to reach.
    // Under FitSectorsToContent this is the ground the regions' inst lines actually stand on
    // rather than the radar tile grids they declare, which is smaller and so cheaper per scan.
    void RegionExtent(float& minX, float& maxX, float& minY, float& maxY);
    // The box one region's entities occupy, in SA world coordinates - what MapWindow fits the
    // grid to when the player is standing in that region. Index -1 is San Andreas' own +/-3000.
    void MapExtent(int index, float& minX, float& maxX, float& minY, float& maxY);
    // How many maps there are, San Andreas included, i.e. the valid range for MapExtent.
    int MapCount();
    // True when this process was started by an SA-MP launcher. samp.dll is injected after ASI
    // plugins have already run, so its presence cannot be relied on this early; the launcher's
    // command line (-c -h <host> -p <port> -n <nick>) can.
    bool RunningUnderSaMp();
}
