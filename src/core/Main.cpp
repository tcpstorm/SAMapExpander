#include <plugin.h>
#include "Log.h"
#include "CrashLog.h"
#include "Settings.h"
#include "RadarOverlay.h"
#include "MenuMapFix.h"
#include "PickupFix.h"
#include "MeleeFix.h"
#include "Water.h"
#include "IplLoader.h"
#include "WaterRender.h"
#include "CityLoader.h"
#include "WorldSectors.h"
#include "DebugKeys.h"
#include "Teleport.h"
#include "Watchdog.h"
#include "TxdProbe.h"
#include "RenderGuard.h"
#include "Gxt.h"
#include "Zones.h"
#include "Interiors.h"
#include "ModelLimits.h"
#include "PathScale.h"
#include "Fla.h"
#include <string>
#include "RegionWeapons.h"
#include "RegionPickups.h"
#include "RegionPeds.h"
#include "RegionPolice.h"
#include "RegionTraffic.h"
#include "RegionParkedCars.h"
#include "PickupSlots.h"

using namespace plugin;

class MapExpander {
public:
    MapExpander() {
        Log::Open();
        CrashLog::Install();
        Settings::Load();

        // Say once, at the top of the log, whether somebody else is already running the show. The
        // path system and the world sector grid are both things fastman92's limit adjuster does
        // properly and this plugin stands aside from, and knowing it is there explains a great
        // many later lines that would otherwise read as failures.
        if (Fla::Loaded()) {
            std::string version = Fla::Version(), paths;
            Fla::DescribePathEncoding(paths);
            Log::Write("fastman92's limit adjuster is loaded%s%s; paths: %s",
                       version.empty() ? "" : ", version ", version.c_str(), paths.c_str());
        }

        ModelLimits::Install();   // must move the streaming arrays before the game initialises them
        // Before anything reads a path node, which is everything about traffic. It only rewrites
        // operands, so it does not care what else has been installed - but it must be in place
        // before CPathFind loads its first area.
        PathScale::Install();
        Gxt::Install();
        if (!Settings::regions.empty()) {
            RadarOverlay::Install();
            Zones::Install();
            if (Settings::world.extendPauseMap) MenuMapFix::Install();
            Water::Install();
            WaterRender::Install();
            // Before CityLoader, which budgets a region's weap IDE lines against the weapon model
            // store this has just moved out of the way.
            RegionWeapons::Install();
            RegionPickups::Install();
            CityLoader::Install();
            // Only when this process is the one placing the region: under SA-MP the server owns
            // the objects and already handles distant ones, so the world grid is left alone.
            if (CityLoader::PlacesOwnEntities() && Settings::world.extendWorldSectors)
                WorldSectors::Install();
            Interiors::Install();
            // After CityLoader, because a region's pedestrians stand on the collision it places
            // and its first tick is a frame away either way.
            // Police before pedestrians: the pedestrian tick is what drives them, so it must not
            // start before they know whether they can task a ped at all.
            RegionPolice::Install();
            RegionTraffic::Install();
            RegionParkedCars::Install();
            RegionPeds::Install();
        }
        if (Settings::world.fixPickupPositions || Settings::world.fixCarGeneratorPositions) PickupFix::Install();
        if (Settings::world.fixMeleeBeyondMap) MeleeFix::Install();
        IplLoader::Install();
        DebugKeys::Install();
        Teleport::Install();
        Watchdog::Install();
        TxdProbe::Install();
        RenderGuard::Install();

        Log::Write("MapExpander initialised: %u region(s)", (unsigned)Settings::regions.size());
    }
} mapExpander;
