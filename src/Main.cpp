#include <plugin.h>
#include "Log.h"
#include "Settings.h"
#include "RadarOverlay.h"
#include "MenuMapFix.h"
#include "PickupFix.h"
#include "DebugKeys.h"

using namespace plugin;

class MapExpander {
public:
    MapExpander() {
        Log::Open();
        Settings::Load();

        if (!Settings::regions.empty()) {
            RadarOverlay::Install();
            if (Settings::world.extendPauseMap) MenuMapFix::Install();
        }
        if (Settings::world.fixPickupPositions || Settings::world.fixCarGeneratorPositions) PickupFix::Install();
        DebugKeys::Install();

        Log::Write("MapExpander initialised: %u region(s)", (unsigned)Settings::regions.size());
    }
} mapExpander;
