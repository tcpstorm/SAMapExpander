// What a module has to know before it makes a vehicle of its own, in one place.
//
// Every fact here was established by disassembling gta_sa.exe 1.0 US, and every one of them is
// here because getting it wrong is not a graceful failure - it is either a crash or a regression
// in San Andreas' own traffic that looks like nothing to do with us.
#include <plugin.h>
#include <CPools.h>
#include <CVehicle.h>
#include <CWorld.h>
#include <cstdint>
#include <cstring>
#include "VehicleSpawn.h"
#include "Log.h"

using namespace plugin;

namespace {

// ------------------------------------------------------------------ which counter a car is on
//
// CCarCtrl::UpdateCarCount (0x424000) is the only thing that moves a vehicle between San Andreas'
// five car counters, and the jump table at 0x4240CC/0x4240DC says exactly which counter each
// eVehicleCreatedBy lands on:
//
//   1 RANDOM    0x969094 NumRandomCars   (and 0x969098 NumLawEnforcerCars for a police one)
//   2 MISSION   0x96909C NumMissionCars
//   3 PARKED    0x9690A0 NumParkedCars
//   4 PERMANENT 0x9690A4 NumPermanentCars
//
// Two of those are budgets San Andreas checks against itself, and both would be regressions:
//
//   RANDOM - CCarCtrl::GenerateOneRandomCar sums random + law + mission + the two emergency
//   counters against MaxNumberOfCars (the int 12 at 0x8A5B24), and GenerateRandomCars bails
//   outright once NumRandomCars reaches 45. Thirty cars of ours marked RANDOM would stop San
//   Andreas generating civilian traffic anywhere, for as long as they existed.
//
//   PARKED - and this one matters even more to parked cars, for the obvious reason. Every reader
//   of NumParkedCars in the whole executable is one of three, and all three are budgets:
//   CCarGenerator::DoInternalProcessing (0x6F351B), CCarCtrl::GenerateEmergencyServicesCar
//   (0x42FA27) and CPopulation::AddToPopulation (0x614860). The first is the one that hurts: it
//   reads NumParkedCars and returns immediately if it is at or above 5 by day or 10 by night. So
//   FIVE cars of ours marked PARKED would stop every one of San Andreas' own 500 car generators
//   producing anything, everywhere in the state, for as long as ours existed - the same shape of
//   regression as the traffic one, one counter over.
//
// NumPermanentCars is read by nothing at all. xref over the whole .text finds only CCarCtrl::Init,
// CCarCtrl::ReInit and UpdateCarCount itself. That is why our vehicles are PERMANENT: not because
// they are special, but because it is the one counter that is not a budget. The price is that San
// Andreas will not cull them for us, which is correct - we know which ones are ours and cull them
// ourselves.
constexpr unsigned char kCreatedByRandom = 1;
constexpr unsigned char kCreatedByPermanent = 4;

// CVehicle::SetVehicleCreatedBy. Never poke m_nCreatedBy (+0x4A4) directly: this reads the old
// value, calls UpdateCarCount to take the car off that counter, writes the new one and calls it
// again to put the car on the new counter. The constructor has already counted the car once.
constexpr uintptr_t kSetVehicleCreatedBy = 0x6D5D70;

// m_nCreatedBy, read at 0x6D5D78 and written at 0x6D5D8E by the setter above. Read here and never
// written - the setter is the only thing that may write it, because it is also what moves the car
// between San Andreas' counters.
constexpr unsigned kVehicleCreatedBy = 0x4A4;

// ------------------------------------------------------------------ is the model in memory
//
// There is no CStreaming::HasModelLoaded to call in 1.0 US: it is inlined at over a hundred sites
// as `cmp byte ptr [m*0x14 + 0x8E4CD0], 1`. Getting this wrong is a fault rather than an empty
// street - the vehicle constructor reaches CVehicleModelInfo::CreateInstance, which dereferences
// m_pVehicleStruct without checking it and faults at 0x4C9691 reading 0x30C.
constexpr uintptr_t kStreamingLoadState = 0x8E4CD0;   // ms_aInfoForModel[0].m_nLoadState
constexpr unsigned kStreamingInfoStride = 0x14;
constexpr unsigned char kLoadStateLoaded = 1;

// ------------------------------------------------------------------ the two functions we call
//
// CCarCtrl::CreateCarForScript and CPopulation::AddPedInCar are called rather than patched, but
// they are checked the same way anything else in this project is checked, because calling the
// wrong address is worse than not calling it.
constexpr uintptr_t kCreateCarForScript = 0x431F80;
constexpr uintptr_t kAddPedInCar = 0x613A00;
const uint8_t kCreateCarPrologue[] = { 0x6A, 0xFF, 0x68, 0xBB, 0x74, 0x83, 0x00 };
const uint8_t kAddPedInCarPrologue[] = { 0x83, 0xEC, 0x18, 0x53, 0x56, 0x57 };

bool gChecked = false;
bool gReady = false;

bool VerifyCall(uintptr_t address, const uint8_t* expected, size_t length, const char* what) {
    uint8_t here[8] = {};
    memcpy(here, (const void*)address, length);
    if (memcmp(here, expected, length) == 0) return true;
    Log::Write("vehicles: %s at 0x%X does not read as expected (%02X %02X %02X %02X...); no "
               "vehicle of ours will be created", what, (unsigned)address,
               here[0], here[1], here[2], here[3]);
    return false;
}

}   // namespace

