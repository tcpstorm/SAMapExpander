// F8 in-game: log the player's SA position, the equivalent region-local coordinates (used to
// calibrate WorldOriginX/Y against a known landmark), and which zone the game's own lookups resolve
// there.
#include <plugin.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <CMessages.h>
#include <safetyhook.hpp>
#include <windows.h>
#include "DebugKeys.h"
#include "Settings.h"
#include "Zones.h"
#include "Gxt.h"
#include "Log.h"

using namespace plugin;

constexpr uintptr_t kTimerUpdate = 0x561B10;  // CTimer::Update, once per frame from the main loop

// Install() runs during the DLL's static initialisation, before this file's globals are
// constructed (DebugKeys.o sorts before Main.o), so the hook object is a function-local static.
static SafetyHookInline& FrameHook() { static SafetyHookInline h; return h; }
static bool gWasDown = false;

static void OnFrame() {
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
    Zones::LogPlayerZone(pos.x, pos.y, pos.z);
}

static void __cdecl Hooked_TimerUpdate() {
    FrameHook().ccall<void>();
    Gxt::EnsureInjected();   // the one per-frame hook we have that fires under SA-MP too
    OnFrame();
}

namespace DebugKeys {
    void Install() {
        // plugin-sdk's events never fire under SA-MP, so poll from a function the game itself calls.
        FrameHook() = safetyhook::create_inline(reinterpret_cast<void*>(kTimerUpdate),
                                                reinterpret_cast<void*>(&Hooked_TimerUpdate));
        if (!FrameHook()) Log::Write("could not hook CTimer::Update; F8 diagnostics and the per-frame GXT table check are unavailable");
    }
}
