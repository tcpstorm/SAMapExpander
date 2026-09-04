// F8 in-game: log the player's SA position and, for every region, the equivalent region-local
// coordinates. Used to calibrate WorldOriginX/Y against a known landmark.
#include <plugin.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <CMessages.h>
#include <windows.h>
#include "DebugKeys.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

static bool gWasDown = false;

static void OnGameProcess() {
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    bool justPressed = down && !gWasDown;
    gWasDown = down;
    if (!justPressed) return;

    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    CVector pos = player->GetPosition();

    char msg[128];
    snprintf(msg, sizeof(msg), "MapExpander: SA %.1f, %.1f, %.1f (see log)", pos.x, pos.y, pos.z);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);

    Log::Write("F8: player at SA (%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
    for (auto& r : Settings::regions)
        Log::Write("    [%s] local (%.1f, %.1f)", r.name.c_str(), pos.x - r.originX, pos.y - r.originY);
}

namespace DebugKeys {
    void Install() {
        Events::gameProcessEvent += OnGameProcess;
    }
}
