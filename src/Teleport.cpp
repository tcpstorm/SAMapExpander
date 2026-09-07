// F7, with [World] Debug=1: one press per stop, round the imported cities and home again.
//
// Each press moves to the next stop: every region in the order the ini lists them, then San
// Andreas - and the San Andreas stop is wherever the player was standing when the lap began, so
// the last press of a lap puts them back rather than somewhere they never asked for.
//
// A region sits thousands of units from anything the game will drive, fly or fade you to, so this
// is the only practical way to go and look at one. The first attempt at it called CPed::Teleport by
// address and followed that with CStreaming::LoadScene, both from a hook on CTimer::Update, and it
// was blamed - with good reason - for a run of crashes that were then misattributed to whatever
// else had changed that day. What follows is a rewrite that does what the game itself does, in the
// order the game does it, from the point in the frame the game does it from.
//
// It also fixes a calling-convention bug the old code had that on its own is enough to explain
// those crashes: it declared Teleport as taking plugin-sdk's CVector by value, and clang does not
// pass such a class the way MSVC does, so the coordinates the game received were not the ones in
// the log. See the comment on TeleportFn below - that one is worth reading even if none of the
// rest of this is.
//
//
// WHAT THE GAME DOES
//
// San Andreas relocates the player a long way in three places, and all three agree:
//
//   CGameLogic::UpdateSkip (0x442480)  the "skip to the next part" offered after a mission failure.
//       The closest match to what F7 wants: an arbitrary point, arbitrarily far away, in one frame.
//   CGameLogic::RestorePlayerStuffDuringResurrection (0x442060)  waking up at a hospital.
//   Scripts::SetCharCoordinates (0x464DC0)  what opcode 00A1 SET_CHAR_COORDINATES runs, and so the
//       sanctioned path, since script opcodes run at a point in the frame the game considers safe.
//
// The sequence, read off UpdateSkip at 0x4425CB:
//
//   CTimer::Suspend()                                  0x5619D0
//   CStreaming::LoadSceneCollision(&dest)              0x40ED80
//   CStreaming::LoadScene(&dest)                       0x40EB70
//   Scripts::ClearSpaceForMissionEntity(dest, entity)  0x486B00
//   entity->vtable[0x38](dest, false)                  CEntity::Teleport
//   CTimer::Resume()                                   0x561A00
//
// Every line of it is load bearing.
//
//   * Suspend and Resume. Both streaming calls block on the disc - each ends in
//     CStreaming::LoadAllRequestedModels, which spins until every outstanding request is in memory,
//     and for a region the streamer has never been near before that is a real pause. CTimer::Suspend
//     stops the game clock; CTimer::Resume adds the interval it was stopped for to the game's
//     suspended-time total, so the next frame sees an ordinary time step instead of a two second
//     one. Without the pair, every physical object in the world integrates two seconds of gravity
//     in a single step the moment the load finishes.
//
//   * Collision, then the scene, then the entity. Collision decides where the ground is; the scene
//     decides what is drawn and what is instanced into the world's sector lists. Both are done at
//     the DESTINATION while the player is still standing safely where they were. The old code moved
//     first and streamed afterwards, and never asked for collision at all, so the player spent at
//     least one frame somewhere with no ground under them and nothing around them.
//
//   * ClearSpaceForMissionEntity shoves whatever is already standing at the destination out of the
//     way, sized by the arriving entity's own collision model.
//
//   * The vtable slot rather than one override's address. CEntity::Teleport is virtual and the type
//     that overrides it is the type that does the bookkeeping: CPed::Teleport (0x5E4110) flushes
//     the ped's tasks, calls CWorld::Remove, writes the new coordinates, drops the ped's entity
//     references and calls CWorld::Add, so the ped comes out filed in the sector lists for where it
//     now is instead of where it was. CAutomobile (0x6A9CA0), CBike (0x6BCFC0), CTrain (0x6F20E0)
//     and CObject (0x5A17B0) each have their own. Calling one of them by address only works for
//     that one type. The slot is +0x38, index 14, in this executable - the slot all three of the
//     functions above call through - and plugin-sdk's idea of the index does not match, which is
//     how the first attempt managed to jump to address zero.
//
//
// WHO GETS MOVED
//
// Not necessarily the player ped. UpdateSkip (0x44263A) and SET_CHAR_COORDINATES (0x464DCF, which
// reads the ped's in-a-vehicle flag and then its m_pVehicle at +0x58C) both look for the player's
// vehicle first and teleport that instead, leaving the ped where it sits relative to the car. That
// is the only thing that can work: a passenger's matrix is written from the vehicle's every frame,
// so moving the man on his own is undone immediately - and in the meantime CPed::Teleport has run
// CWorld::Remove and CWorld::Add over a ped that, as a passenger, the world does not hold the way
// the teleport assumes. The old code moved the ped unconditionally.
//
//
// WHERE THE GROUND IS
//
// A region's ground is wherever its own collision puts it, which nothing here knows in advance, so
// ask - once the collision is in memory and not before. CWorld::FindGroundZFor3DCoord (0x5696C0)
// casts a vertical line down from a given height and reports whether it hit anything; its simpler
// sibling FindGroundZForCoord (0x569660) returns a flat 20.0 when it finds nothing, which is
// indistinguishable from a real answer and therefore useless here.
//
// What comes back is ground level, and an entity's origin is not at its feet, so add
// CEntity::GetDistanceFromCentreOfMassToBaseOfModel (0x536BE0) - exactly what SET_CHAR_COORDINATES
// does at 0x464E0B before it teleports anything, and it works the same for a car as for a man.
//
// If the line finds nothing there is no collision loaded over the region's origin, which is worth
// saying loudly, because it means the region's .col either did not load or does not cover that
// point. We go anyway, at a fixed height: looking at the region from the air says more about what
// is wrong than refusing to move does.
//
//
// INTERIORS
//
// CGame::currArea (0xB72914) is the interior the player is inside, and while it is non-zero the
// game draws that interior and nothing else - arriving outdoors from a house shows an empty world.
// The resurrection path deals with this at 0x4422D6: write 0 to currArea, clear the player's own
// CEntity::m_nAreaCode (+0x2F), and call CStreaming::RemoveBuildingsNotInArea(0) to throw the
// interior's geometry away. Doing it before the streaming means the scene load brings the outdoor
// buildings back in the same breath.
//
//
// THE CAMERA
//
// Every one of the game's relocations finishes with TheCamera.RestoreWithJumpCut (0x50BAB0), and
// the resurrection path calls SetCameraDirectlyBehindForFollowPed_CamOnAString (0x50BD40) first so
// the cut lands behind the player rather than wherever the camera happened to be looking. Without
// the cut the camera interpolates towards the new position across several thousand units of ocean,
// dragging its collision tests through every sector on the way.
//
//
// WHEN IN THE FRAME
//
// The old code ran from a hook on CTimer::Update. That is called from Idle at 0x53E968, before
// CGame::Process - so not literally mid-frame, but outside the game's own update entirely, and it
// runs whether or not the game is simulating: with the pause menu open, or in the front end,
// CGame::Process returns immediately while Idle carries on calling CTimer::Update.
//
// CGame::Process (0x53BEE0) runs, in this order:
//
//   CPad::UpdatePads, CStreaming::Update, CCutsceneMgr::Update, CMenuManager::Process
//   -------- returns here if CTimer's user pause or code pause is set --------
//   CCheat::DoCheats     the game's own per-frame place for a key press to change the world
//   CClock::Update       <- our hook
//   CWeather::Update
//   Scripts::Process     where SET_CHAR_COORDINATES would run
//   CCollision::Update, the ped and vehicle updates, CWorld::Process (physics)
//   CCamera::Process
//
// and Idle then builds the render list out of what CGame::Process left behind. Anything between
// DoCheats and CWorld::Process is a point at which the game itself is willing to move the player:
// the streamer has had its turn this frame, the cutscene manager has had its turn, and physics, the
// camera and the renderer have not started, so nothing observes a half-moved player.
//
// CCheat::DoCheats would be the obvious hook, being the thing that window exists for, but this
// executable is the HOODLUM crack and DoCheats (0x439AF0) has been relocated: its first instruction
// is a jump into the .HOODLUM section, so its whole five-byte prologue is one branch, and it is
// also the function anything that wants to disable cheats would reach for. CClock::Update
// (0x52CF10) sits five bytes further down CGame::Process, has an ordinary prologue, and is called
// from that one site and nowhere else in the executable. We do not care about the clock; we care
// about being called exactly once per frame from inside that window, and only while the game is
// actually simulating - which the pause check above CCheat::DoCheats hands us for free.
#include <plugin.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <CVehicle.h>
#include <CStreaming.h>
#include <CTheScripts.h>
#include <CCutsceneMgr.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CGame.h>
#include <CMessages.h>
#include <safetyhook.hpp>
#include <string>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "Teleport.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kClockUpdate = 0x52CF10;  // once per frame, from CGame::Process+0xDD

