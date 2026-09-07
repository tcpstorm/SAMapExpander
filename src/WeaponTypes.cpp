// Widens San Andreas' weapon system so a gun can be ADDED to it rather than written over one of
// its own. This is the answer to the note that used to stand in RegionWeapons.cpp - that a new
// weapon type would draw in the player's hand and then fire nothing, in silence - which was half
// right. The switches on weapon type really do end in `ja default`, but every one of them is a
// jump table, and a jump table can be made longer.
//
// -------------------------------------------------------------------- where the type numbers come from
//
// CWeaponInfo::aWeaponInfo at 0xC8AAB8 is eighty records of 0x70 bytes - CWeaponInfo::Initialise
// (0x5BF750) walks it from 0xC8AABC to 0xC8CDBC, which is 80 * 0x70 exactly - and
// CWeaponInfo::GetWeaponInfo (0x743C60) indexes it as `type + {POOR 25, STD 0, PRO 36, COP 47}`
// with no bounds check. San Andreas' own weapons occupy:
//
//   0..46    the STD record of every real weapon, UNARMED to PARACHUTE
//   47..57   the POOR records of PISTOL..TEC9    (22..32, plus 25)
//   58..68   their PRO records                   (22..32, plus 36)
//   69       the COP record of PISTOL            (22, plus 47)
//   70..79   never written by any weapon.dat San Andreas ships
//
// So there are ten free records at the end, and ten is exactly how many guns Vice City has that
// San Andreas has not. No relocation of aWeaponInfo and no repointing of the skill offsets is
// needed to reach them, which is a large piece of surgery avoided: a type of 70 is a STD record at
// index 70, and CWeaponInfo::LoadWeaponData (0x5BE9C2) computes that index itself, the same way,
// so the game's own parser will fill our records if it is ever asked to.
//
// The one thing that stops any lower number being used is that eWeaponType does not stop at 46.
// 47 is LAST_WEAPON, 48 is ARMOUR, and 49..58 are the pseudo weapons the damage system uses to say
// how somebody died - RAMMEDBYCAR, RUNOVERBYCAR, EXPLOSION, DROWNING, FALL and the rest. A new gun
// must sit above those or it would be mistaken for one of them, so 70..79 is not merely convenient,
// it is the first block that is genuinely free.
//
// -------------------------------------------------------------------- what has to be widened
//
// CWeaponInfo::FindWeaponType (0x743D10) walks a fixed table of 49 char* at 0x8D6150 and answers 0
// - UNARMED - for a name it does not know. Exactly one instruction names that table, so it is
// copied to an eighty-entry array on the heap and the loop bound `cmp esi, 0x30` is raised.
//
// Then the switches. Each is the same shape: subtract the first type it covers, `cmp` against the
// span, `ja` to a default, and jump through a table. Three of them read a byte index table first,
// so that many types can share one case; two index the jump table directly.
//
//   CWeapon::Fire            0x742461  types 16..43   default 0x742794   the one that matters
//   CWeapon::Fire, tail      0x742874  types 16..40   default 0x74289F   the "a gun went off" bit
//   CWeapon::FireInstantHit  0x740C75  types 22..38   default 0x740E3A   per-gun accuracy
//   WeaponFire audio         0x504F96  types 22..45   default 0x50523F   the gunshot
//   WeaponReload audio       0x5036F2  types 22..34   default 0x503830   the reload click
//
// Only the first is fatal on its own: its default is the case ROCKET and FREEFALL_BOMB take, which
// fires nothing, so a new type would indeed be a silent, harmless prop. The other four degrade -
// the country rifle's accuracy, no gunshot, no reload click - which is where the "fires blanks in
// silence" reading came from. All five are widened here in the same way: both tables are copied to
// the heap with room for types up to 79, the `cmp` bound is raised, and the two disp32 operands are
// repointed. A new type is then routed to its donor's case, so a Python takes exactly the code path
// the Desert Eagle takes while keeping its own record, its own model and its own name.
//
// Two other bounds are raised: CPickup::Update (0x457A3B) refuses to treat a model as a weapon
// pickup unless its type is below 47, which would leave the guns lying in Vice City's streets
// inert; and CWeapon::IsType2Handed (0x73B1E0) is a hardcoded list of seven types, so it is hooked
// rather than patched, to answer for the donor.
//
// -------------------------------------------------------------------- what is deliberately left alone
//
// Events::ComputeDamageResponse (0x4C0327) and Events::ComputeDamageAnim (0x4B44BB) both branch on
// `weapon type >= 47` into the arm that handles the pseudo weapons above, and that bound cannot be
// raised without sending RAMMEDBYCAR and EXPLOSION down the real-weapon arm with it. So a ped shot
// with one of the new guns takes the damage - that is computed from the weapon's own record and
// never reaches these - but plays the generic hit and death reactions rather than the pistol or
// shotgun ones. CDarkel (0x43D7B6) will likewise not hand a new gun back after a rampage.
//
// The HUD icon is not patchable from here at all: CHud::DrawWeaponIcon (0x58D7D0) builds a texture
// key from the weapon model's name with "ICON" appended and looks it up in the model's own texture
// dictionary - desert_eagle.txd holds desert_eagleicon - and an imported weapon's dictionary has no
// such texture, so the wheel shows the gun with no icon. That is the importer's to fix, by adding
// one when it copies the dictionary.
//
// Every site is checked against the exact bytes San Andreas ships before anything is written, and
// if a single one has been patched by another mod nothing at all is done and the game stays
// vanilla. None of it runs under SA-MP; the caller sees to that.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "WeaponTypes.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr int kFirstNewType = 70;
constexpr int kLastNewType = 79;
constexpr int kNewTypeCount = kLastNewType - kFirstNewType + 1;

