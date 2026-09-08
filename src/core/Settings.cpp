#include <plugin.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "Settings.h"
#include "Ini.h"
#include "Log.h"

using namespace plugin;

namespace Settings {
    std::vector<MapRegion> regions;
    WorldSettings world;

    static std::string ResolveGamePath(const std::string& relative) {
        if (relative.size() > 1 && (relative[1] == ':' || relative[0] == '\\' || relative[0] == '/'))
            return relative;
        return GAME_PATH(relative.c_str());
    }

    static bool SameFile(const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }

    // The region.ini of every region folder that has one, in name order. The importer writes one
    // as it builds a region, so this is how a freshly imported city is found without anyone
    // editing MapExpander.ini - see the note at the discovery loop below.
    static std::vector<std::string> RegionIniPaths(const std::string& root) {
        std::vector<std::string> out;
        WIN32_FIND_DATAA found;
        HANDLE search = FindFirstFileA((root + "\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return out;
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (found.cFileName[0] == '.') continue;   // . and ..
            std::string path = root + "\\" + found.cFileName + "\\region.ini";
            DWORD attributes = GetFileAttributesA(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                out.push_back(path);
        } while (FindNextFileA(search, &found));
        FindClose(search);
        std::sort(out.begin(), out.end());
        return out;
    }

    static void AddPath(std::vector<std::string>& out, const std::string& path) {
        for (const std::string& have : out)
            if (SameFile(have, path)) return;   // listed twice, or by name and again by a wildcard
        out.push_back(path);
    }

    // A comma separated list of game-relative paths. An entry whose file name holds * or ? is
    // expanded to every match in that folder, in name order, so a city's thirty IDEs can be
    // "maps\*.ide"; list the ones whose order matters first, they are not repeated.
    static std::vector<std::string> SplitPaths(const std::string& list) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i <= list.size()) {
            size_t end = list.find(',', i);
            if (end == std::string::npos) end = list.size();
            size_t a = list.find_first_not_of(" \t", i);
            size_t b = list.find_last_not_of(" \t", end ? end - 1 : 0);
            i = end + 1;
            if (a == std::string::npos || a >= end || b == std::string::npos || b < a) continue;

            std::string path = ResolveGamePath(list.substr(a, b - a + 1));
            if (path.find_first_of("*?") == std::string::npos) { AddPath(out, path); continue; }

            size_t slash = path.find_last_of("\\/");
            std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
            std::vector<std::string> matches;
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(path.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) matches.push_back(folder + fd.cFileName);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            std::sort(matches.begin(), matches.end(),
                      [](const std::string& x, const std::string& y) { return _stricmp(x.c_str(), y.c_str()) < 0; });
            if (matches.empty()) Log::Write("%s matches no file", path.c_str());
            for (const std::string& m : matches) AddPath(out, m);
        }
        return out;
    }

    // "Interior<N> = area, minX, minY, minZ, maxX, maxY, maxZ [, name]" - seven numbers and an
    // optional label, the numbers in the region's own coordinates. Parsed by hand rather than
    // through GetFloat because the whole box is one ini value; a line that does not hold seven
    // numbers is reported and dropped instead of silently becoming a box at the origin.
    static bool ReadInteriorVolume(const std::string& value, InteriorVolume& out) {
        std::vector<std::string> fields;
        size_t at = 0;
        while (true) {
            size_t end = value.find(',', at);
            fields.push_back(value.substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos) break;
            at = end + 1;
        }
        if (fields.size() < 7) return false;
        float n[7];
        for (int i = 0; i < 7; i++) {
            const char* start = fields[i].c_str();
            char* stop = nullptr;
            n[i] = strtof(start, &stop);
            if (stop == start) return false;
        }
        out.area = (int)n[0];
        out.minX = n[1]; out.minY = n[2]; out.minZ = n[3];
        out.maxX = n[4]; out.maxY = n[5]; out.maxZ = n[6];
        out.name.clear();
        if (fields.size() > 7) {
            for (size_t i = 7; i < fields.size(); i++) out.name += (i > 7 ? "," : "") + fields[i];
            size_t a = out.name.find_first_not_of(" \t");
            size_t b = out.name.find_last_not_of(" \t");
            out.name = a == std::string::npos ? std::string() : out.name.substr(a, b - a + 1);
        }
        // A box written back to front would never contain anything; sort the corners rather than
        // making the reader remember which end goes first.
        if (out.minX > out.maxX) std::swap(out.minX, out.maxX);
        if (out.minY > out.maxY) std::swap(out.minY, out.maxY);
        if (out.minZ > out.maxZ) std::swap(out.minZ, out.maxZ);
        return true;
    }

