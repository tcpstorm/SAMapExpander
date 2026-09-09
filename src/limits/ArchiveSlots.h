#pragma once
#include <cstdint>

// CStreaming::ms_files moved somewhere with more than eight slots, so a fourth, fifth and sixth
// region can register an archive of its own. See ArchiveSlots.cpp for what is patched and why.
namespace ArchiveSlots {

// Move the table. Must run before CStreaming::InitImageList, which is where San Andreas registers
// gta3.img and gta_int.img; the plugin's static constructor is long before that. Writes nothing at
// all unless every site still holds the value the table expects.
void Install();

int VanillaCount();     // 8: what San Andreas ships, and all the log should ever claim without this
int MaxCount();         // 32: CdStream's own file table, the next wall behind this one
int Count();            // slots now - VanillaCount() until Install succeeds
bool Moved();
uintptr_t Base();       // where ms_files is now
unsigned Stride();      // 0x30, sizeof(tStreamingFileDesc)
int Used();             // entries with a name in them

}   // namespace ArchiveSlots