constexpr uintptr_t kWeaponInfoArray = 0xC8AAB8;
constexpr unsigned kWeaponInfoStride = 0x70;
constexpr unsigned kSlotField = 0x14;      // CWeaponInfo::m_nWeaponSlot, -1 until a line writes it

// CWeaponInfo::ms_aWeaponNames. 49 entries, of which 47 is the empty string that stands for
// LAST_WEAPON; the game's own array cannot be grown in place because unrelated data follows it.
constexpr uintptr_t kNameTable = 0x8D6150;
constexpr int kVanillaNames = 49;
constexpr uintptr_t kEmptyString = 0x858B54;   // the "" the vanilla table already uses at index 47

// CWeaponInfo::FindWeaponType: `mov eax, [esi*4 + 0x8D6150]` then `cmp esi, 0x30`.
constexpr uintptr_t kNameLoadAt = 0x743D20;
const uint8_t kNameLoadOp[3] = { 0x8B, 0x04, 0xB5 };
constexpr uintptr_t kNameBoundAt = 0x743D36;
const uint8_t kNameBoundOp[3] = { 0x83, 0xFE, 0x30 };

// CPickup::Update: `cmp edi, 0x2f` / `jge`, the guard that says a pickup's model is a weapon.
constexpr uintptr_t kPickupBoundAt = 0x457A3B;
const uint8_t kPickupBoundOp[3] = { 0x83, 0xFF, 0x2F };

// CWeapon::IsType2Handed, whose first five bytes are `mov eax,[ecx]` and `cmp eax, 0x1f`.
constexpr uintptr_t kIsType2Handed = 0x73B1E0;
const uint8_t kIsType2HandedOp[5] = { 0x8B, 0x01, 0x83, 0xF8, 0x1F };

// ---------------------------------------------------------------- the switches on weapon type

struct Dispatch {
    const char* what;
    uintptr_t cmpAt;              // `cmp reg, imm8` - the span the switch covers
    uint8_t   cmpOp[2];           // 83 F8 for eax, 83 F9 for ecx
    uint8_t   oldBound;
    uintptr_t jaAt;               // the `ja default` that goes with it
    uintptr_t expectedDefault;    // where that jump lands, checked rather than assumed
    uintptr_t indexAt;            // `movzx r32, byte ptr [r32 + disp32]`, or 0 when there is none
    uint8_t   indexOp[3];
    uintptr_t indexTable;
    uintptr_t jumpAt;             // `jmp dword ptr [r32*4 + disp32]`
    uint8_t   jumpOp[3];
    uintptr_t jumpTable;
    int       firstType;          // the weapon type the switch's index 0 stands for
};