    // ------------------------------------------------------------------ packing regions together
    //
    // San Andreas files every static entity into a grid of exactly 120 x 120 sectors, and that
    // count is fixed: the index that reaches the array is clamped with `cmp reg, imm8`, three bytes
    // with a sign-extended immediate, at every one of the 136 sites that do it, so the largest
    // index the code can express is 127 and the grid can never be more than 128 a side
    // (tools/sector_array_sites.txt is the whole enumeration). What WorldSectors can do is stretch
    // those 120 cells over whatever ground is in use - and the wider that ground, the bigger each
    // cell, and the more entities every per-frame scan over a cell walks. The cost is the square of
    // the cell: sectors of 128 units hold 6.5 times what a vanilla 50-unit one does.
    //
    // So the one thing left that decides the frame cost of a region is how far apart the maps are,
    // and that is a placement question, not an engine one. This closes the gap: each region is
    // moved up against the ground already in use, on whichever of the four sides leaves the world
    // squarest, and the world grid then has less to cover.
    //
    // Choosing the side is what earns the most. Liberty City off the west coast with Vice City to
    // the east spreads the world over 15348 units on one axis and only 6052 on the other, and the
    // grid is square because one pair of constants serves both, so it pays for the wider one twice:
    // 128-unit sectors. The same city to the north sits alongside Vice City instead of opposite it
    // and the world is 10500 a side - 88-unit sectors, less than half the scan cost, for a city
    // that is exactly as far from Los Santos either way.
    //
    // Off by default, because it moves the map out from under anything that remembers where it was:
    // pickups and parked cars in a savegame, screenshots, teleport coordinates, and an SA-MP
    // server's idea of where the region's buildings are. The log says what it would save so the
    // choice can be made on a number.
    void PackRegions(bool apply) {
        if (regions.empty()) return;

        struct Box { float x0, x1, y0, y1; };
        auto boxOf = [](const MapRegion& r) {
            return Box{ r.TileWest(0), r.TileWest(r.tilesX), r.TileNorth(r.tilesY), r.TileNorth(0) };
        };

        // San Andreas' own map is the fixed centre everything else packs around.
        Box used{ -3000.0f, 3000.0f, -3000.0f, 3000.0f };
        Box loose = used;
        for (const MapRegion& r : regions) {
            Box b = boxOf(r);
            loose.x0 = b.x0 < loose.x0 ? b.x0 : loose.x0;
            loose.x1 = b.x1 > loose.x1 ? b.x1 : loose.x1;
            loose.y0 = b.y0 < loose.y0 ? b.y0 : loose.y0;
            loose.y1 = b.y1 > loose.y1 ? b.y1 : loose.y1;
        }

        const float gap = world.regionGap < 0.0f ? 0.0f : world.regionGap;
        std::vector<std::pair<float, float>> moved;   // the packed origin of each region, in order
        for (const MapRegion& r : regions) {
            Box b = boxOf(r);
            const float w = b.x1 - b.x0, h = b.y1 - b.y0;

            // Already inside the ground in use: leave it alone. Someone who drops a small map into
            // the middle of San Andreas means it.
            if (b.x0 >= used.x0 && b.x1 <= used.x1 && b.y0 >= used.y0 && b.y1 <= used.y1) {
                moved.push_back({ r.originX, r.originY });
                continue;
            }

            // Which side of San Andreas the author put it on, by the largest overhang. Only used to
            // break ties, so that a region that is already where packing would put it anyway stays
            // pointing the way it was written.
            const float over[4] = { b.x1 - 3000.0f, -3000.0f - b.x0, b.y1 - 3000.0f, -3000.0f - b.y0 };
            int authorSide = 0;
            for (int s = 1; s < 4; s++) if (over[s] > over[authorSide]) authorSide = s;

            // Try all four sides and keep the one that leaves the smallest square, since the grid
            // is square and pays for whichever axis is widest. Along the other axis the region is
            // free to sit anywhere, so try two: centred on San Andreas, which keeps a city looking
            // like it was put next to the map, and centred on everything placed so far, which is
            // what packs tightest once there is more than one region out there.
            float bestX = r.originX, bestY = r.originY, bestSpan = -1.0f;
            int bestSide = 0, bestAlign = 0;
            Box bestUsed = used;
            const float centre[2][2] = { { 0.0f, 0.0f },
                                         { (used.x0 + used.x1) * 0.5f, (used.y0 + used.y1) * 0.5f } };
            for (int align = 0; align < 2; align++)
            for (int side = 0; side < 4; side++) {
                Box p = b;
                switch (side) {
                    case 0: p.x0 = used.x1 + gap; p.y0 = centre[align][1] - h * 0.5f; break;      // east
                    case 1: p.x0 = used.x0 - gap - w; p.y0 = centre[align][1] - h * 0.5f; break;  // west
                    case 2: p.y0 = used.y1 + gap; p.x0 = centre[align][0] - w * 0.5f; break;      // north
                    default: p.y0 = used.y0 - gap - h; p.x0 = centre[align][0] - w * 0.5f; break; // south
                }
                p.x1 = p.x0 + w;
                p.y1 = p.y0 + h;
                Box u{ p.x0 < used.x0 ? p.x0 : used.x0, p.x1 > used.x1 ? p.x1 : used.x1,
                       p.y0 < used.y0 ? p.y0 : used.y0, p.y1 > used.y1 ? p.y1 : used.y1 };
                const float span = (u.x1 - u.x0) > (u.y1 - u.y0) ? (u.x1 - u.x0) : (u.y1 - u.y0);
                // Ties: the author's side first, then the San Andreas alignment, then the fixed
                // order east, west, north, south - so the same ini always produces the same world.
                const bool better = bestSpan < 0.0f || span < bestSpan - 0.5f ||
                    (span < bestSpan + 0.5f &&
                     ((side == authorSide && bestSide != authorSide) ||
                      (side == bestSide && align < bestAlign)));
                if (better) {
                    bestSpan = span;
                    bestSide = side;
                    bestAlign = align;
                    bestX = r.originX + (p.x0 - b.x0);
                    bestY = r.originY + (p.y0 - b.y0);
                    bestUsed = u;
                }
            }
            moved.push_back({ bestX, bestY });
            used = bestUsed;
        }

        const float looseSpan = (loose.x1 - loose.x0) > (loose.y1 - loose.y0)
                              ? (loose.x1 - loose.x0) : (loose.y1 - loose.y0);
        const float packedSpan = (used.x1 - used.x0) > (used.y1 - used.y0)
                               ? (used.x1 - used.x0) : (used.y1 - used.y0);
        // 120 is the sector count the grid has and cannot exceed by more than 8; see the comment
        // above and tools/sector_array_sites.txt.
        const float looseCell = looseSpan / 120.0f, packedCell = packedSpan / 120.0f;

        // Packing that does not shrink the world is not packing, it is just moving the map about;
        // a single region already tucked in closer than RegionGap would come out of the loop above
        // very slightly worse off. Leave it where the author put it.
        if (packedSpan >= looseSpan - 1.0f) {
            if (apply)
                Log::Write("regions: already as tight as packing would make them (%.0f units of "
                           "world a side, %.0f-unit sectors); left where their ini puts them",
                           looseSpan, looseCell);
            return;
        }

        if (!apply) {
            Log::Write("regions: left where their ini puts them, which needs %.0f units of world a "
                       "side and so %.0f-unit sectors. Packed against the map they would need "
                       "%.0f, giving %.0f-unit sectors and %.0f%% of the per-frame sector scan "
                       "cost%s",
                       looseSpan, looseCell, packedSpan, packedCell,
                       100.0f * (packedCell * packedCell) / (looseCell * looseCell),
                       world.packRegions ? "" : " ([World] PackRegions = 1 does it, and moves the "
                       "maps with everything that remembers where they were - savegame pickups and "
                       "parked cars, teleport coordinates, screenshots)");
            return;
        }

        for (size_t i = 0; i < regions.size(); i++) {
            MapRegion& r = regions[i];
            if (r.originX == moved[i].first && r.originY == moved[i].second) continue;
            Log::Write("[%s] packed: origin (%.1f, %.1f) -> (%.1f, %.1f)", r.name.c_str(),
                       r.originX, r.originY, moved[i].first, moved[i].second);
            r.originX = moved[i].first;
            r.originY = moved[i].second;
        }
        Log::Write("regions: packed against the map - the world is %.0f units a side instead of "
                   "%.0f, so the fixed 120x120 sector grid gets %.0f-unit cells instead of %.0f "
                   "and every per-frame sector scan walks %.0f%% of what it did",
                   packedSpan, looseSpan, packedCell, looseCell,
                   100.0f * (packedCell * packedCell) / (looseCell * looseCell));
    }

