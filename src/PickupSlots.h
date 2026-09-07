#pragma once
#include <cstdint>

// CPickups::aPickUps moved somewhere bigger, so a region's world pickups can go in on top of San
// Andreas' own rather than instead of them. See PickupSlots.cpp for what is patched and why.
namespace PickupSlots {

// 620: the array San Andreas ships with, and the number of entries its savegame holds. This never
// changes - it is the whole reason the move is safe.
int VanillaCount();

// Move the array to a block with room for `regionPlacements` more pickups than San Andreas' own,
// keeping `reserve` slots spare on top of that. Returns true when the array was moved; when it
// returns false not one byte of the game has been written and the caller must carry on as if the
// array were still the vanilla one.
bool Grow(int regionPlacements, int reserve);

bool Grown();          // whether Grow moved it
uintptr_t Base();      // where aPickUps is now - the vanilla 0x9788C0 until Grow succeeds
int Count();           // how many entries it has now - VanillaCount() until Grow succeeds

}   // namespace PickupSlots
