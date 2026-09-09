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

}   // namespace Ground
