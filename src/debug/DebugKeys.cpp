// F6, F8 and F9, with [World] Debug=1. F6 drops the player through the world where they stand, so
// the fall recovery can be tried without hunting for a hole in the collision. F8 logs the player's
// SA position, the equivalent region-local
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
#include "FallRecovery.h"
#include "RegionRestarts.h"
#include "VehicleSpawn.h"
#include "MissionEngine.h"
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
static bool gDropWasDown = false;

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
    int refused = 0;
    for (int i = 0; i < imported; i++)
    {
        const int type = RegionWeapons::ImportedType(i);
        if (type < 0) continue;
        const int model = RegionWeapons::WeaponModel(type);
        if (model <= 0)
        {
            refused++;
            continue;
        }
        CallRequestModel(model, 0x16);
        wanted++;
    }
    if (wanted) CallLoadAllRequestedModels(false);

    int given = 0;
    for (int i = 0; i < imported; i++)
    {
        const int type = RegionWeapons::ImportedType(i);
        if (type < 0) continue;
        if (RegionWeapons::WeaponModel(type) <= 0) continue;
        CallGiveWeapon(player, type, 500, false);
        Log::Write("F9: gave weapon type %d (behaving as %d), model %d", type,
                   WeaponTypes::DonorOf(type), RegionWeapons::WeaponModel(type));
        given++;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "MapExpander: %d imported weapon(s), 500 rounds each", given);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);
    Log::Write("F9: %d imported weapon(s) given, %d refused for having no model", given, refused);
}

// F10, with [World] Debug=1: put the vehicle named by [World] DebugSpawnVehicle down six units in
// front of the player. It exists so an imported car can be looked at on demand rather than waited
// for in traffic. The model is streamed in first, the same way F9 streams a weapon's, and the car
// is handed to the game as an ordinary random car so it is cleaned up like any other.
static void SpawnDebugVehicle() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) return;
    const std::string& name = Settings::world.debugSpawnVehicle;
    char msg[160];
    int model = -1;
    CBaseModelInfo* info = name.empty() ? nullptr : CModelInfo::GetModelInfo((char*)name.c_str(), &model);
    if (!info || model < 0) {
        snprintf(msg, sizeof(msg), "MapExpander: no model called %s", name.c_str());
        CMessages::AddMessageJumpQ(msg, 4000, 0, false);
        Log::Write("F10: no model is registered under the name \"%s\"", name.c_str());
        return;
    }
    if (!VehicleSpawn::Ready()) {
        CMessages::AddMessageJumpQ((char*)"MapExpander: vehicle creation is owned by another mod", 4000, 0, false);
        Log::Write("F10: VehicleSpawn is not ready, so nothing is created");
        return;
    }
    if (!VehicleSpawn::ModelIsLoaded(model)) {
        CallRequestModel(model, 0x16);
        CallLoadAllRequestedModels(false);
    }
    if (!VehicleSpawn::ModelIsLoaded(model))
    {
        snprintf(msg, sizeof(msg), "MapExpander: %s (model %d) cannot be built - see log", name.c_str(), model);
        CMessages::AddMessageJumpQ(msg, 4000, 0, false);
        Log::Write("F10: %s is model %d, loaded=%d - not spawning it", name.c_str(),
                   model, VehicleSpawn::ModelIsLoaded(model) ? 1 : 0);
        return;
    }
    if (!VehicleSpawn::PoolHasRoom(4)) {
        CMessages::AddMessageJumpQ((char*)"MapExpander: the vehicle pool is full", 4000, 0, false);
        Log::Write("F10: the vehicle pool has no room");
        return;
    }
    CVector pos = player->GetPosition();
    CVector forward(0.0f, 1.0f, 0.0f);
    if (player->m_matrix) forward = player->m_matrix->up;
    CVector at(pos.x + forward.x * 6.0f, pos.y + forward.y * 6.0f, pos.z + 0.5f);
    CVehicle* car = VehicleSpawn::Create(model, at, false);
    if (!car) {
        snprintf(msg, sizeof(msg), "MapExpander: the game refused to create %s", name.c_str());
        CMessages::AddMessageJumpQ(msg, 4000, 0, false);
        Log::Write("F10: CreateCarForScript returned nothing for %s (model %d)", name.c_str(), model);
        return;
    }
    VehicleSpawn::HandBackToTheGame(car);
    snprintf(msg, sizeof(msg), "MapExpander: %s (model %d)", name.c_str(), model);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);
    Log::Write("F10: created %s (model %d) at SA (%.1f, %.1f, %.1f)", name.c_str(), model, at.x, at.y, at.z);
}

