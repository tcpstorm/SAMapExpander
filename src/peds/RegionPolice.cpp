// Cops that come after you in an imported city.
//
// Everything about the wanted system itself already works out here. The stars go up, the police
// radio talks, the wanted level decays, CWanted counts its pursuers, and the player is hated by
// anything with a badge - none of that has a map extent or a path dependency, which was checked.
// What does not happen is anybody turning up, and for one reason: every route San Andreas has for
// producing a policeman goes through the path graph.
//
//   CPopulation::AddToPopulation      makes cops on foot at ped path nodes
//   CCarCtrl::GenerateOneRandomCar    makes patrol cars at car path nodes
//   CPopulation::CreateWaitingCoppers places its car with CPathFind::FindNthNodeClosestToCoors
//   CRoadBlocks::GenerateRoadBlocks   is path nodes from end to end
//
// A region has no path nodes - it cannot, a node position is an int16 times 0.125 and reaches only
// +/-4096 - so all four produce nothing and a five-star chase is silent. See
// docs/known-limitations.md.
//
// So the cops on foot are made here, from the region's own pedestrian graph, which is the only
// description of walkable ground a region has. The recipe is not invented: it is what
// CPopulation::CreateWaitingCoppers does once it has chosen a spot, read out of the game and
// followed step for step - create the ped as a cop, give it a Colt 45 with plenty of ammunition,
// and set CTaskComplexKillPedOnFoot on the player. After that the ped is an ordinary San Andreas
// policeman: it chases, shoots, takes cover, calls for help, registers itself with CWanted and is
// culled like any other.
//
// Patrol cars, helicopters, roadblocks and the SWAT and army responses at four and five stars are
// NOT here, and cannot be until traffic is: every one of them is a vehicle placed on a car path
// node. What a region gets is police on foot, at every wanted level, in the numbers CWanted says
// it wants. That is most of what a chase feels like on foot and none of what it feels like in a
// car, and it is worth saying so plainly rather than pretending otherwise.
//
// Nothing here runs under SA-MP: the server owns the police there.
#include <plugin.h>
#include <CPopulation.h>
#include <CPools.h>
#include <CPed.h>
#include <CPlayerPed.h>
#include <CWanted.h>
#include <CStreaming.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CModelInfo.h>
#include <CPedModelInfo.h>
#include <CTaskComplexKillPedOnFoot.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include "RegionPolice.h"
#include "RegionPeds.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// CTask::operator new, and CTaskComplexKillPedOnFoot's constructor. The task is built the way the
// game builds it - the game's allocator, then the real constructor - rather than with C++ new,
// because the game will be the one to delete it.
constexpr uintptr_t kTaskNew = 0x61A5A0;
constexpr uintptr_t kKillPedOnFootCtor = 0x620E30;
constexpr unsigned kKillPedOnFootSize = 0x38;   // matches plugin-sdk's VALIDATE_SIZE
// push -1; push 0x83F648; mov eax, fs:[0] - the exception frame this constructor opens with.
const uint8_t kKillPedOnFootPrologue[] = { 0x6A, 0xFF, 0x68, 0x48, 0xF6, 0x83, 0x00 };

typedef void*(__cdecl* TaskNewFn)(unsigned size);
typedef void*(__thiscall* KillPedOnFootCtorFn)(void* self, CPed* target, int time, int specFlags,
                                               int delay, int chance, char a7);

// What CPopulation::CreateWaitingCoppers passes: no time limit, no special flags, and the same
// trailing 2 - and the two task flags it then sets by hand at task+0xC.
constexpr int kNoTimeLimit = -1;
constexpr unsigned char kCopTaskFlags = 0x30;
constexpr int kTaskPrimary = 3;             // ePrimaryTasks::TASK_PRIMARY_PRIMARY
constexpr int kCopWeapon = 22;              // WEAPON_PISTOL, the Colt 45 its own coppers carry
constexpr unsigned kCopAmmo = 30000;
constexpr unsigned char kPedCreatedByGame = 1;

// Where a cop may appear. Far enough that they are not conjured next to the player, close enough
// that the chase does not take a minute to arrive. San Andreas' own foot cops come off the nearest
// path nodes, which is much the same band.
constexpr float kMinCopDistance = 25.0f;
constexpr float kMaxCopDistance = 55.0f;
// A cop this far away has lost the player; the game will cull them, and until it does they are not
// counted against the number wanted, so the chase keeps coming.
constexpr float kCopForgetDistance = 130.0f;
// One at a time, and not every frame: a wall of policemen appearing together looks like a spawn
// and not like a response.
constexpr unsigned kSpawnIntervalMs = 900;

bool gCanTask = false;
unsigned gNextSpawn = 0;
std::vector<int> gCops;    // CPools ped handles