    void Load() {
        regions.clear();
        // Field by field: WorldSettings holds a std::vector, so a brace list would silently leave
        // every field after it at whatever the aggregate default is.
        world = {};
        world.fixPickupPositions = true;
        world.fixCarGeneratorPositions = true;
        world.compressedPositionScale = 4.0f;
        world.fixMeleeBeyondMap = true;
        world.extendPauseMap = true;
        world.pauseMapMargin = 1500.0f;
        world.extendWaterRendering = 1;
        world.modelIdLimit = 25000;
        world.freeModelIdStart = 18631;
        world.freeModelIdEnd = -1;         // up to ModelIdLimit - 1
        world.atomicModelInfoLimit = -1;   // the store's real capacity
        world.colModelPoolLimit = -1;      // vanilla 10150 plus the regions' own collision
        world.txdPoolLimit = -1;           // vanilla 5000 plus the regions' dictionary names
        world.buildingPoolLimit = -1;      // vanilla 13000 plus the regions' inst lines
        world.dummyPoolLimit = -1;         // vanilla 2500, same
        world.ptrNodePoolLimit = -1;       // vanilla 70000 plus what the regions need
        world.extendWorldSectors = true;
        world.packRegions = false;         // leave every region exactly where its ini puts it
        world.regionGap = 500.0f;
        world.linkRegionLods = true;
        world.dropUnlinkedLods = true;
        world.regionWeapons = true;
        world.regionWeaponMode = 0;        // Add: new weapon types, San Andreas' own left alone
        world.regionPickups = true;
        world.pickupReserve = 200;
        world.regionPeds = true;
        world.regionPedDensity = 1.0f;
        world.regionPedRadius = 55.0f;
        world.regionPolice = true;
        world.regionTraffic = true;
        world.regionTrafficDensity = 1.0f;
        world.regionTrafficRadius = 90.0f;
        world.regionPaths = false;         // see MapExpander.ini: this is the untested half
        world.widenPathNodes = true;
        world.pathNodeScale = 0.0f;        // derive from the world the regions actually make

        IniFile ini;
        if (!ini.Load(PLUGIN_PATH("MapExpander.ini"))) {
            Log::Write("MapExpander.ini not found next to the plugin; nothing to do");
            return;
        }

        if (auto* w = ini.Find("World")) {
            world.debug = w->GetBool("Debug", false);
            Log::SetDebug(world.debug);
            world.fixPickupPositions = w->GetBool("FixPickupPositions", true);
            world.fixCarGeneratorPositions = w->GetBool("FixCarGeneratorPositions", true);
            world.compressedPositionScale = w->GetFloat("CompressedPositionScale", 4.0f);
            world.fixMeleeBeyondMap = w->GetBool("FixMeleeBeyondMap", true);
            world.extendPauseMap = w->GetBool("ExtendPauseMap", true);
            world.pauseMapMargin = w->GetFloat("PauseMapMargin", 1500.0f);
            world.pauseMapCustomBounds = w->Has("PauseMapMinX") && w->Has("PauseMapMaxX") &&
                                         w->Has("PauseMapMinY") && w->Has("PauseMapMaxY");
            world.pauseMapMinX = w->GetFloat("PauseMapMinX", 0.0f);
            world.pauseMapMaxX = w->GetFloat("PauseMapMaxX", 0.0f);
            world.pauseMapMinY = w->GetFloat("PauseMapMinY", 0.0f);
            world.pauseMapMaxY = w->GetFloat("PauseMapMaxY", 0.0f);
            world.extendWaterRendering = w->GetInt("ExtendWaterRendering", 1);
            world.freeModelIdStart = w->GetInt("FreeModelIdStart", 18631);
            world.freeModelIdEnd = w->GetInt("FreeModelIdEnd", -1);   // -1: up to ModelIdLimit
            world.atomicModelInfoLimit = w->GetInt("AtomicModelInfoLimit", -1);
            world.colModelPoolLimit = w->GetInt("ColModelPoolLimit", -1);
            world.txdPoolLimit = w->GetInt("TxdPoolLimit", -1);
            world.buildingPoolLimit = w->GetInt("BuildingPoolLimit", -1);
            world.dummyPoolLimit = w->GetInt("DummyPoolLimit", -1);
            world.ptrNodePoolLimit = w->GetInt("PtrNodePoolLimit", -1);
            world.extendWorldSectors = w->GetBool("ExtendWorldSectors", true);
            world.packRegions = w->GetBool("PackRegions", false);
            world.regionGap = w->GetFloat("RegionGap", 500.0f);
            world.linkRegionLods = w->GetBool("LinkRegionLods", true);
            world.dropUnlinkedLods = w->GetBool("DropUnlinkedLods", true);
            world.regionWeapons = w->GetBool("RegionWeapons", true);
            // Add gives the region's guns weapon types of their own and leaves San Andreas' alone;
            // Replace writes them over the nearest San Andreas weapon, which changes that weapon
            // everywhere. Anything else reads as Add, because Add is the one that cannot surprise.
            world.regionWeaponMode =
                _stricmp(w->GetString("RegionWeaponMode", "Add").c_str(), "Replace") == 0 ? 1 : 0;
            world.regionPickups = w->GetBool("RegionPickups", true);
            world.pickupReserve = w->GetInt("PickupReserve", 200);
            world.regionPeds = w->GetBool("RegionPeds", true);
            world.regionPedDensity = w->GetFloat("RegionPedDensity", 1.0f);
            world.regionPedRadius = w->GetFloat("RegionPedRadius", 55.0f);
            world.regionPolice = w->GetBool("RegionPolice", true);
            world.regionTraffic = w->GetBool("RegionTraffic", true);
            world.regionTrafficDensity = w->GetFloat("RegionTrafficDensity", 1.0f);
            world.regionTrafficRadius = w->GetFloat("RegionTrafficRadius", 90.0f);
            world.regionPaths = w->GetBool("RegionPaths", false);
            world.widenPathNodes = w->GetBool("WidenPathNodes", true);
            world.pathNodeScale = w->GetFloat("PathNodeScale", 0.0f);
            for (const std::string& s : SplitPaths(w->GetString("DebugSkipModels", ""))) world.skipRenderModels.push_back(atoi(s.c_str()));
            if (world.compressedPositionScale <= 0.0f) world.compressedPositionScale = 4.0f;
            world.gxtFiles = SplitPaths(w->GetString("Gxt", ""));
            world.gxtLogMisses = w->GetBool("GxtLogMisses", false);
            world.modelIdLimit = w->GetInt("ModelIdLimit", 25000);
        }

        // Take the regions the importer has already described, then anything the user has written
        // by hand.
        //
        // The importer writes a complete MapExpander\<region>\region.ini beside every region it
        // builds - archives, IDEs, IPLs, collision, names, object data, zones, interiors, the lot -
        // because it is the thing that knows what it just made. Reading those directly is what
        // makes the mod work from a retail install without the user editing anything: import a
        // game, run it, and the region is there. Nothing has to be copied into this file, which
        // also ends a whole class of bug, since every key the importer learned to write was one
        // more line that had to be pasted into an existing MapExpander.ini or silently do nothing.
        //
        // A section here still wins: it is loaded after, and a region of the same name replaces the
        // discovered one outright, so a hand-written override is still a hand-written override.
        std::vector<IniFile> regionInis;      // kept alive: the sections below point into these
        std::vector<const IniFile::Section*> described;
        for (auto& s : ini.Sections()) {
            if (s.name.empty() || s.name == "World" || s.name == "General") continue;
            described.push_back(&s);
        }
        for (const std::string& path : RegionIniPaths(ResolveGamePath("MapExpander"))) {
            regionInis.emplace_back();
            if (!regionInis.back().Load(path)) {
                Log::Write("region: %s could not be read", path.c_str());
                regionInis.pop_back();
                continue;
            }
            for (auto& s : regionInis.back().Sections()) {
                if (s.name.empty() || s.name == "World" || s.name == "General") continue;
                bool overridden = false;
                for (const IniFile::Section* have : described)
                    if (SameFile(have->name, s.name)) { overridden = true; break; }
                if (overridden) {
                    Log::Write("region: [%s] found in %s, but MapExpander.ini describes it; using "
                               "MapExpander.ini", s.name.c_str(), path.c_str());
                    continue;
                }
                described.push_back(&s);
                Log::Write("region: [%s] described by %s", s.name.c_str(), path.c_str());
            }
        }

        for (const IniFile::Section* sp : described) {
            const IniFile::Section& s = *sp;

            MapRegion r;
            r.name = s.name;
            r.tileFolder = ResolveGamePath(s.GetString("TileFolder", "MapExpander\\" + s.name + "\\tiles"));
            r.filePattern = s.GetString("FilePattern", "tile_%d_%d.png");
            std::string water = s.GetString("Water", "");
            r.waterFile = water.empty() ? std::string() : ResolveGamePath(water);
            r.waterXOffset = s.GetFloat("WaterXOffset", 0.0f);
            r.imgFiles = SplitPaths(s.GetString("Img", ""));
            r.ideFiles = SplitPaths(s.GetString("Ide", ""));
            r.colFiles = SplitPaths(s.GetString("Col", ""));
            r.namesFile = s.GetString("Names", "");
            if (!r.namesFile.empty()) r.namesFile = ResolveGamePath(r.namesFile);
            r.objectDataFile = s.GetString("ObjectData", "");
            if (!r.objectDataFile.empty()) r.objectDataFile = ResolveGamePath(r.objectDataFile);
            r.tileSize = s.GetFloat("TileSize", 500.0f);
            r.tilesX = s.GetInt("TilesX", 0);
            r.tilesY = s.GetInt("TilesY", 0);
            r.iplFiles = SplitPaths(s.GetString("Ipl", ""));
            r.zoneFiles = SplitPaths(s.GetString("Zones", ""));
            if (s.Has("ZoneNames")) r.zoneNamesFile = ResolveGamePath(s.GetString("ZoneNames", ""));
            if (s.Has("Weapons")) r.weaponFile = ResolveGamePath(s.GetString("Weapons", ""));
            if (s.Has("Pickups")) r.pickupFile = ResolveGamePath(s.GetString("Pickups", ""));
            if (s.Has("PedNodes")) r.pedNodeFile = ResolveGamePath(s.GetString("PedNodes", ""));
            if (s.Has("PedGroups")) r.pedGroupFile = ResolveGamePath(s.GetString("PedGroups", ""));
            if (s.Has("CarNodes")) r.carNodeFile = ResolveGamePath(s.GetString("CarNodes", ""));
            r.zoneInfoLabel = s.GetString("ZoneInfoLabel", "SAN_AND");
            r.regionName = s.GetString("RegionName", "");

            // Interior1, Interior2, ... An ini section is a map, so a gap in the numbering must not
            // stop the scan; go on to a fixed ceiling and take every key that is actually there.
            for (int i = 1; i <= 256; i++) {
                char key[32];
                snprintf(key, sizeof(key), "Interior%d", i);
                if (!s.Has(key)) continue;
                InteriorVolume v;
                if (!ReadInteriorVolume(s.GetString(key, ""), v)) {
                    Log::Write("[%s] %s: expected \"area, minX, minY, minZ, maxX, maxY, maxZ [, name]\"; skipped",
                               s.name.c_str(), key);
                    continue;
                }
                // 0 is the outside world and 13 is San Andreas' and Vice City's shared "everywhere"
                // code; neither is an interior, and switching to either would be a way to make the
                // whole map vanish. The area code itself is one byte (CEntity + 0x2F).
                if (v.area <= 0 || v.area > 255 || v.area == 13) {
                    Log::Write("[%s] %s: area %d is not an interior (0 is the outside world, 13 is "
                               "\"everywhere\", and the code is one byte); skipped",
                               s.name.c_str(), key, v.area);
                    continue;
                }
                r.interiors.push_back(v);
            }

            bool hasOrigin = s.Has("WorldOriginX") && s.Has("WorldOriginY");
            bool hasExtent = s.Has("WorldMinX") && s.Has("WorldMaxX") && s.Has("WorldMinY") && s.Has("WorldMaxY");
            if (hasOrigin) {
                r.originX = s.GetFloat("WorldOriginX", 0.0f);
                r.originY = s.GetFloat("WorldOriginY", 0.0f);
            } else if (hasExtent) {
                r.originX = (s.GetFloat("WorldMinX", 0.0f) + s.GetFloat("WorldMaxX", 0.0f)) * 0.5f;
                r.originY = (s.GetFloat("WorldMinY", 0.0f) + s.GetFloat("WorldMaxY", 0.0f)) * 0.5f;
            } else {
                Log::Write("[%s] needs WorldOriginX/Y or WorldMinX/MaxX/MinY/MaxY; skipped", s.name.c_str());
                continue;
            }

            r.originZ = s.GetFloat("WorldOriginZ", 0.0f);

            // Default grid: centred on the local origin, like SA's own radar (-3000..3000 for 12 tiles).
            r.gridMinX = s.GetFloat("GridMinX", -0.5f * r.tilesX * r.tileSize);
            r.gridMaxY = s.GetFloat("GridMaxY", 0.5f * r.tilesY * r.tileSize);

            if (r.tilesX <= 0 || r.tilesY <= 0 || r.tileSize <= 0.0f) {
                Log::Write("[%s] needs TilesX, TilesY and TileSize > 0; skipped", s.name.c_str());
                continue;
            }

            Log::Write("[%s] origin=(%.1f, %.1f) tiles=%dx%d size=%.0f grid local x>=%.0f y<=%.0f folder=%s",
                       r.name.c_str(), r.originX, r.originY, r.tilesX, r.tilesY, r.tileSize,
                       r.gridMinX, r.gridMaxY, r.tileFolder.c_str());
            for (auto& f : r.iplFiles) Log::Write("[%s] ipl: %s", r.name.c_str(), f.c_str());
            regions.push_back(r);
        }

        // Last, because it rewrites the origins every other module is about to read: the tiles, the
        // zones, the water, the interiors and every inst line all sit at origin + something. Never
        // under SA-MP, where the server places the region's buildings from its own coordinates and
        // a client that quietly moved the map would draw the radar somewhere the buildings are not.
        const bool underSaMp = RunningUnderSaMp();
        if (world.packRegions && underSaMp)
            Log::Write("regions: PackRegions is on, but under SA-MP the server places the region's "
                       "buildings from its own coordinates and a client that moved the map on its "
                       "own would draw the radar where the buildings are not; left as the ini says");
        PackRegions(world.packRegions && !underSaMp);
    }

