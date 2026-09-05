#include <plugin.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
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
        world.buildingPoolLimit = -1;      // vanilla 13000 plus the regions' inst lines
        world.dummyPoolLimit = -1;         // vanilla 2500, same
        world.ptrNodePoolLimit = -1;       // vanilla 70000 plus what the regions need

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
            world.buildingPoolLimit = w->GetInt("BuildingPoolLimit", -1);
            world.dummyPoolLimit = w->GetInt("DummyPoolLimit", -1);
            world.ptrNodePoolLimit = w->GetInt("PtrNodePoolLimit", -1);
            if (world.compressedPositionScale <= 0.0f) world.compressedPositionScale = 4.0f;
            world.gxtFiles = SplitPaths(w->GetString("Gxt", ""));
            world.gxtLogMisses = w->GetBool("GxtLogMisses", false);
            world.modelIdLimit = w->GetInt("ModelIdLimit", 25000);
        }

        for (auto& s : ini.Sections()) {
            if (s.name.empty() || s.name == "World" || s.name == "General") continue;

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
            r.tileSize = s.GetFloat("TileSize", 500.0f);
            r.tilesX = s.GetInt("TilesX", 0);
            r.tilesY = s.GetInt("TilesY", 0);
            r.iplFiles = SplitPaths(s.GetString("Ipl", ""));
            r.zoneFiles = SplitPaths(s.GetString("Zones", ""));
            if (s.Has("ZoneNames")) r.zoneNamesFile = ResolveGamePath(s.GetString("ZoneNames", ""));
            r.zoneInfoLabel = s.GetString("ZoneInfoLabel", "SAN_AND");
            r.regionName = s.GetString("RegionName", "");

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

    void WorldExtent(float& minX, float& maxX, float& minY, float& maxY) {
        if (world.pauseMapCustomBounds) {
            minX = world.pauseMapMinX; maxX = world.pauseMapMaxX;
            minY = world.pauseMapMinY; maxY = world.pauseMapMaxY;
            return;
        }
        minX = -3000.0f; maxX = 3000.0f; minY = -3000.0f; maxY = 3000.0f;
        for (auto& r : regions) {
            if (r.TileWest(0) < minX) minX = r.TileWest(0);
            if (r.TileWest(r.tilesX) > maxX) maxX = r.TileWest(r.tilesX);
            if (r.TileNorth(0) > maxY) maxY = r.TileNorth(0);
            if (r.TileNorth(r.tilesY) < minY) minY = r.TileNorth(r.tilesY);
        }
        minX -= world.pauseMapMargin; maxX += world.pauseMapMargin;
        minY -= world.pauseMapMargin; maxY += world.pauseMapMargin;
    }
}
