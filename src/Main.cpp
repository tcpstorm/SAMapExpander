#include <plugin.h>
#include "Log.h"
#include "Settings.h"
#include "RadarOverlay.h"
#include "MenuMapFix.h"
#include "PickupFix.h"
#include "MeleeFix.h"
#include "Water.h"
#include "IplLoader.h"
#include "WaterRender.h"
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
            WaterRender::Install();
        }
        if (Settings::world.fixPickupPositions || Settings::world.fixCarGeneratorPositions) PickupFix::Install();
        if (Settings::world.fixMeleeBeyondMap) MeleeFix::Install();
        IplLoader::Install();
        DebugKeys::Install();

        Log::Write("MapExpander initialised: %u region(s)", (unsigned)Settings::regions.size());
    }
} mapExpander;
