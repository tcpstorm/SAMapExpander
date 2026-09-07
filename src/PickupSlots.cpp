// Gives San Andreas more pickup slots, so a region's own can go in beside its.
//
// CPickups::aPickUps is 620 entries of 0x20 bytes at 0x9788C0, and San Andreas' own main script
// takes 551 of them - its oysters, its horseshoes, its photo opportunities, the guns at its
// safehouses - before a region is asked for anything. That leaves 69, and the two shipped regions
// between them want 291. Every one of them was refused, and no amount of lowering the reserve
// changes it: the array is simply not big enough for two cities as well as San Andreas.
//
// So move it to the heap and make it longer. The array is only ever addressed absolutely - there is
// no pointer to repoint, every access has 0x9788C0 plus a field offset written into the instruction
// - so relocating it means finding those instructions and rewriting the address in each. Which is
// the same surgery ModelLimits does for the streaming tables and CityLoader does for the object
// data, and it follows the same rule they do: every site is checked to still hold the exact value
// expected before a single byte is written, and if any one of them does not, nothing is written at
// all and the feature turns itself off.
//
//
// FINDING THE SITES
//
// Scanning the executable for any four bytes that happen to fall inside the array's address range
// finds 107 candidates, and most of them are wreckage - byte sequences that land in range by
// accident, in the middle of instructions that have nothing to do with pickups. Decoding backwards
// from each one does not settle it either, because x86 has no way of knowing where an instruction
// starts from its middle: CCollision::ProcessColModels' "hit" disassembles to `ror byte ptr [edi],
// 0x8e` and CPathFind's to `les eax, ptr [ecx + 0xf]`, neither of which is a real instruction.
//
// Disassembling linearly from each function's own start, which does know where instructions begin,
// gives 58 genuine references across 23 functions. They are listed below with the field offset each
// one carries, and each is rewritten by looking for that exact address in the handful of bytes at
// the site and replacing it - which needs no instruction decoding at all and refuses to write if
// what it expects is not there.
//
//
// THE ONE THAT WOULD HAVE BEEN A DISASTER
//
// 0x97D640 is one byte past the end of the array, so it is what the loops compare against to know
// they have finished. It is ALSO a live global variable in its own right, sitting immediately
// behind the array, and four instructions read and write it as one - CPickup::Update stores 6 into
// it twice. Rewriting those four as though they were end-of-array tests would silently corrupt
// whatever that variable is for. Only the two genuine bounds tests are touched; the four variable
// accesses are named below and deliberately left exactly as they are.
//
//
// SAVEGAMES
//
// CPickups::Save walks the array 0x20 bytes at a time until it reaches the end and writes each
// entry; CPickups::Load reads them back the same way. Both are pointed at the new array but keep
// their old count, so they still write and read exactly 620 entries - the savegame's layout does
// not change by a byte, and a save made with this loaded still opens without it.
//
// That works because of where the region's pickups live: above the vanilla 620, in the space that
// only exists because of this file. San Andreas' own pickups stay in the first 620 and are saved
// and restored exactly as before, while the region's are put back by RegionPickups at the start of
// every session anyway - they are placements read from a file, not state worth persisting.
#include <plugin.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "PickupSlots.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kArray = 0x9788C0;      // CPickups::aPickUps
constexpr int kVanillaCount = 620;
constexpr int kEntry = 0x20;
constexpr uintptr_t kArrayEnd = kArray + (uintptr_t)kVanillaCount * kEntry;   // 0x97D640

// How many slots to add beyond what the regions ask for, so San Andreas' script can still create
// pickups of its own mid-game - mission items, respawns - after the regions have had theirs.
constexpr int kHeadroom = 256;
// A region file that asked for a hundred thousand slots would be a mistake, not a request.
constexpr int kMaxCount = 8192;

// Every instruction that names the array, as { address, field offset within an entry }. Found by
// disassembling each function linearly from its own start; see the note above about why a plain
// byte scan will not do. The offsets are the fields of a CPickup - 0x00 position, 0x0C, 0x18 model,
// 0x1A index, 0x1C type, 0x1D - and one end-of-array sentinel at 0x4D7C.
const struct { uintptr_t at; unsigned offset; } kSites[] = {
    { 0x4556D2, 0x01A }, { 0x4590CB, 0x000 }, { 0x456F51, 0x01C },
    { 0x456F64, 0x03C }, { 0x457002, 0x03C }, { 0x457099, 0x03C }, { 0x45719F, 0x000 },
    { 0x4571E8, 0x008 }, { 0x4571F3, 0x016 }, { 0x4571FA, 0x01D }, { 0x457214, 0x000 },
    { 0x45721A, 0x01C }, { 0x457226, 0x00C }, { 0x45722C, 0x01D }, { 0x457266, 0x01C },
    { 0x45726D, 0x00C }, { 0x45727D, 0x01C }, { 0x45728A, 0x00C }, { 0x45729A, 0x018 },
    { 0x4572AA, 0x01D }, { 0x4572C2, 0x01D }, { 0x4572FE, 0x01D }, { 0x457313, 0x004 },
    { 0x457319, 0x01D }, { 0x457344, 0x01A }, { 0x457354, 0x01A }, { 0x457366, 0x01A },
    { 0x45736F, 0x01A }, { 0x456A39, 0x01A }, { 0x455289, 0x01A }, { 0x5D35A2, 0x004 },
    { 0x455204, 0x00C }, { 0x456A83, 0x012 }, { 0x456BF3, 0x000 }, { 0x456BFD, 0x000 },
    { 0x456C0A, 0x01A }, { 0x456E6C, 0x004 }, { 0x456E82, 0x01A }, { 0x456ED6, 0x01A },
    { 0x456DE2, 0x004 }, { 0x456E00, 0x01A }, { 0x4573E8, 0x01A }, { 0x4573F6, 0x000 },
    { 0x456D32, 0x010 }, { 0x455471, 0x004 }, { 0x4563A9, 0x012 }, { 0x5D3542, 0x000 },
    { 0x456453, 0x012 }, { 0x4555BD, 0x012 }, { 0x455664, 0x008 }, { 0x458E2A, 0x004 },
    { 0x458FE4, 0x01C }, { 0x585F08, 0x000 }, { 0x587110, 0x000 }, { 0x47AC91, 0x000 },
    { 0x494725, 0x000 }, { 0x48ADC2, 0x000 },
};

