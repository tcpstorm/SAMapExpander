// Makes room in CPickups::aPickUps for a region's world pickups, by moving the array somewhere
// bigger and leaving the savegame looking at exactly the first 620 entries.
//
// THE PROBLEM. aPickUps is 620 fixed entries of 0x20 bytes at 0x9788C0. San Andreas' own main
// script takes 551 of them - fifty oysters, fifty horseshoes, its own street weapons, the spray
// shops' respray pickups - and it asks for them before a region's placements go in, so a region
// meets 69 free slots. Liberty City wants 170 and Vice City 121. Even with PickupReserve at 0 they
// do not fit, and RegionPickups spent the whole budget dropping them.
//
// WHY THE ARRAY WAS LEFT ALONE UNTIL NOW, and why that objection no longer applies. CPickups::Save
// (0x5D3540) writes the array straight into the savegame - 620 entries of 0x20 bytes, one memcpy
// each - and CPickups::Load (0x5D35A0) reads them back the same way. Widen the array naively and
// the savegame block widens with it: saves this plugin makes stop being readable by vanilla San
// Andreas and by every other mod, and a save written before the change no longer lines up with the
// one written after. That is somebody's playthrough, so it is not a trade worth making.
//
// The way round it is that Save and Load do not care how big the array is. Each walks a cursor from
// a start pointer to an end pointer, and both pointers are plain 32-bit operands in the code:
//
//     CPickups::Save   mov esi, 0x9788C0 ... add esi, 0x20 ... cmp esi, 0x97D640 ... jl
//     CPickups::Load   mov esi, 0x9788C4 ... add esi, 0x20 ... cmp esi, 0x97D644 ... jl
//
// So the array is moved and enlarged, every site that walks it at run time is given the new size -
// and Save and Load are given the new base but the OLD COUNT. They keep reading and writing exactly
// 620 entries of 0x20 bytes, in the same order, from the same offsets. The savegame block is byte
// for byte what vanilla writes. A save made before this change loads afterwards; a save made
// afterwards loads in vanilla San Andreas; nothing of the user's is at risk.
//
// What that costs is that entries 620 and up are never saved - and they are exactly where a
// region's pickups go (RegionPickups blocks the low 620 while it places, so ours can only land
// above them). That is not a loss. A region's pickups are static world placements read out of
// pickups.dat and put back by RegionPickups every time a game starts, so saving them would only
// mean restoring them twice; and a savegame that did contain them would carry region model ids into
// a vanilla San Andreas that has no such models.
//
// THE TRAP. 0x97D640 is one past the last entry AND a live global variable in its own right - the
// linker put it immediately behind the array, and CPickup::Update and CPickups::Update read and
// write it. The same is true of 0x97D644, and 0x97D648..0x97D65C are floats a CRT thunk at 0x84A24C
// initialises. So "every instruction naming 0x97D640" is the wrong question. The right one is which
// instructions use those addresses as a LOOP SENTINEL - `cmp <cursor>, imm32`, an immediate - as
// against which use them as memory - `mov [0x97D640], 6`, a memory operand. The two are told apart
// by hand below and only the first kind is touched; the variables are left exactly alone.
//
// FINDING THE SITES. gta_sa.exe 1.0 US HOODLUM was disassembled with capstone: a linear sweep of
// the first .text, and of .HOODLUM, which holds the stolen-byte stubs the crack moved whole
// functions into. Five CPickups functions live there rather than in .text - Init,
// AddToCollectedPickupsArray, GetActualPickupIndex, UpdateMoneyPerDay and FindPickUpForThisObject -
// and one more instruction, CPickup::Remove's `sub eax, aPickUps`, sits in a stub at the very top of
// .text at 0x4020BB, reached by a jmp from 0x4556C5. Missing any of those would leave a loop
// walking from the new array to the old array's end: a wild sweep over megabytes of the address
// space. Every stub below was found from the branch that reaches it and re-decoded from there, not
// from a sliding byte scan, and every one of the 96 operands is checked to still hold the value
// this table expects BEFORE anything at all is written. On any mismatch the game is left completely
// vanilla and the caller turns the feature off.
#include <plugin.h>
#include <cstdint>
#include <cstdlib>
#include "PickupSlots.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kArray = 0x9788C0;     // CPickups::aPickUps
constexpr unsigned kStride = 0x20;         // sizeof(CPickup)
constexpr int kVanillaCount = 620;
constexpr uintptr_t kArrayEnd = kArray + (uintptr_t)kVanillaCount * kStride;   // 0x97D640

