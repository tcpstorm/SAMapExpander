#pragma once

// "What is the ground here, and is there any?" - asked so that the second half of the question is
// answerable. Ground.cpp says why San Andreas' obvious routine cannot answer it.
namespace Ground {

// The ground under (x, y), searched downward from `fromZ`. Returns false when there is nothing
// there at all, and leaves outZ alone. A caller that gets false must not place anything: there is
// no floor, and inventing one puts a pedestrian in the air.
bool Below(float x, float y, float fromZ, float& outZ);

// The same question for a point whose own height is not entirely trusted: probe from `lift` above
// the height on offer, so a surface a little ABOVE the claimed one is still found. Everything this
// project spawns comes off a twenty-year-old graph, so this is the one to call.
bool Under(float x, float y, float claimedZ, float& outZ);

// How far above a claimed height Under() looks. Public so a caller can say in a log line what the
// search actually covered.
float ProbeLift();

// Every surface at (x, y) from `fromZ` downwards, highest first, into `out`. Returns how many were
// written.
//
// This is a measuring instrument, not a placement routine. "The ped is under the map" and "the ped
// is on the only floor there is" look identical from a single probe, and which of the two it is
// decides whether the fix is a different start height or a different rule entirely. Guessing at
// that from one number has now cost two rounds; this counts the decks.
int Surfaces(float x, float y, float fromZ, float* out, int max);

// The same, formatted for a log line: "3 surface(s) from 210.0 down: 12.4, 3.9, -18.2".
void Describe(float x, float y, float fromZ, char* out, unsigned size);

// Is this point below EVERY surface at its own x and y - that is, under the world rather than
// under something? True for a player who has fallen through the map, or is flying under it, and
// false for one standing in a tunnel, under a bridge or inside a building, because those still
// have a floor beneath them. See Ground.cpp for why the distinction is drawn that way.
bool BeneathEverything(float x, float y, float z);

}   // namespace Ground
