#include <plugin.h>
#include <cctype>
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

    static std::vector<std::string> SplitPaths(const std::string& list) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i <= list.size()) {
            size_t end = list.find(',', i);
            if (end == std::string::npos) end = list.size();
            size_t a = list.find_first_not_of(" \t", i);
            size_t b = list.find_last_not_of(" \t", end ? end - 1 : 0);
            if (a != std::string::npos && a < end && b != std::string::npos && b >= a)
                out.push_back(ResolveGamePath(list.substr(a, b - a + 1)));
            i = end + 1;
        }
        return out;
    }

    void Load() {
        regions.clear();
        world = { true, true, 4.0f, true, 1500.0f, false, 0.0f, 0.0f, 0.0f, 0.0f };

        IniFile ini;
        if (!ini.Load(PLUGIN_PATH("MapExpander.ini"))) {
            Log::Write("MapExpander.ini not found next to the plugin; nothing to do");
            return;
        }

        if (auto* w = ini.Find("World")) {
            world.fixPickupPositions = w->GetBool("FixPickupPositions", true);
            world.fixCarGeneratorPositions = w->GetBool("FixCarGeneratorPositions", true);
            world.compressedPositionScale = w->GetFloat("CompressedPositionScale", 4.0f);
            world.extendPauseMap = w->GetBool("ExtendPauseMap", true);
            world.pauseMapMargin = w->GetFloat("PauseMapMargin", 1500.0f);
            world.pauseMapCustomBounds = w->Has("PauseMapMinX") && w->Has("PauseMapMaxX") &&
                                         w->Has("PauseMapMinY") && w->Has("PauseMapMaxY");
            world.pauseMapMinX = w->GetFloat("PauseMapMinX", 0.0f);
            world.pauseMapMaxX = w->GetFloat("PauseMapMaxX", 0.0f);
            world.pauseMapMinY = w->GetFloat("PauseMapMinY", 0.0f);
            world.pauseMapMaxY = w->GetFloat("PauseMapMaxY", 0.0f);
            if (world.compressedPositionScale <= 0.0f) world.compressedPositionScale = 4.0f;
        }

        for (auto& s : ini.Sections()) {
            if (s.name.empty() || s.name == "World" || s.name == "General") continue;

            MapRegion r;
            r.name = s.name;
            r.tileFolder = ResolveGamePath(s.GetString("TileFolder", "MapExpander\\tiles\\" + s.name));
            r.filePattern = s.GetString("FilePattern", "tile_%d_%d.png");
            std::string water = s.GetString("Water", "");
            r.waterFile = water.empty() ? std::string() : ResolveGamePath(water);
            r.waterXOffset = s.GetFloat("WaterXOffset", 0.0f);
            r.tileSize = s.GetFloat("TileSize", 500.0f);
            r.tilesX = s.GetInt("TilesX", 0);
            r.tilesY = s.GetInt("TilesY", 0);
            r.iplFiles = SplitPaths(s.GetString("Ipl", ""));
            r.zoneFiles = SplitPaths(s.GetString("Zones", ""));
            if (s.Has("ZoneNames")) r.zoneNamesFile = ResolveGamePath(s.GetString("ZoneNames", ""));

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
