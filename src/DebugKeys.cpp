// Two keys in-game, both with [World] Debug=1. F8 logs the player's SA position, the equivalent
// region-local coordinates (used to calibrate WorldOriginX/Y against a known landmark), and which
// zone the game's own lookups resolve there. F7 teleports to the middle of the first region, which
// is the only practical way to go and look at one. The per-frame hook here also drives the GXT
// table check.
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
static bool gTeleportWasDown = false;

// CStreaming::LoadScene, so the region is actually in memory when we arrive rather than a few
// seconds later; the streamer will not have been anywhere near these coordinates before.
typedef void(__cdecl* LoadSceneFn)(const CVector*);
static const LoadSceneFn CallLoadScene = reinterpret_cast<LoadSceneFn>(0x40EB70);

// CPed::Teleport, by address. Teleport is virtual, and plugin-sdk's idea of where it sits in the
// vtable does not match this executable - dispatching through it lands on a null slot and the game
// jumps to address 0, which is a crash with no module and no clue in it. Call the override the
// game itself uses for peds instead.
typedef void(__thiscall* PedTeleportFn)(void*, CVector, bool);
static const PedTeleportFn CallPedTeleport = reinterpret_cast<PedTeleportFn>(0x5E4110);

// F7: stand in the middle of the first region. There is no other way to get out there to look at
// one, and walking is 3000 units of ocean.
//
// Teleport walks the player's RenderWare frame, so it cannot be called while the player has no
// RwObject - during the opening cutscene, for one, where the ped on screen is not this one. That is
// a null dereference deep inside the game with nothing in the log to say why, so refuse early
// instead, and log either side of each call so a crash here says which one it was.
static void TeleportToRegion() {
    if (Settings::regions.empty()) return;
    const MapRegion& r = Settings::regions.front();
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Log::Write("F7: no player ped yet");
        return;
    }
    if (!player->m_pRwObject) {
        Log::Write("F7: the player has no model right now (a cutscene, or still loading) - "
                   "not teleporting; try again once you have control");
        return;
    }

    CVector pos(r.originX, r.originY, 60.0f);
    Log::Write("F7: teleporting to [%s] at SA (%.1f, %.1f, %.1f)", r.name.c_str(),
               pos.x, pos.y, pos.z);
    CallPedTeleport(player, pos, false);
    Log::Write("F7: moved, streaming the scene there");
    CallLoadScene(&pos);
    Log::Write("F7: arrived");

    char msg[128];
    snprintf(msg, sizeof(msg), "MapExpander: %s (%.0f, %.0f)", r.name.c_str(), pos.x, pos.y);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);
}

static void OnFrame() {
    if (!Settings::world.debug) return;

    bool teleport = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (teleport && !gTeleportWasDown) TeleportToRegion();
    gTeleportWasDown = teleport;

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
        if (!FrameHook()) Log::Write("could not hook CTimer::Update; the per-frame GXT table check and F8 diagnostics are unavailable");
    }
}