// CPickups::GenerateNewOne's free-slot search is unrolled ten entries at a time: it reads
// entries [i .. i+9], adds 0x140 to its cursor and compares that against a pointer one past the
// end. The loop therefore runs ceil(count / 10) times whatever the count is, and only a count that
// is a multiple of ten stops the last pass reading past the last entry. Vanilla's 620 is 62 x 10;
// ours is rounded up to keep that true.
constexpr int kScanGroup = 10;
// Spare entries on top of what the regions ask for and what the reserve keeps back, in the spirit
// of the entity pools' headroom: a mod, a script or a later region file can want a few more, and
// the whole array costs 0x20 bytes an entry.
constexpr int kMargin = 64;
// A slot index is packed into the low half of the handle CPickups::GetUniquePickupIndex builds, so
// the array can never exceed 65535 entries; this is far below that and simply refuses a silly ini.
constexpr int kMaxCount = 4000;

// ---------------------------------------------------------------- the sites
//
// `operandOffset` is where the four bytes sit inside the instruction. Each was read out of the
// executable and confirmed to be the only place in that instruction the value appears.

// A dword naming &aPickUps[0].<field>, or a fixed low entry: the array's base plus a constant.
// Rewritten to the new base plus the same constant.
struct BaseSite { uintptr_t address; uint8_t operandOffset; uint32_t oldValue; };