// The five sites, with the exact bytes 1.0 US ships at each. `indexAt` of 0 means the jump table is
// indexed by the type directly, one entry per type, rather than through a byte table of cases.
const Dispatch kDispatch[] = {
    { "CWeapon::Fire",
      0x742461, { 0x83, 0xF8 }, 0x1B, 0x742469, 0x742794,
      0x74246F, { 0x0F, 0xB6, 0x90 }, 0x742A30,
      0x742476, { 0xFF, 0x24, 0x95 }, 0x742A0C, 16 },

    { "CWeapon::Fire (the shot's noise and stat)",
      0x742874, { 0x83, 0xF8 }, 0x18, 0x742877, 0x74289F,
      0x742879, { 0x0F, 0xB6, 0x80 }, 0x742A58,
      0x742880, { 0xFF, 0x24, 0x85 }, 0x742A4C, 16 },

    { "CWeapon::FireInstantHit",
      0x740C75, { 0x83, 0xF8 }, 0x10, 0x740C78, 0x740E3A,
      0x740C7E, { 0x0F, 0xB6, 0x90 }, 0x741348,
      0x740C85, { 0xFF, 0x24, 0x95 }, 0x741334, 22 },

    { "the gunshot audio",
      0x504F96, { 0x83, 0xF9 }, 0x17, 0x504F99, 0x50523F,
      0, { 0, 0, 0 }, 0,
      0x504F9F, { 0xFF, 0x24, 0x8D }, 0x505244, 22 },

    { "the reload audio",
      0x5036F2, { 0x83, 0xF8 }, 0x0C, 0x5036F5, 0x503830,
      0, { 0, 0, 0 }, 0,
      0x5036FB, { 0xFF, 0x24, 0x85 }, 0x503838, 22 },
};

constexpr int kDispatchCount = (int)(sizeof(kDispatch) / sizeof(kDispatch[0]));

struct Widened {
    uint8_t*  index = nullptr;    // our byte index table, when the switch has one
    uint32_t* jump = nullptr;     // our jump table
    int       cases = 0;          // entries copied from the vanilla jump table
    int       defaultCase = 0;    // the entry we appended, holding the default target
};

Widened gWidened[kDispatchCount];

// ---------------------------------------------------------------- state

const char** gNames = nullptr;    // our eighty-entry replacement for ms_aWeaponNames
bool gReady = false;
int gNext = kFirstNewType;
int gDonor[kNewTypeCount];
bool gWantTwoHanded = false;

// ---------------------------------------------------------------- verification

bool BytesAre(uintptr_t address, const uint8_t* expected, size_t length) {
    for (size_t i = 0; i < length; i++)
        if (patch::GetUChar(address + i) != expected[i]) return false;
    return true;
}

void Refuse(const char* what, uintptr_t address) {
    Log::Write("weapon types: 0x%X is not what San Andreas 1.0 US ships (%s) - another mod has "
               "patched it, so nothing is widened and the weapon system is left exactly as it was",
               (unsigned)address, what);
}

// Where a `ja` lands, whether it is the short form or the near one. 0 if it is not a `ja` at all,
// which is itself a reason to refuse.
uintptr_t JumpAboveTarget(uintptr_t at) {
    const uint8_t first = patch::GetUChar(at);
    if (first == 0x77) return at + 2 + (int8_t)patch::GetUChar(at + 1);
    if (first == 0x0F && patch::GetUChar(at + 1) == 0x87)
        return at + 6 + (int32_t)patch::GetUInt(at + 2);
    return 0;
}

bool VerifyDispatch(const Dispatch& d) {
    const uint8_t cmpBytes[3] = { d.cmpOp[0], d.cmpOp[1], d.oldBound };
    if (!BytesAre(d.cmpAt, cmpBytes, 3)) { Refuse(d.what, d.cmpAt); return false; }
    if (JumpAboveTarget(d.jaAt) != d.expectedDefault) { Refuse(d.what, d.jaAt); return false; }
    if (d.indexAt) {
        if (!BytesAre(d.indexAt, d.indexOp, 3) ||
            patch::GetUInt(d.indexAt + 3) != (unsigned)d.indexTable) {
            Refuse(d.what, d.indexAt);
            return false;
        }
    }
    if (!BytesAre(d.jumpAt, d.jumpOp, 3) || patch::GetUInt(d.jumpAt + 3) != (unsigned)d.jumpTable) {
        Refuse(d.what, d.jumpAt);
        return false;
    }
    return true;
}

bool VerifyEverything() {
    if (!BytesAre(kNameLoadAt, kNameLoadOp, 3) ||
        patch::GetUInt(kNameLoadAt + 3) != (unsigned)kNameTable) {
        Refuse("CWeaponInfo::FindWeaponType's name table", kNameLoadAt);
        return false;
    }
    if (!BytesAre(kNameBoundAt, kNameBoundOp, 3)) {
        Refuse("CWeaponInfo::FindWeaponType's loop bound", kNameBoundAt);
        return false;
    }
    if (!BytesAre(kPickupBoundAt, kPickupBoundOp, 3)) {
        Refuse("CPickup::Update's weapon test", kPickupBoundAt);
        return false;
    }
    for (const Dispatch& d : kDispatch)
        if (!VerifyDispatch(d)) return false;
    return true;
}

