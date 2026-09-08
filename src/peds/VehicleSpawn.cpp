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

// ------------------------------------------------------------------ is the model in memory
//
// There is no CStreaming::HasModelLoaded to call in 1.0 US: it is inlined at over a hundred sites
// as `cmp byte ptr [m*0x14 + 0x8E4CD0], 1`, so the flag is read the same way here. Getting this
// wrong is a fault rather than an empty street - the vehicle constructor reaches
// CVehicleModelInfo::CreateInstance, which dereferences m_pVehicleStruct without checking it and
// faults at 0x4C9691 reading 0x30C.
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

// The idiom San Andreas' own script uses for a car it no longer wants to own: opcode 01C3
// MARK_CAR_AS_NO_LONGER_NEEDED puts a mission car back on the RANDOM counter and lets the game
// dispose of it. Used here only where we genuinely stop owning a vehicle and cannot delete it -
// the one the player is sitting in.
void HandBackToTheGame(CVehicle* vehicle) {
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