const BaseSite kBaseSites[] = {
    // --- the stolen-byte stub CPickup::Remove jumps to, at the very top of .text
    { 0x4020BB, 1, 0x9788C0 },   // sub eax, aPickUps          this - aPickUps, for the slot index

    { 0x455204, 1, 0x9788CC },   // CPickups::PassTime          loop start, at +0xC
    { 0x455289, 3, 0x9788DA },   // CPickups::GetUniquePickupIndex
    { 0x455471, 1, 0x9788C4 },   // CPickups::RemovePickupObjects   loop start, at +4
    { 0x4555BD, 1, 0x9788D2 },   // CPickups::TryToMerge_WeaponType loop start, at +0x12
    { 0x455664, 2, 0x9788C8 },   // CPickups::TryToMerge_WeaponType index -> pointer
    { 0x4556D2, 3, 0x9788DA },   // CPickup::Remove
    { 0x4563A9, 1, 0x9788D2 },   // CPickups::RemoveUnnecessaryPickups loop start
    { 0x456453, 1, 0x9788D2 },   // CPickups::TestForPickupsInBubble   loop start
    { 0x456A39, 2, 0x9788DA },   // CPickups::GetNewUniquePickupIndex
    { 0x456A83, 1, 0x9788D2 },   // CPickups::PictureTaken             loop start
    { 0x456BF3, 2, 0x9788C0 },   // CPickups::PictureTaken  index -> pointer
    { 0x456BFD, 1, 0x9788C0 },   // CPickups::PictureTaken  pointer -> index
    { 0x456C0A, 3, 0x9788DA },   // CPickups::PictureTaken
    { 0x456D32, 1, 0x9788D0 },   // CPickups::RemovePickUpsInArea      loop start, at +0x10
    { 0x456DE2, 1, 0x9788C4 },   // CPickups::RemoveMissionPickUps     loop start
    { 0x456E00, 3, 0x9788DA },   // CPickups::RemoveMissionPickUps
    { 0x456E6C, 1, 0x9788C4 },   // CPickups::ReInit   first loop's start
    { 0x456E82, 3, 0x9788DA },   // CPickups::ReInit
    { 0x456ED6, 1, 0x9788DA },   // CPickups::ReInit   second loop's start, at +0x1A

    // CPickups::GenerateNewOne. Three searches, each walking the type byte at +0x1C: one backwards
    // from the last entry (for the mine and mission types), then forwards for a free slot, then
    // forwards again for a slot holding a pickup it is allowed to take over.
    { 0x456F51, 1, 0x9788DC },   // backwards search: stop at &aPickUps[0].type
    { 0x456F64, 1, 0x9788FC },   // forward search 1 starts at &aPickUps[1].type
    { 0x457002, 1, 0x9788FC },   // forward search 2
    { 0x457099, 1, 0x9788FC },   // forward search 3
    { 0x45719F, 2, 0x9788C0 },   // index -> pointer, to free the old slot's object
    { 0x4571E8, 2, 0x9788C8 },   // and then the fields of the entry it settled on
    { 0x4571F3, 3, 0x9788D6 },
    { 0x4571FA, 2, 0x9788DD },
    { 0x457214, 2, 0x9788C0 },
    { 0x45721A, 2, 0x9788DC },
    { 0x457226, 2, 0x9788CC },
    { 0x45722C, 2, 0x9788DD },
    { 0x457266, 2, 0x9788DC },
    { 0x45726D, 2, 0x9788CC },
    { 0x45727D, 2, 0x9788DC },
    { 0x45728A, 2, 0x9788CC },
    { 0x45729A, 3, 0x9788D8 },   // the model index, +0x18
    { 0x4572AA, 2, 0x9788DD },
    { 0x4572C2, 2, 0x9788DD },
    { 0x4572FE, 2, 0x9788DD },
    { 0x457313, 2, 0x9788C4 },
    { 0x457319, 2, 0x9788DD },
    { 0x457344, 3, 0x9788DA },
    { 0x457354, 3, 0x9788DA },
    { 0x457366, 3, 0x9788DA },
    { 0x45736F, 3, 0x9788DA },

    { 0x4573E8, 3, 0x9788DA },   // CPickups::RemovePickUp
    { 0x4573F6, 2, 0x9788C0 },   // CPickups::RemovePickUp  index -> pointer
    { 0x458E2A, 2, 0x9788C4 },   // CPickups::Update  first slice's start
    { 0x458FE4, 2, 0x9788DC },   // CPickups::Update  second slice's start
    { 0x4590CB, 1, 0x9788C0 },   // CPickups::DetonateMinesHitByGunShot  loop start

    { 0x47AC91, 1, 0x9788C0 },   // script opcode: is there a pickup of this kind within range
    { 0x48ADC2, 1, 0x9788C0 },   // script opcode: the same shape, testing every entry
    { 0x494725, 2, 0x9788C0 },   // script opcode: a pickup's coordinates, index -> pointer
    { 0x585F08, 2, 0x9788C0 },   // CRadar::Draw3dMarkers   index -> pointer
    { 0x587110, 1, 0x9788C0 },   // CRadar::DrawEntityBlip  index -> pointer

    // The save path. These two get the new base and keep the vanilla count, which is the whole
    // point of the exercise - see the end sites below.
    { 0x5D3542, 1, 0x9788C0 },   // CPickups::Save  loop start
    { 0x5D35A2, 1, 0x9788C4 },   // CPickups::Load  loop start

    // --- whole functions HOODLUM moved out of .text into its own section
    { 0x1561589, 3, 0x9788DA },  // CPickups::AddToCollectedPickupsArray (entered from 0x455240)
    { 0x1564659, 1, 0x9788DA },  // CPickups::Init (0x454A70): clears every entry
    { 0x1564BC8, 3, 0x9788DA },  // CPickups::GetActualPickupIndex (0x4552A0)
    { 0x156B378, 3, 0x9788DA },  // CPickups::UpdateMoneyPerDay (0x455680)
    { 0x156B38B, 3, 0x9788D6 },
    { 0x156D876, 1, 0x9788C4 },  // CPickups::FindPickUpForThisObject (0x4551C0)  loop start
    { 0x156D896, 1, 0x9788C0 },  // its "nothing found" return
    { 0x156D8A0, 1, 0x9788C0 },  // its "found" return, index -> pointer
};

// The one operand that names the LAST entry rather than the first: CPickups::GenerateNewOne's
// backwards search starts at &aPickUps[619].type, so it moves with the count as well as the base.
constexpr uintptr_t kLastEntrySite = 0x456F43;
constexpr uint8_t kLastEntryOperandOffset = 1;
constexpr uint32_t kLastEntryOldValue = 0x97D63C;   // kArray + 619 * 0x20 + 0x1C
constexpr unsigned kLastEntryField = 0x1C;

// A dword naming one past the last entry - &aPickUps[620].<field> - that a loop compares its cursor
// against. `savePath` marks the two that must keep counting 620 however big the array gets.
struct EndSite { uintptr_t address; uint8_t operandOffset; unsigned field; bool savePath; };