// The count, written into instructions as a plain number rather than an address.
//
// CPickups::GenerateNewOne searches for a free slot and compares the index against 620 four times,
// having started it at 619. CPickups::Update is not a loop over the array at all: it takes the
// frame counter modulo 32, multiplies by 620 and divides by 32, so each frame it updates a
// thirty-second of the array. That 620 is the count as much as the others are, and if it is left
// alone every pickup above the old end is never updated - never collected, never respawned, never
// drawn. Which would look exactly like the pickups not being there.
const struct { uintptr_t at; int value; } kCounts[] = {
    { 0x456F3E, kVanillaCount - 1 },   // CPickups::GenerateNewOne  mov ebx, 619
    { 0x456FF4, kVanillaCount },       // CPickups::GenerateNewOne  cmp ebx, 620
    { 0x45708B, kVanillaCount },       // CPickups::GenerateNewOne  cmp ebx, 620
    { 0x45718E, kVanillaCount },       // CPickups::GenerateNewOne  cmp ebx, 620
    { 0x4571D1, kVanillaCount },       // CPickups::GenerateNewOne  cmp ebx, 620
    { 0x458DF7, kVanillaCount },       // CPickups::Update  imul ecx, ecx, 620
    { 0x458E0B, kVanillaCount },       // CPickups::Update  lea eax, [ecx + 620]
    { 0x458FB0, kVanillaCount },       // CPickups::Update  imul ecx, ecx, 620
    { 0x458FBF, kVanillaCount },       // CPickups::Update  add ecx, 620
};

// The two end-of-array comparisons that are genuinely that, and what each becomes.
//
//   CPickups::Save     stays at 620 entries, so the savegame is unchanged - see the note above.
//   DetonateMinesHitByGunShot  walks the whole array looking for mines, so it takes the new end.
//
// CPickups::Load's bound is the same test one field along, 0x97D644, and stays at 620 with Save.
//
// NOT in this list, and deliberately: 0x457ACE and 0x457CC3 in CPickup::Update, and 0x458EE1 and
// 0x458EF5 in CPickups::Update. Those four read and write 0x97D640 as a variable, not as the end of
// anything. Rewriting them would corrupt it.
constexpr uintptr_t kSaveBound = 0x5D3555;      // cmp esi, 0x97D640  - keep to 620
constexpr uintptr_t kLoadBound = 0x5D35D2;      // cmp esi, 0x97D644  - keep to 620
constexpr uintptr_t kMinesBound = 0x4590E2;     // cmp esi, 0x97D640  - the whole array

// Where CPickups::GenerateNewOne starts looking, and it is not a field offset however much it looks
// like one.
//
// The search runs backwards: `mov ebx, 619` sets the index, `mov eax, 0x97D63C` sets a pointer at
// the type byte of entry 619 - 619 * 0x20 + 0x1C - and the two walk down together until the pointer
// reaches entry 0's type at 0x9788DC. Treating 0x4D7C as a fixed offset and repointing it like any
// other field leaves the pointer starting at entry 619 while the index starts at 1166, so the two
// disagree by 547 the whole way down: the search finds a free slot and reports the wrong index for
// it. That is what "the game refused" meant with 1157 slots standing empty.
//
// So it scales with the count, like the index does.
constexpr uintptr_t kSearchStart = 0x456F43;    // mov eax, &aPickUps[619].type

uint8_t* gArray = nullptr;
int gCount = 0;

// Find `want` in the few bytes at `at` and say where, or 0. The window is a whole instruction's
// worth; an x86 instruction cannot be longer than 15 bytes and every site here is far shorter.
uintptr_t FindOperand(uintptr_t at, uint32_t want) {
    for (unsigned i = 0; i <= 10; i++) {
        if (IsBadReadPtr((void*)(at + i), 4)) return 0;
        if (*(const uint32_t*)(at + i) == want) return at + i;
    }
    return 0;
}