static bool gMissionKeyWasDown = false;
static bool gMissionEndKeyWasDown = false;
static size_t gNextMissionIndex = 0;

// F11, with [World] Debug=1: start the next loaded mission (cycling through every one
// MissionEngine::Install() found), so a mission can be tried without standing at its own
// payphone - see docs/missions.md, "nothing places a ringing payphone marker yet". F12 ends
// whatever is running early, the way walking away from a real mission does.
static void CycleMission() {
    const std::vector<std::string>& ids = MissionEngine::LoadedIds();
    if (ids.empty()) {
        CMessages::AddMessageJumpQ((char*)"MapExpander: no missions loaded", 4000, 0, false);
        Log::Write("F11: no missions loaded - see docs/missions.md for how to deploy tools/"
                   "import_missions.py's output and point a region's [region] Missions= at it");
        return;
    }
    // While one is running, F11 reports it rather than failing to start the next: "could not
    // start that mission" was every tester's first sight of a mission that was in fact running.
    if (MissionEngine::Running()) {
        std::string state;
        MissionEngine::Describe(state);
        Log::Write("F11: %s", state.c_str());
        char msg[200];
        snprintf(msg, sizeof(msg), "MapExpander: %.190s", state.c_str());
        for (char* c = msg; *c; c++) if (*c == '\n') *c = ' ';
        CMessages::AddMessageJumpQ(msg, 6000, 0, false);
        return;
    }
    const std::string& id = ids[gNextMissionIndex % ids.size()];
    gNextMissionIndex++;
    if (MissionEngine::StartMission(id)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "MapExpander: starting mission %s", id.c_str());
        CMessages::AddMessageJumpQ(msg, 4000, 0, false);
    } else {
        CMessages::AddMessageJumpQ((char*)"MapExpander: could not start that mission (see log)",
                                   4000, 0, false);
    }
}

static void OnFrame() {
    if (!Settings::world.debug) return;

    // F6 drops the player through the world where they stand, which is the only practical way to
    // try the fall recovery on purpose: a real fall needs a hole in the collision to be found first.
    bool dropDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (dropDown && !gDropWasDown) FallRecovery::DebugDrop();
    gDropWasDown = dropDown;

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
    // Whether this reading is worth anything, said on the reading itself.
    //
    // Three F8 samples in one evening were taken from a jetpack, from under the map, and from the
    // air beside a terminal roof, and each was reasoned from as though the player had been standing
    // in a street. One of them cost a whole false theory about the graph being scaled. A
    // measurement should carry its own validity rather than depend on somebody remembering where
    // they were, so the line now says.
    float floorZ = 0.0f;
    const char* standing = "UNDER THE WORLD - no floor beneath the player, so nothing on this line "
                           "describes anywhere a person could be";
    char airborne[96] = {};
    if (Ground::Standing(pos.x, pos.y, pos.z, floorZ)) {
        float above = pos.z - floorZ;
        if (above <= 2.5f) {
            standing = "on the ground";
        } else {
            snprintf(airborne, sizeof(airborne),
                     "AIRBORNE, %.1f units above the floor beneath - not a reading from a street",
                     above);
            standing = airborne;
        }
    }
    Log::Write("    ground under the player: %s (the player's own z is %.2f) - %s",
               surfaces, pos.z, standing);
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
    std::string fall;
    FallRecovery::Describe(fall);
    Log::Write("    fall recovery: %s", fall.c_str());
    std::string restarts;
    RegionRestarts::Describe(restarts);
    Log::Write("    restart points: %s", restarts.c_str());
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