// CEntity::Teleport's vtable slot in this executable, verified against the three sites that call
// through it: Scripts::SetCharCoordinates+0x8F (0x464E4F) and CGameLogic::UpdateSkip+0x1FF
// (0x44267F, the vehicle) and +0x322 (0x4427A2, the ped on foot).
constexpr int kTeleportSlot = 0x38 / sizeof(void*);

// A loose sanity check on that slot before we jump through it. The game's code lies between the
// start of .text and the end of the crack's own section; a slot holding anything else means the
// object is not what we take it for, and the first attempt at this feature died jumping to zero.
constexpr uintptr_t kCodeLow = 0x401000, kCodeHigh = 0x2000000;

// Teleport takes its destination BY VALUE, and that is a trap worth spelling out. gta_sa.exe was
// built with MSVC, which pushes the twelve bytes of the vector onto the stack; clang building for
// i686 follows the GCC ABI, which passes any class with a user-declared copy constructor by
// invisible reference instead - and plugin-sdk's CVector declares one. Write the typedef with
// CVector in it and the compiler pushes a POINTER where the game reads x, the bool where it reads
// y, and whatever happened to be on the stack where it reads z. The log then reports the
// coordinates it meant to use while the game files the player at a denormal x, a zero y and a
// garbage z, and a garbage z run through CEntity::Add's float-to-sector-index conversion indexes
// off the end of the world grid. That is what the first attempt at this feature did - it used
// exactly this typedef with CVector in it - and it is the best explanation anyone has for the
// crashes that followed it. A bare three-float struct is trivially copyable and is passed the way
// the game expects. (plugin-sdk's own CEntity::Teleport wrapper takes CVector by value too, so it
// has the same problem; another reason not to go through it.)
struct RawVector { float x, y, z; };
typedef void(__thiscall* TeleportFn)(void* entity, RawVector pos, bool resetRotation);