// ---------------------------------------------------------------- widening

bool WidenNameTable() {
    gNames = (const char**)calloc(kLastNewType + 1, sizeof(const char*));
    if (!gNames) return false;
    for (int i = 0; i < kVanillaNames; i++)
        gNames[i] = *(const char* const*)(kNameTable + (uintptr_t)i * 4);
    // Everything past San Andreas' own names answers to nothing until a gun claims it. The empty
    // string the vanilla table already keeps at index 47 is reused, so a stray lookup of "" still
    // finds 47 first and behaves exactly as it did.
    for (int i = kVanillaNames; i <= kLastNewType; i++) gNames[i] = (const char*)kEmptyString;

    patch::SetUInt(kNameLoadAt + 3, (unsigned)(uintptr_t)gNames);
    patch::SetUChar(kNameBoundAt + 2, (unsigned char)kLastNewType);
    return true;
}

bool WidenDispatch(const Dispatch& d, Widened& w) {
    const int oldSpan = d.oldBound + 1;
    const int newSpan = kLastNewType - d.firstType + 1;

    if (d.indexAt) {
        // How many cases the vanilla jump table has is not written down anywhere, so it is read off
        // the byte table: the highest case index it names, plus one.
        int cases = 0;
        for (int i = 0; i < oldSpan; i++) {
            const int c = patch::GetUChar(d.indexTable + i);
            if (c + 1 > cases) cases = c + 1;
        }
        w.cases = cases;
        w.defaultCase = cases;
        w.jump = (uint32_t*)calloc((size_t)cases + 1, sizeof(uint32_t));
        w.index = (uint8_t*)calloc((size_t)newSpan, 1);
        if (!w.jump || !w.index) return false;
        for (int i = 0; i < cases; i++) w.jump[i] = patch::GetUInt(d.jumpTable + (uintptr_t)i * 4);
        // One case of our own, holding wherever the `ja` used to go, so that every type we have not
        // claimed still behaves precisely as it did before the bound was raised.
        w.jump[cases] = (uint32_t)d.expectedDefault;
        for (int i = 0; i < oldSpan; i++) w.index[i] = patch::GetUChar(d.indexTable + i);
        for (int i = oldSpan; i < newSpan; i++) w.index[i] = (uint8_t)w.defaultCase;
        patch::SetUInt(d.indexAt + 3, (unsigned)(uintptr_t)w.index);
    } else {
        w.jump = (uint32_t*)calloc((size_t)newSpan, sizeof(uint32_t));
        if (!w.jump) return false;
        for (int i = 0; i < oldSpan; i++) w.jump[i] = patch::GetUInt(d.jumpTable + (uintptr_t)i * 4);
        for (int i = oldSpan; i < newSpan; i++) w.jump[i] = (uint32_t)d.expectedDefault;
    }

    patch::SetUInt(d.jumpAt + 3, (unsigned)(uintptr_t)w.jump);
    patch::SetUChar(d.cmpAt + 2, (unsigned char)(kLastNewType - d.firstType));
    return true;
}

// Point one new type at the case its donor uses. A donor outside the switch's span - a melee weapon
// where the switch starts at PISTOL, say - keeps the default, which is what the donor itself gets.
void RouteDispatch(const Dispatch& d, const Widened& w, int type, int donor) {
    const int mine = type - d.firstType;
    const int theirs = donor - d.firstType;
    const bool inSpan = theirs >= 0 && theirs <= d.oldBound;
    if (d.indexAt)
        w.index[mine] = inSpan ? patch::GetUChar(d.indexTable + theirs) : (uint8_t)w.defaultCase;
    else
        w.jump[mine] = inSpan ? patch::GetUInt(d.jumpTable + (uintptr_t)theirs * 4)
                              : (uint32_t)d.expectedDefault;
}

// ---------------------------------------------------------------- CWeapon::IsType2Handed

SafetyHookInline& TwoHandedOriginal() { static SafetyHookInline h; return h; }

// __thiscall on a CWeapon, whose first field is the weapon type. The vanilla body is a list of
// seven `cmp`s, so a new type can only be added to it by answering for the donor instead.
bool __fastcall IsType2HandedHook(void* self, void*) {
    const int type = *(const int*)self;
    if (type >= kFirstNewType && type <= kLastNewType) {
        const int donor = gDonor[type - kFirstNewType];
        if (donor >= 0) {
            int stand = donor;
            return TwoHandedOriginal().thiscall<bool>(&stand);
        }
    }
    return TwoHandedOriginal().thiscall<bool>(self);
}

}   // namespace

