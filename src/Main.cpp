#include <plugin.h>
#include "Log.h"
#include "Settings.h"
#include "RadarOverlay.h"
#include "MenuMapFix.h"
#include "PickupFix.h"
#include "Water.h"
#include "IplLoader.h"
#include "DebugKeys.h"
#include "Gxt.h"
#include "Zones.h"

using namespace plugin;

class MapExpander {
public:
    MapExpander() {
        Log::Open();
        Settings::Load();

        Gxt::Install();
        if (!Settings::regions.empty()) {
            RadarOverlay::Install();
            Zones::Install();
            if (Settings::world.extendPauseMap) MenuMapFix::Install();
            Water::Install();
        }
        if (Settings::world.fixPickupPositions || Settings::world.fixCarGeneratorPositions) PickupFix::Install();
        IplLoader::Install();
        DebugKeys::Install();

        Log::Write("MapExpander initialised: %u region(s)", (unsigned)Settings::regions.size());
    }
} mapExpander;