// CEntity::GetDistanceFromCentreOfMassToBaseOfModel: ground level plus this is where the entity's
// origin belongs - the negated lowest point of its collision model, or a ped's stance offset.
typedef float(__thiscall* BaseOfModelFn)(void* entity);
const BaseOfModelFn CallDistanceToBaseOfModel = reinterpret_cast<BaseOfModelFn>(0x536BE0);

// Install() runs during the DLL's static initialisation, before this file's globals would be
// constructed, so the hook object is a function-local static.
SafetyHookInline& FrameHook() { static SafetyHookInline h; return h; }
bool gWasDown = false;

// How far above the region's own zero to start the downward line test. The game's own
// FindGroundZForCoord starts at an absolute 1000; a region carries its heights on originZ, so this
// is the same thing measured from the region's floor.
const float kProbeHeight = 1000.0f;
// Where to put the player when that line hits nothing at all.
const float kNoGroundHeight = 100.0f;

// Where F7 goes next, and where it came from.
//
// One press per stop: each region in the order the ini lists them, then San Andreas, then round
// again. The San Andreas stop is deliberately not a fixed landmark - it is wherever the player was
// standing when the cycle began, so the last press of a lap puts you back where you started rather
// than somewhere in Los Santos you never asked for. Only if that is not known - the cycle was
// already part way round when the game loaded, say - does it fall back to Grove Street, which is
// as safe a piece of San Andreas as any: on the ground, out of the water, inside the streamed map.
int gNextStop = 0;
bool gHaveHome = false;
CVector gHome;
// Grove Street, for when nothing better is known.
const CVector kSanAndreasFallback(2493.9f, -1682.0f, 13.5f);