bool Verify() {
    for (const auto& s : kSites)
        if (!FindOperand(s.at, (uint32_t)(kArray + s.offset))) {
            Log::Write("pickup slots: %08X does not hold %08X; leaving the array alone",
                       (unsigned)s.at, (unsigned)(kArray + s.offset));
            return false;
        }
    for (const auto& c : kCounts)
        if (!FindOperand(c.at, (uint32_t)c.value)) {
            Log::Write("pickup slots: %08X does not hold %d; leaving the array alone",
                       (unsigned)c.at, c.value);
            return false;
        }
    if (!FindOperand(kSearchStart, (uint32_t)(kArray + (uintptr_t)(kVanillaCount - 1) * kEntry + 0x1C)) ||
        !FindOperand(kSaveBound, (uint32_t)kArrayEnd) ||
        !FindOperand(kLoadBound, (uint32_t)(kArrayEnd + 4)) ||
        !FindOperand(kMinesBound, (uint32_t)kArrayEnd)) {
        Log::Write("pickup slots: a bounds test is not where it should be; leaving the array alone");
        return false;
    }
    return true;
}

void Write32(uintptr_t at, uint32_t value) {
    DWORD old = 0;
    VirtualProtect((void*)at, 4, PAGE_EXECUTE_READWRITE, &old);
    *(uint32_t*)at = value;
    VirtualProtect((void*)at, 4, old, &old);
}

// How many the regions between them are asking for. Counted from the files rather than from
// RegionPickups, because this has to run before the game's own start-up and RegionPickups reads
// them later; a line that is not a comment is a placement.
int RegionDemand() {
    int total = 0;
    for (const MapRegion& r : Settings::regions) {
        if (r.pickupFile.empty()) continue;
        FILE* f = fopen(r.pickupFile.c_str(), "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != ';' && *p != '#' && *p != '\n' && *p != '\r') total++;
        }
        fclose(f);
    }
    return total;
}

}   // namespace

namespace PickupSlots {

int Count() { return gCount; }
uintptr_t Array() { return gArray ? (uintptr_t)gArray : kArray; }

void Install() {
    if (!Settings::world.regionPickups) return;

    // Under SA-MP the server owns the pickups and places them itself, so there is nothing here for
    // a region to add and no reason to touch the game's array at all.
    if (Settings::RunningUnderSaMp()) return;

    const int demand = RegionDemand();
    if (!demand) return;

    int count = kVanillaCount + demand + kHeadroom;
    if (count > kMaxCount) {
        Log::Write("pickup slots: %d wanted, which is past the %d this will go to; capped",
                   count, kMaxCount);
        count = kMaxCount;
    }

    if (!Verify()) return;

    gArray = (uint8_t*)VirtualAlloc(nullptr, (size_t)count * kEntry, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (!gArray) {
        Log::Write("pickup slots: could not allocate %d entries; leaving the array alone", count);
        return;
    }
    // The game clears the array itself in CPickups::Init, but only over the count it knows about,
    // and that is now this one - VirtualAlloc hands back zeroed pages regardless, so an entry the
    // game never touches reads as the empty type rather than as whatever was in memory.
    const uintptr_t base = (uintptr_t)gArray;

    for (const auto& s : kSites)
        Write32(FindOperand(s.at, (uint32_t)(kArray + s.offset)), (uint32_t)(base + s.offset));
    for (const auto& c : kCounts)
        Write32(FindOperand(c.at, (uint32_t)c.value),
                (uint32_t)(c.value == kVanillaCount - 1 ? count - 1 : count));

    // The backwards search starts at the last entry there now is, in step with its index.
    Write32(FindOperand(kSearchStart, (uint32_t)(kArray + (uintptr_t)(kVanillaCount - 1) * kEntry + 0x1C)),
            (uint32_t)(base + (uintptr_t)(count - 1) * kEntry + 0x1C));

    // Save and Load keep to the first 620 entries, so the savegame is byte for byte what it was.
    Write32(FindOperand(kSaveBound, (uint32_t)kArrayEnd),
            (uint32_t)(base + (uintptr_t)kVanillaCount * kEntry));
    Write32(FindOperand(kLoadBound, (uint32_t)(kArrayEnd + 4)),
            (uint32_t)(base + 4 + (uintptr_t)kVanillaCount * kEntry));
    // The mine sweep is a runtime walk of everything there is.
    Write32(FindOperand(kMinesBound, (uint32_t)kArrayEnd),
            (uint32_t)(base + (uintptr_t)count * kEntry));

    gCount = count;
    Log::Write("pickup slots: %d -> %d entries at %08X (%d for the regions, %d spare for San "
               "Andreas' script); %d site(s) and %d count(s) repointed, saves still hold %d",
               kVanillaCount, count, (unsigned)base, demand, count - kVanillaCount - demand,
               (int)(sizeof(kSites) / sizeof(kSites[0])) + 3,
               (int)(sizeof(kCounts) / sizeof(kCounts[0])), kVanillaCount);
}

}   // namespace PickupSlots