const EndSite kEndSites[] = {
    { 0x455231, 1, 0x0C, false },   // CPickups::PassTime
    { 0x4554A9, 2, 0x04, false },   // CPickups::RemovePickupObjects
    { 0x455649, 2, 0x12, false },   // CPickups::TryToMerge_WeaponType
    { 0x456436, 2, 0x12, false },   // CPickups::RemoveUnnecessaryPickups
    { 0x4564D7, 2, 0x12, false },   // CPickups::TestForPickupsInBubble
    { 0x456BDC, 2, 0x12, false },   // CPickups::PictureTaken
    { 0x456DD1, 2, 0x10, false },   // CPickups::RemovePickUpsInArea
    { 0x456E4A, 2, 0x04, false },   // CPickups::RemoveMissionPickUps
    { 0x456EC7, 2, 0x04, false },   // CPickups::ReInit, first loop
    { 0x456EEE, 1, 0x1A, false },   // CPickups::ReInit, second loop
    // The three unrolled searches stop at &aPickUps[count + 1].type, one group of ten past the end
    // of the entries they read; that is why the count is rounded to a multiple of ten.
    { 0x456FC2, 1, 0x3C, false },   // CPickups::GenerateNewOne, forward search 1
    { 0x457059, 1, 0x3C, false },   // forward search 2
    { 0x457154, 1, 0x3C, false },   // forward search 3
    { 0x4590E2, 2, 0x00, false },   // CPickups::DetonateMinesHitByGunShot
    { 0x156466E, 1, 0x1A, false },  // CPickups::Init, in HOODLUM's section
    { 0x156D88E, 2, 0x04, false },  // CPickups::FindPickUpForThisObject, likewise

    // And the two that stay at 620 for ever.
    { 0x5D3555, 2, 0x00, true },    // CPickups::Save
    { 0x5D35D2, 2, 0x04, true },    // CPickups::Load
};

// A dword immediate that is the entry count itself, one less than it, or the array's size in bytes.
enum CountKind { kCount, kCountLessOne, kCountBytes };
struct CountSite { uintptr_t address; uint8_t operandOffset; CountKind kind; };

const CountSite kCountSites[] = {
    { 0x456F3E, 1, kCountLessOne },  // GenerateNewOne: the backwards search's starting index
    { 0x456FF4, 2, kCount },         // GenerateNewOne: "did search 1 find one before the end"
    { 0x45708B, 2, kCount },         // search 2
    { 0x45718E, 2, kCount },         // search 3
    { 0x4571D1, 2, kCount },         // and the last check before it writes the entry
    // CPickups::Update does not walk the array; it does a slice of it per frame - (frame & 31) *
    // count / 32 up to ((frame & 31) + 1) * count / 32 for one pass, and sixths of it for the
    // other. These four are that count. Leave them at 620 and every entry above the old end is
    // never updated: our pickups would stand there, and never be collectable.
    { 0x458DF7, 2, kCount },         // CPickups::Update, first pass: imul ecx, ecx, count
    { 0x458E0B, 2, kCount },         //                              lea eax, [ecx + count]
    { 0x458FB0, 2, kCount },         // second pass
    { 0x458FBF, 2, kCount },
    // Two script opcodes walk the array by byte offset instead of by pointer, so their bound is the
    // array's size rather than its length.
    { 0x47ACF1, 2, kCountBytes },
    { 0x48AE3D, 2, kCountBytes },
};

uint8_t* gBlock = nullptr;
int gCount = kVanillaCount;

uint32_t EndValue(const EndSite& s, uintptr_t base, int count) {
    return (uint32_t)(base + (uintptr_t)count * kStride + s.field);
}

uint32_t CountValue(const CountSite& s, int count) {
    switch (s.kind) {
        case kCountLessOne: return (uint32_t)(count - 1);
        case kCountBytes:   return (uint32_t)((unsigned)count * kStride);
        default:            return (uint32_t)count;
    }
}