void TeleportToNextStop() {
    // The stop list is every region, then San Andreas; the San Andreas one has no MapRegion behind
    // it, which is what `region` being null below means.
    const int stops = (int)Settings::regions.size() + 1;
    if (gNextStop >= stops) gNextStop = 0;
    const bool goingHome = gNextStop == (int)Settings::regions.size();
    const MapRegion* region = goingHome ? nullptr : &Settings::regions[gNextStop];

    // The ped on screen during a cutscene is a CCutsceneObject and not this one, and the camera is
    // on a spline that a jump cut would fight.
    if (CCutsceneMgr::ms_running || CCutsceneMgr::ms_cutsceneProcessing) {
        Log::Write("F7: a cutscene is running; not teleporting");
        return;
    }
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        Log::Write("F7: no player ped yet; not teleporting");
        return;
    }
    if (!player->m_pRwObject) {
        Log::Write("F7: the player has no model right now (still loading); not teleporting, try "
                   "again once you have control");
        return;
    }

    // Move the car, not the man sitting in it.
    CVehicle* vehicle = FindPlayerVehicle();
    CEntity* mover = vehicle ? static_cast<CEntity*>(vehicle) : static_cast<CEntity*>(player);
    const char* what = vehicle ? "the player's vehicle" : "the player on foot";

    void** vtable = *reinterpret_cast<void***>(mover);
    void* slot = vtable ? vtable[kTeleportSlot] : nullptr;
    uintptr_t slotAddr = reinterpret_cast<uintptr_t>(slot);
    if (slotAddr < kCodeLow || slotAddr >= kCodeHigh) {
        Log::Write("F7: the Teleport slot of %s holds %08X, which is not code; refusing to jump "
                   "through it", what, (unsigned)slotAddr);
        return;
    }

    // Remember where the lap started, so its last press can bring the player back here. Taken
    // before anything moves, and only at the top of a lap, so wandering around a region between
    // presses does not overwrite it.
    if (gNextStop == 0) {
        gHome = player->GetPosition();
        gHaveHome = true;
    }

    // The floor a region's heights are measured from; San Andreas' own is simply zero.
    const float floorZ = region ? region->originZ : 0.0f;
    CVector dest;
    if (region) {
        dest = CVector(region->originX, region->originY, floorZ + kProbeHeight);
    } else if (gHaveHome) {
        dest = CVector(gHome.x, gHome.y, gHome.z + kProbeHeight);
    } else {
        dest = CVector(kSanAndreasFallback.x, kSanAndreasFallback.y,
                       kSanAndreasFallback.z + kProbeHeight);
    }
    const char* where = region ? region->name.c_str()
                              : (gHaveHome ? "San Andreas, where you set off from"
                                           : "San Andreas (Grove Street)");
    Log::Write("F7: stop %d of %d - going to [%s] at SA (%.1f, %.1f), moving %s through Teleport "
               "at %08X", gNextStop + 1, stops, where, dest.x, dest.y, what, (unsigned)slotAddr);
    gNextStop++;

    // Everything from here to CTimer::Resume runs with the game clock stopped. The two streaming
    // calls below block until the disc has caught up, and a frame that takes two seconds of wall
    // clock must not become two seconds of physics.
    CTimer::Suspend();

    if (CGame::currArea != 0) {
        Log::Write("F7: leaving interior %d first, so it is the outdoor map that gets drawn",
                   CGame::currArea);
        CGame::currArea = 0;
        player->m_nAreaCode = 0;
        if (vehicle) vehicle->m_nAreaCode = 0;
        CStreaming::RemoveBuildingsNotInArea(0);
    }

    Log::Write("F7: loading the collision at the destination");
    CStreaming::LoadSceneCollision(&dest);
    Log::Write("F7: streaming the scene there");
    CStreaming::LoadScene(&dest);

    bool foundGround = false;
    float groundZ = CWorld::FindGroundZFor3DCoord(dest.x, dest.y, dest.z, &foundGround, nullptr);
    if (foundGround) {
        dest.z = groundZ + CallDistanceToBaseOfModel(mover);
        Log::Write("F7: ground at z %.2f, so arriving at z %.2f", groundZ, dest.z);
    } else {
        dest.z = floorZ + kNoGroundHeight;
        Log::Write("WARNING: F7 found nothing to stand on under (%.1f, %.1f) anywhere between z "
                   "%.1f and -1000 - the region's collision either did not load or does not cover "
                   "its origin. Arriving at z %.1f with nothing underneath.",
                   dest.x, dest.y, floorZ + kProbeHeight, dest.z);
    }

    // Sized by the arriving entity's own collision model, so a car clears more room than a man.
    CTheScripts::ClearSpaceForMissionEntity(dest, mover);

    Log::Write("F7: teleporting");
    RawVector wire = { dest.x, dest.y, dest.z };
    reinterpret_cast<TeleportFn>(slot)(mover, wire, false);

    // Cut, do not fly: the camera is several thousand units behind and would drag its collision
    // tests the whole way across.
    TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
    TheCamera.RestoreWithJumpCut();

    CTimer::Resume();
    Log::Write("F7: arrived at SA (%.1f, %.1f, %.1f)", dest.x, dest.y, dest.z);

    char msg[128];
    snprintf(msg, sizeof(msg), "MapExpander: %s (%.0f, %.0f)", where, dest.x, dest.y);
    CMessages::AddMessageJumpQ(msg, 4000, 0, false);
}

void __cdecl Hooked_ClockUpdate() {
    FrameHook().ccall<void>();

    bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    bool justPressed = down && !gWasDown;
    gWasDown = down;
    if (justPressed) TeleportToNextStop();
}

}  // namespace

namespace Teleport {

void Install() {
    if (!Settings::world.debug || Settings::regions.empty()) return;

    // plugin-sdk's events never fire under SA-MP, so poll from a function the game itself calls -
    // and from this one in particular, for the reasons set out at the top of the file.
    FrameHook() = safetyhook::create_inline(reinterpret_cast<void*>(kClockUpdate),
                                            reinterpret_cast<void*>(&Hooked_ClockUpdate));
    if (!FrameHook()) {
        Log::Write("could not hook CClock::Update; the F7 teleport is unavailable");
        return;
    }

    std::string tour;
    for (const MapRegion& r : Settings::regions) tour += r.name + " -> ";
    Log::Write("F7 cycles one stop per press: %sSan Andreas (back where you set off) -> round again",
               tour.c_str());
    if (Settings::RunningUnderSaMp())
        Log::Write("note: under SA-MP the server owns where the player is and will pull you back, "
                   "so F7 is only of use in single player");
}

}  // namespace Teleport
