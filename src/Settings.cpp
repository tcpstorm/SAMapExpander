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
        world.linkRegionLods = true;
        world.dropUnlinkedLods = true;
        world.regionWeapons = true;
        world.regionPickups = true;
        world.pickupReserve = 200;

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
            world.linkRegionLods = w->GetBool("LinkRegionLods", true);
            world.dropUnlinkedLods = w->GetBool("DropUnlinkedLods", true);
            world.regionWeapons = w->GetBool("RegionWeapons", true);
            world.regionPickups = w->GetBool("RegionPickups", true);
            world.pickupReserve = w->GetInt("PickupReserve", 200);
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