// Read a site's operand and say whether it is still what this table expects. Nothing is written
// until every one of them has answered yes.
bool Holds(uintptr_t address, uint8_t operandOffset, uint32_t expected, const char* what) {
    uint32_t have = (uint32_t)patch::GetUInt(address + operandOffset);
    if (have == expected) return true;
    Log::Write("ERROR: the pickup array was not enlarged: %s at 0x%X holds 0x%08X, expected 0x%08X "
               "(another mod has patched it, or this is not the 1.0 US executable); San Andreas' "
               "pickups are left exactly as they are and the region's are left out",
               what, (unsigned)(address + operandOffset), have, expected);
    return false;
}

}   // namespace

namespace PickupSlots {

int VanillaCount() { return kVanillaCount; }
bool Grown() { return gBlock != nullptr; }
uintptr_t Base() { return gBlock ? (uintptr_t)gBlock : kArray; }
int Count() { return gCount; }

bool Grow(int regionPlacements, int reserve) {
    if (gBlock) return true;              // already done; Install is only meant to run once
    if (regionPlacements <= 0) return false;
    if (reserve < 0) reserve = 0;

    // San Andreas' own 620, every placement the regions asked for, the slots the reserve keeps back
    // so its script can still have its oysters and horseshoes afterwards, and a little headroom -
    // then rounded up to the group of ten GenerateNewOne's search reads at a time.
    int wanted = kVanillaCount + regionPlacements + reserve + kMargin;
    wanted = ((wanted + kScanGroup - 1) / kScanGroup) * kScanGroup;
    if (wanted > kMaxCount) {
        wanted = (kMaxCount / kScanGroup) * kScanGroup;
        Log::Write("pickup slots: %d entries asked for is more than this will go to; capped at %d",
                   kVanillaCount + regionPlacements + reserve + kMargin, wanted);
    }
    if (wanted <= kVanillaCount) return false;

    for (const BaseSite& s : kBaseSites)
        if (!Holds(s.address, s.operandOffset, s.oldValue, "the array's address")) return false;
    if (!Holds(kLastEntrySite, kLastEntryOperandOffset, kLastEntryOldValue,
               "the last entry's address")) return false;
    for (const EndSite& s : kEndSites)
        if (!Holds(s.address, s.operandOffset, EndValue(s, kArray, kVanillaCount),
                   "the end of the array")) return false;
    for (const CountSite& s : kCountSites)
        if (!Holds(s.address, s.operandOffset, CountValue(s, kVanillaCount),
                   "the number of entries")) return false;

    // Zeroed, because that is what the loader gives the vanilla array and what CPickups::Init
    // expects to find; Init then writes every entry itself, over the whole new length.
    uint8_t* block = (uint8_t*)calloc((size_t)wanted, kStride);
    if (!block) {
        Log::Write("ERROR: the pickup array was not enlarged: cannot allocate %d entries; the "
                   "region's pickups are left out", wanted);
        return false;
    }

    const uintptr_t base = (uintptr_t)block;
    for (const BaseSite& s : kBaseSites)
        patch::SetUInt(s.address + s.operandOffset, (unsigned)(base + (s.oldValue - kArray)));
    patch::SetUInt(kLastEntrySite + kLastEntryOperandOffset,
                   (unsigned)(base + (uintptr_t)(wanted - 1) * kStride + kLastEntryField));
    for (const EndSite& s : kEndSites)
        patch::SetUInt(s.address + s.operandOffset,
                       EndValue(s, base, s.savePath ? kVanillaCount : wanted));
    for (const CountSite& s : kCountSites)
        patch::SetUInt(s.address + s.operandOffset, CountValue(s, wanted));

    gBlock = block;
    gCount = wanted;
    Log::Write("pickup slots: the array is %d -> %d entries at %p (%d for San Andreas, %d asked for "
               "by the regions, %d kept in reserve, %d spare); %u site(s) patched",
               kVanillaCount, wanted, (void*)block, kVanillaCount, regionPlacements, reserve,
               wanted - kVanillaCount - regionPlacements - reserve,
               (unsigned)(sizeof(kBaseSites) / sizeof(kBaseSites[0]) + 1 +
                          sizeof(kEndSites) / sizeof(kEndSites[0]) +
                          sizeof(kCountSites) / sizeof(kCountSites[0])));
    Log::Write("pickup slots: the save game still holds exactly the first %d entries, so it stays "
               "byte for byte what vanilla San Andreas writes; the region's pickups go above them "
               "and are put back from pickups.dat every time a game starts", kVanillaCount);
    return true;
}

}   // namespace PickupSlots
