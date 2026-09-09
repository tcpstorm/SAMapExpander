#pragma once

// What is left of San Andreas' one flat resource id space, said out loud in the log.
//
// Every streamable thing the game owns - models, texture dictionaries, collision files, IPL
// sections, path data, animations, car recordings and external scripts - is one entry in
// CStreaming::ms_aInfoForModel, numbered in one contiguous space with a fixed range for each kind.
// Adding a region spends ids out of THREE of those ranges at once, and only one of them (the model
// range) ever says so. Report tells the user which range is nearest its end, and refuses to be
// quiet about one that has gone past it: a texture dictionary numbered past the end of the TXD
// range does not fail, it takes the streaming entry of a collision file.
namespace IdSpace {

// After CStreaming::Init2, which is when CStreaming::LoadCdDirectory has created the last of the
// TXD and COL slots and every region's IDE has been read.
void Report();

}   // namespace IdSpace