namespace WeaponTypes {

bool Available() { return gReady; }
int FirstType() { return kFirstNewType; }
int LastType() { return kLastNewType; }
int Remaining() { return gReady ? kLastNewType - gNext + 1 : 0; }

int DonorOf(int type) {
    if (type < kFirstNewType || type > kLastNewType) return -1;
    return gDonor[type - kFirstNewType];
}

bool RecordIsFree(int type) {
    if (type < 0 || type >= 80) return false;
    const uintptr_t record = kWeaponInfoArray + (uintptr_t)type * kWeaponInfoStride;
    return *(const int32_t*)(record + kSlotField) == -1;
}

bool Install() {
    if (gReady) return true;
    for (int i = 0; i < kNewTypeCount; i++) gDonor[i] = -1;

    if (!VerifyEverything()) return false;

    if (!WidenNameTable()) {
        Log::Write("weapon types: out of memory widening the name table; nothing is changed");
        return false;
    }
    for (int i = 0; i < kDispatchCount; i++) {
        if (!WidenDispatch(kDispatch[i], gWidened[i])) {
            // Half a widening is worse than none, but by here the name table is already pointed at
            // our copy and any earlier switch at its own. Those are all supersets of the vanilla
            // behaviour - every type San Andreas knows still lands where it always did - so the
            // game is playable; we simply refuse to hand out new types.
            Log::Write("weapon types: out of memory widening %s; no new weapon types will be "
                       "handed out, and San Andreas' own weapons are unaffected", kDispatch[i].what);
            return false;
        }
    }

    patch::SetUChar(kPickupBoundAt + 2, (unsigned char)(kLastNewType + 1));

    Log::Write("weapon types: San Andreas' weapon system widened - names to %d, and the five "
               "switches on weapon type (CWeapon::Fire, its noise, CWeapon::FireInstantHit, the "
               "gunshot and the reload) now reach type %d. %d new types are free (%d..%d); San "
               "Andreas' own %d weapons are untouched",
               kLastNewType, kLastNewType, kNewTypeCount, kFirstNewType, kLastNewType, 47);
    gReady = true;
    return true;
}

int Add(const char* name, int donorType) {
    if (!gReady || !name || !*name) return -1;
    if (gNext > kLastNewType) {
        Log::Write("weapon types: no free type left for %s - San Andreas has room for %d new "
                   "weapons and they are all taken", name, kNewTypeCount);
        return -1;
    }
    if (donorType < 0 || donorType > 46) {
        Log::Write("weapon types: %s has no San Andreas weapon to take its behaviour from; skipped",
                   name);
        return -1;
    }

    // Whether aWeaponInfo[type] is genuinely free cannot be settled here: this runs while the ASI
    // loads, long before CWeaponInfo::Initialise has put -1 in every record's slot and before any
    // weapon.dat has been read. RecordIsFree() below is what the caller asks once the table has
    // loaded, and it is the caller that refuses to write a record somebody else has claimed.
    const int type = gNext;
    gNext++;
    gDonor[type - kFirstNewType] = donorType;
    gNames[type] = name;
    for (int i = 0; i < kDispatchCount; i++)
        RouteDispatch(kDispatch[i], gWidened[i], type, donorType);

    // Only worth an inline hook if one of the guns actually is a rifle; a pistol answers the same
    // either way, and a hook not installed cannot go wrong.
    const bool donorIsTwoHanded = donorType == 25 || donorType == 27 || donorType == 30 ||
                                  donorType == 31 || donorType == 33 || donorType == 34 ||
                                  donorType == 37;
    if (donorIsTwoHanded && !gWantTwoHanded) {
        gWantTwoHanded = true;
        if (BytesAre(kIsType2Handed, kIsType2HandedOp, sizeof(kIsType2HandedOp))) {
            TwoHandedOriginal() = safetyhook::create_inline(
                reinterpret_cast<void*>(kIsType2Handed),
                reinterpret_cast<void*>(&IsType2HandedHook));
            if (!TwoHandedOriginal())
                Log::Write("weapon types: CWeapon::IsType2Handed could not be hooked; a new rifle "
                           "will be held one-handed in some animations");
        } else {
            Log::Write("weapon types: CWeapon::IsType2Handed is not vanilla, so it is left alone; a "
                       "new rifle will be held one-handed in some animations");
        }
    }

    Log::Write("weapon types: %s is weapon type %d, behaving as %s (type %d) and keeping its own "
               "record, model and name", name, type, *(const char* const*)(kNameTable +
               (uintptr_t)donorType * 4), donorType);
    return type;
}

}   // namespace WeaponTypes
