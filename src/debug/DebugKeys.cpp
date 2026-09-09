// F8 and F9, with [World] Debug=1. F8 logs the player's SA position, the equivalent region-local
// coordinates (used to calibrate WorldOriginX/Y against a known landmark), and which zone the
// game's own lookups resolve there. The per-frame hook here also drives the GXT table check.
//
// The other debug key, F7, lives in Teleport.cpp: it has to run from a different point in the
// frame, so it carries its own hook rather than riding on this one.
#include <plugin.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <CMessages.h>
#include <safetyhook.hpp>
#include <windows.h>
#include <string>
#include "DebugKeys.h"
#include "Settings.h"
#include "Zones.h"
#include "RegionPeds.h"
#include "RegionPolice.h"
#include "RegionTraffic.h"
#include "RegionParkedCars.h"
#include "StreamedModels.h"
#include "Ground.h"
#include "WeaponTypes.h"
#include "RegionWeapons.h"
#include "Interiors.h"
#include "Gxt.h"
#include "Watchdog.h"
#include "Log.h"

using namespace plugin;

constexpr uintptr_t kTimerUpdate = 0x561B10;  // CTimer::Update, once per frame from the main loop

// Install() runs during the DLL's static initialisation, before this file's globals are
// constructed (DebugKeys.o sorts before Main.o), so the hook object is a function-local static.
static SafetyHookInline& FrameHook() { static SafetyHookInline h; return h; }
static bool gWasDown = false;
static bool gGiveWasDown = false;

// CPed::GiveWeapon(type, ammo, flag). The flag is what the game passes when a weapon is meant to
// become the one in the ped's hands rather than merely sit in the inventory.
typedef int(__thiscall* GiveWeaponFn)(void* ped, int type, unsigned int ammo, bool select);
static const GiveWeaponFn CallGiveWeapon = reinterpret_cast<GiveWeaponFn>(0x5E6080);

// CStreaming::RequestModel and LoadAllRequestedModels - what makes a model exist before something
// tries to draw it. See the note in GiveRegionWeapons for why handing over a weapon needs them.
typedef void(__cdecl* RequestModelFn)(int model, int flags);
static const RequestModelFn CallRequestModel = reinterpret_cast<RequestModelFn>(0x4087E0);
typedef void(__cdecl* LoadAllRequestedModelsFn)(bool priorityOnly);
static const LoadAllRequestedModelsFn CallLoadAllRequestedModels =
    reinterpret_cast<LoadAllRequestedModelsFn>(0x40EA10);

// F9, with [World] Debug=1: hand over every gun a region added.
//
// The imported weapons are types of their own, so no cheat knows their names and the only way to
// get one is to find its pickup in the street - which is a long walk when what you want is to check
// that a scope works. This gives the lot, with ammo, in one press.
static void GiveRegionWeapons() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    if (!WeaponTypes::Available()) {
        CMessages::AddMessageJumpQ((char*)"MapExpander: no imported weapons in this game", 4000, 0, false);
        Log::Write("F9: no region weapon types were claimed, so there is nothing to give");
        return;
    }

    // Ask for every model first, and wait for them.
    //
    // A weapon picked up off the street is visible because the pickup itself streamed the model in
    // before anyone could hold it. GiveWeapon does no such thing: it writes the type into the ped's
    // inventory and leaves the model to whoever wants it, so a gun handed over this way arrives
    // with nothing to draw - invisible in the hands, and no icon either, since the icon is a
    // texture in the model's own dictionary and that dictionary comes with the model.
    const int imported = RegionWeapons::ImportedCount();
    int wanted = 0;
    for (int i = 0; i < imported; i++) {
        const int type = RegionWeapons::ImportedType(i);
        if (type < 0) continue;
        const int model = RegionWeapons::WeaponModel(type);
        if (model <= 0) continue;
        CallRequestModel(model, 0x16);   // the flags CPickups uses for a weapon it is about to show
        wanted++;
    }
    if (wanted) CallLoadAllRequestedModels(false);

    int given = 0;
    for (int i = 0; i < imported; i++) {
        const int type = RegionWeapons::ImportedType(i);
        // A type this never handed out has no record of its own and would arrive as whatever the
        // slot happened to hold, so only the ones with a donor - the ones actually claimed - count.
        if (type < 0) continue;
        CallGiveWeapon(player, type, 500, false);
        Log::Write("F9: gave weapon type %d (behaving as %d), model %d", type,
                   WeaponTypes::DonorOf(type), RegionWeapons::WeaponModel(type));
        given++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "MapExpander: %d imported weapon(s), 500 rounds each", given);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);
    Log::Write("F9: %d imported weapon(s) given", given);
}

