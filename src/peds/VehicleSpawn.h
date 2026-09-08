#pragma once
#include <plugin.h>

// The handful of things a module has to get right to put a vehicle of its own into San Andreas
// and take it out again. Two modules do it now - the traffic on a region's carriageways and the
// cars parked at its kerbs - and every one of these was worth getting wrong once, so there is one
// copy of each rather than two. VehicleSpawn.cpp says why each is what it is.
namespace VehicleSpawn {

// Check the two functions both modules call before either of them calls one. False means another
// mod owns them and nothing of ours should create a vehicle at all.
bool Ready();

// Is this model actually in memory? There is no CStreaming::HasModelLoaded to call in 1.0 US.
bool ModelIsLoaded(int model);

// Has the vehicle pool room, leaving `reserve` slots free for everybody else?
bool PoolHasRoom(int reserve);

// Move a vehicle onto the PERMANENT counter, which is the only one nothing in San Andreas budgets
// against. Always through the game's own setter.
void MakePermanent(CVehicle* vehicle);

// Hand a vehicle back to San Andreas as an ordinary random car, the way the script's
// MARK_CAR_AS_NO_LONGER_NEEDED does, so the game may clean it up on its own terms.
void HandBackToTheGame(CVehicle* vehicle);

// Take a vehicle of ours out of the world the way the game does - never with C++ delete.
void Remove(CVehicle* vehicle);

// Is the player driving, or sitting in, this one?
bool PlayerIsIn(CVehicle* vehicle);

}   // namespace VehicleSpawn
