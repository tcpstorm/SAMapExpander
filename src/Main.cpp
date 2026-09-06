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
#include "AlphaListGuard.h"
#include "Gxt.h"
#include "Zones.h"
#include "ModelLimits.h"

using namespace plugin;

class MapExpander {
public:
    MapExpander() {
        Log::Open();
        CrashLog::Install();
        Settings::Load();

        ModelLimits::Install();   // must move the streaming arrays before the game initialises them
        Gxt::Install();
        if (!Settings::regions.empty()) {
            RadarOverlay::Install();
            Zones::Install();
            if (Settings::world.extendPauseMap) MenuMapFix::Install();
            Water::Install();
            WaterRender::Install();
            CityLoader::Install();
            // Only when this process is the one placing the region: under SA-MP the server owns
            // the objects and already handles distant ones, so the world grid is left alone.
            if (CityLoader::PlacesOwnEntities() && Settings::world.extendWorldSectors)
                WorldSectors::Install();
        }
        if (Settings::world.fixPickupPositions || Settings::world.fixCarGeneratorPositions) PickupFix::Install();
        if (Settings::world.fixMeleeBeyondMap) MeleeFix::Install();
        IplLoader::Install();
        DebugKeys::Install();
        Teleport::Install();
        Watchdog::Install();
        TxdProbe::Install();
        AlphaListGuard::Install();

        Log::Write("MapExpander initialised: %u region(s)", (unsigned)Settings::regions.size());
    }
} mapExpander;