    bool RunningUnderSaMp() {
        static int cached = -1;
        if (cached >= 0) return cached != 0;
        const char* why = nullptr;
        if (GetModuleHandleA("samp.dll")) why = "samp.dll is loaded";
        else {
            std::string cmd = GetCommandLineA();
            for (char& c : cmd) c = (char)tolower((unsigned char)c);
            bool host = cmd.find(" -h ") != std::string::npos;
            bool nick = cmd.find(" -n ") != std::string::npos;
            bool port = cmd.find(" -p ") != std::string::npos;
            if (host && (nick || port)) why = "started by the SA-MP launcher (-h/-p/-n on the command line)";
        }
        cached = why ? 1 : 0;
        if (why) Log::Write("SA-MP detected: %s", why);
        return cached != 0;
    }

    // The ground that has to be addressable: San Andreas' own +/-3000 and every region's tiles.
    // Deliberately without the pause map's margin and without its custom bounds - those say what
    // the map screen scrolls over, and paying for them in the world grid costs cell size, which
    // every sector scan pays for again on every frame.
    void RegionExtent(float& minX, float& maxX, float& minY, float& maxY) {
        minX = -3000.0f; maxX = 3000.0f; minY = -3000.0f; maxY = 3000.0f;
        for (auto& r : regions) {
            if (r.TileWest(0) < minX) minX = r.TileWest(0);
            if (r.TileWest(r.tilesX) > maxX) maxX = r.TileWest(r.tilesX);
            if (r.TileNorth(0) > maxY) maxY = r.TileNorth(0);
            if (r.TileNorth(r.tilesY) < minY) minY = r.TileNorth(r.tilesY);
        }
    }

    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY) {
        if (world.pauseMapCustomBounds) {
            minX = world.pauseMapMinX; maxX = world.pauseMapMaxX;
            minY = world.pauseMapMinY; maxY = world.pauseMapMaxY;
            return;
        }
        RegionExtent(minX, maxX, minY, maxY);
        minX -= world.pauseMapMargin; maxX += world.pauseMapMargin;
        minY -= world.pauseMapMargin; maxY += world.pauseMapMargin;
    }
}