namespace VehicleSpawn {

bool Ready() {
    if (!gChecked) {
        gChecked = true;
        gReady = VerifyCall(kCreateCarForScript, kCreateCarPrologue, sizeof(kCreateCarPrologue),
                            "CCarCtrl::CreateCarForScript") &&
                 VerifyCall(kAddPedInCar, kAddPedInCarPrologue, sizeof(kAddPedInCarPrologue),
                            "CPopulation::AddPedInCar");
    }
    return gReady;
}

bool ModelIsLoaded(int model) {
    if (model < 0 || model > 30000) return false;
    return *(const unsigned char*)(kStreamingLoadState + (uintptr_t)model * kStreamingInfoStride)
           == kLoadStateLoaded;
}

bool PoolHasRoom(int reserve) {
    auto* pool = CPools::ms_pVehiclePool;
    return pool && (int)pool->GetNoOfFreeSpaces() > reserve;
}

void MakePermanent(CVehicle* vehicle) {
    reinterpret_cast<void(__thiscall*)(void*, unsigned char)>(kSetVehicleCreatedBy)(
        vehicle, kCreatedByPermanent);
}

// The one hole in "we only ever write to a vehicle of ours".
//
// A CPools handle is (slot << 8) | the slot's re-use byte, and CPools::GetVehicle (0x54FFF0)
// checks that byte before handing the vehicle back - which is why a handle to a car the game has
// destroyed reads as nothing. But CPool's re-use counter is SEVEN BITS (CPool.h's `nId : 7`), so
// the 128th re-use of a slot answers to the handle we stored for the 1st. San Andreas has the same
// property and lives with it because it re-checks nothing; we would be writing status, mission,
// cruise speed and createdBy into whatever car happened to land there - one of the game's own.
//
// So a handle is not the whole record. The model a car was created with cannot change while it
// lives, and PERMANENT is a counter San Andreas puts nothing on by itself (mission cars are
// MISSION, traffic is RANDOM, generators are PARKED), so the two together are as good as a name.
// A vehicle that fails either test is one we no longer own and nothing here touches it.
CVehicle* Resolve(int ref, int model) {
    if (ref < 0) return nullptr;
    CVehicle* vehicle = CPools::GetVehicle(ref);
    if (!vehicle) return nullptr;
    if (model >= 0 && (int)vehicle->m_nModelIndex != model) return nullptr;
    if (*((const unsigned char*)vehicle + kVehicleCreatedBy) != kCreatedByPermanent) return nullptr;
    return vehicle;
}

// The idiom San Andreas' own script uses for a car it no longer wants to own: opcode 01C3
// MARK_CAR_AS_NO_LONGER_NEEDED reaches CTheScripts::CleanUpThisVehicle, which is at 0x486670 and
// behind the HOODLUM jump at 0x1565CA0. It does two things that matter and this does both:
//
//   `and byte ptr [esi+0x428], 0xF7` clears bit 3, which is bIsLocked - "locked by the script
//   (cannot be removed)". CCarCtrl::CreateCarForScript SETS that bit at 0x43205F, which is what
//   keeps San Andreas' distance and pool sweeps off a vehicle of ours while it is ours. Handing
//   one back without clearing it would put the car on the RANDOM counter and still leave nothing
//   able to remove it, which is the leak this is meant to avoid.
//
//   `SetVehicleCreatedBy(RANDOM)`, through the same setter.
//
// What it also does and this does not: it clears two CPhysical flags at +0x40 that
// CreateCarForScript never set, and it takes the vehicle out of the script's mission cleanup list,
// which ours were never in - CreateCarForScript is called with missionCleanup false.
constexpr uintptr_t kVehicleFlagsByte0 = 0x428;
constexpr unsigned char kFlagIsLocked = 0x08;

void HandBackToTheGame(CVehicle* vehicle) {
    *((unsigned char*)vehicle + kVehicleFlagsByte0) &= (unsigned char)~kFlagIsLocked;
    reinterpret_cast<void(__thiscall*)(void*, unsigned char)>(kSetVehicleCreatedBy)(
        vehicle, kCreatedByRandom);
}

// NOT with C++ delete. A CVehicle comes out of the game's own pool through its own allocator, and
// the plugin's operator delete knows nothing about that - freeing one that way corrupts a heap
// that is not ours. What the game does, in CPopulation::RemovePedsIfThePoolGetsFull and everywhere
// else it disposes of an entity, is CWorld::Remove followed by the first vtable entry with the
// deleting flag set, which is the scalar deleting destructor and returns the memory whence it came.
void Remove(CVehicle* vehicle) {
    CWorld::Remove(vehicle);
    typedef void(__thiscall* ScalarDeletingDtorFn)(void* self, unsigned char deleting);
    void** vtable = *(void***)vehicle;
    reinterpret_cast<ScalarDeletingDtorFn>(vtable[0])(vehicle, 1);
}

bool PlayerIsIn(CVehicle* vehicle) {
    CPed* player = FindPlayerPed();
    if (!player || !vehicle) return false;
    if (vehicle->m_pDriver == player) return true;
    for (int i = 0; i < 8; i++)
        if (vehicle->m_apPassengers[i] == player) return true;
    return false;
}

}   // namespace VehicleSpawn