static void OnFrame() {
    if (!Settings::world.debug) return;

    bool giveDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (giveDown && !gGiveWasDown) GiveRegionWeapons();
    gGiveWasDown = giveDown;

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
        Log::Write("    [%s] local (%.1f, %.1f, %.1f)", r.name.c_str(),
                   pos.x - r.originX, pos.y - r.originY, pos.z - r.originZ);
    // Every surface under the player's own feet, highest first, from well above their head.
    //
    // This is the one measurement that tells "the ped is under the map" apart from "the ped is on
    // the only floor there is". If the player is standing on a street and the list has two entries
    // twenty units apart, the spawners are choosing the wrong one; if it has one, they are choosing
    // the only one and the fault is elsewhere. Press F8 standing where the peds come out wrong.
    char surfaces[256] = {};
    Ground::Describe(pos.x, pos.y, pos.z + 300.0f, surfaces, sizeof(surfaces));
    Log::Write("    ground under the player: %s (the player's own z is %.2f)", surfaces, pos.z);
    // Stand on a road and press F8, near the region's origin and again at the far edge of the
    // city. A graph that is a scaled copy of the map is a few units out near the origin and tens
    // of units out at the edge, and no amount of watching the cars can show it - they follow their
    // own graph faithfully either way. Two readings settle it; one settles nothing.
    char roads[256] = {};
    RegionTraffic::DescribeNearestNode(pos, roads, sizeof(roads));
    Log::Write("    carriageways here: %s", roads);
    // The local coordinates above are the ones an Interior<N> box is written in, so F8 in a corner
    // of a room is how you find the numbers for one.
    std::string interior;
    Interiors::Describe(interior);
    Log::Write("    interior: %s", interior.c_str());
    Zones::LogPlayerZone(pos.x, pos.y, pos.z);
    // The population, which is the one thing that cannot be checked by looking at the log on
    // start-up: it says nothing until somebody is actually standing there.
    Log::Write("    peds: %d walking on the region's own graph, %d police out, %d car(s) driving "
               "its carriageways, %d parked at its kerbs; the player is %s a region's pedestrian "
               "graph", RegionPeds::Count(), RegionPolice::Count(), RegionTraffic::Count(),
               RegionParkedCars::Count(), RegionPeds::PlayerOverGraph() ? "over" : "not over");
    // Why there are none, when there are none. A budget of zero is San Andreas' own popcycle
    // saying so; no models in memory is the streaming array being read in the wrong place, which
    // is how every one of these went quiet once already; no armed bays in range is simply an empty
    // stretch of road. The three cannot be told apart from the counts above.
    int placeholders = 0;
    int loaded = RegionTraffic::LoadedModelCount(&placeholders);
    Log::Write("    population budgets: traffic %d car(s) from the zone's popcycle row, %d of %d "
               "placeholder model(s) in memory; parked cars %d at once, %d bay(s) armed and in "
               "range; %d model(s) still asked for and not yet handed back",
               RegionTraffic::Budget(), loaded, placeholders,
               RegionParkedCars::Budget(), RegionParkedCars::ArmedNear(pos), StreamedModels::Held());
}

static void __cdecl Hooked_TimerUpdate() {
    FrameHook().ccall<void>();
    Watchdog::Tick();
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