CWanted* PlayerWanted() {
    CPlayerPed* player = FindPlayerPed();
    if (!player || !player->m_pPlayerData) return nullptr;
    return player->m_pPlayerData->m_pWanted;
}

// How many cops on foot a wanted level should have chasing. CWanted keeps its own ceiling and the
// game raises it with the stars, so it is asked rather than second-guessed; the fallback is the
// shape of San Andreas' own table for the case where it reads as nothing.
int CopsWanted(const CWanted* wanted) {
    static const int kByStars[] = { 0, 1, 2, 3, 5, 6, 8 };
    int stars = (int)wanted->m_nWantedLevel;
    if (stars < 0) stars = 0;
    if (stars > 6) stars = 6;
    int ceiling = (int)wanted->m_nMaxCopsInPursuit;
    int want = kByStars[stars];
    if (ceiling > 0 && want > ceiling) want = ceiling;
    return want;
}

bool VerifyTask() {
    uint8_t here[sizeof(kKillPedOnFootPrologue)];
    memcpy(here, (const void*)kKillPedOnFootCtor, sizeof(here));
    if (memcmp(here, kKillPedOnFootPrologue, sizeof(here)) == 0) return true;
    Log::Write("police: CTaskComplexKillPedOnFoot at 0x%X does not read as expected; the region "
               "gets no police response", (unsigned)kKillPedOnFootCtor);
    return false;
}

// Set the ped after the player, exactly as CreateWaitingCoppers does: build the task with the
// game's own allocator and constructor, set the two flags it sets, and put it in the primary slot.
bool SendAfterPlayer(CPed* cop, CPed* player) {
    if (!gCanTask || !cop || !cop->m_pIntelligence) return false;
    void* memory = reinterpret_cast<TaskNewFn>(kTaskNew)(kKillPedOnFootSize);
    if (!memory) return false;
    reinterpret_cast<KillPedOnFootCtorFn>(kKillPedOnFootCtor)(memory, player, kNoTimeLimit,
                                                              0, 0, 0, 2);
    *((uint8_t*)memory + 0xC) |= kCopTaskFlags;
    cop->m_pIntelligence->m_TaskMgr.SetTask(reinterpret_cast<CTask*>(memory), kTaskPrimary, false);
    return true;
}

int LiveCops(const CVector& player) {
    size_t write = 0;
    int live = 0;
    for (size_t i = 0; i < gCops.size(); i++) {
        CPed* cop = CPools::GetPed(gCops[i]);
        if (!cop) continue;
        gCops[write++] = gCops[i];
        CVector p = cop->GetPosition();
        float dx = p.x - player.x, dy = p.y - player.y;
        if (!cop->IsAlive()) continue;
        if (dx * dx + dy * dy > kCopForgetDistance * kCopForgetDistance) continue;
        live++;
    }
    gCops.resize(write);
    return live;
}

}   // namespace

namespace RegionPolice {

bool Active() { return gCanTask; }

int Count() { return (int)gCops.size(); }

void Tick(const CVector& playerPosition) {
    if (!gCanTask || !Settings::world.regionPolice) return;
    CWanted* wanted = PlayerWanted();
    if (!wanted) return;
    int live = LiveCops(playerPosition);
    if (wanted->m_nWantedLevel == 0) return;   // the ones already out finish what they are doing
    if (live >= CopsWanted(wanted)) return;
    if (CTimer::m_snTimeInMilliseconds < gNextSpawn) return;
    gNextSpawn = CTimer::m_snTimeInMilliseconds + kSpawnIntervalMs;

    int model = CStreaming::GetDefaultCopModel();
    if (model < 0) return;
    if (!CStreaming::HasModelLoaded(model)) {
        // The cop model for wherever the region has been placed. Ask and come back: the streamer
        // has it in a moment and a chase that starts a second late is better than one that does
        // not start.
        CStreaming::RequestModel(model, GAME_REQUIRED);
        return;
    }
    CVector at;
    if (!RegionPeds::FindSpawnPoint(playerPosition, kMinCopDistance, kMaxCopDistance, true, at))
        return;

    CPed* cop = CPopulation::AddPed(PED_TYPE_COP, (unsigned)model, at, false);
    if (!cop) return;
    cop->SetCharCreatedBy(kPedCreatedByGame);
    cop->GiveWeapon((eWeaponType)kCopWeapon, kCopAmmo, true);
    cop->SetCurrentWeapon((eWeaponType)kCopWeapon);
    SendAfterPlayer(cop, FindPlayerPed());
    gCops.push_back(CPools::GetPedRef(cop));
}

void Install() {
    if (!Settings::world.regionPolice) return;
    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region police response is off, the server owns the police");
        return;
    }
    gCanTask = VerifyTask();
    if (gCanTask)
        Log::Write("police: a wanted level over a region will be answered on foot; patrol cars, "
                   "roadblocks and the helicopter need traffic and are not there");
}

}   // namespace RegionPolice
