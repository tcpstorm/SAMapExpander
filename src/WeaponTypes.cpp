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
//   CWeapon::Fire                 0x742461  types 16..43  default 0x742794  the one that matters
//   CWeapon::Fire, tail           0x742874  types 16..40  default 0x74289F  the "a gun went off" bit
//   CWeapon::FireInstantHit       0x740C75  types 22..38  default 0x740E3A  per-gun accuracy
//   WeaponFire audio              0x504F96  types 22..45  default 0x50523F  the gunshot
//   WeaponReload audio            0x5036F2  types 22..34  default 0x503830  the reload click
//   CPed::UpdateCameraWeaponModes 0x609844  types 31..43  default 0x6098B6  the scope
//   CWeapon::DoBulletImpact       0x73BF2F  types 24..38  default 0x73BF65  the impact decal
//
// Only the first is fatal on its own: its default is the case ROCKET and FREEFALL_BOMB take, which
// fires nothing, so a new type would indeed be a silent, harmless prop. The others degrade - the
// country rifle's accuracy, no gunshot, no reload click - which is where the "fires blanks in
// silence" reading came from. All seven are widened here in the same way: both tables are copied to
// the heap with room for types up to 79, the `cmp` bound is raised, and the two disp32 operands are
// repointed. A new type is then routed to its donor's case, so a Python takes exactly the code path
// the Desert Eagle takes while keeping its own record, its own model and its own name.
//
// The last two took a second sweep to find. They normalise the type with `lea eax, [ecx - 0x1f]`
// rather than `add eax, -0x1f`, and the first sweep only looked for the `add` form - which is how a
// new sniper rifle came to draw, aim and fire perfectly well without ever zooming in.
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
constexpr unsigned kModelId1 = 0x0C;       // CWeaponInfo::m_nModelId1
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

    // The scope. This one subtracts with `lea eax, [ecx - 0x1f]` rather than `add eax, -0x1f`,
    // which is why the first sweep for these switches walked straight past it and a new sniper
    // rifle drew, aimed and fired without ever zooming in. It is the switch that decides which
    // camera weapon mode a gun gets - SNIPERRIFLE gets mode 7, the rocket launcher 8, the
    // heat-seeker 0x33, the camera 0x2E, the M4 0x22 - and its default clears the mode outright,
    // so a type it does not cover has no first-person weapon camera at all. CWeapon::Fire's own
    // sniper case (0x7424A6) then reads that camera mode rather than the weapon type to decide
    // whether to call CWeapon::FireSniper, so getting the mode right is the whole of it.
    { "CPed::UpdateCameraWeaponModes (the scope)",
      0x609844, { 0x83, 0xF8 }, 0x0C, 0x609847, 0x6098B6,
      0x609849, { 0x0F, 0xB6, 0x88 }, 0x6098DC,
      0x609850, { 0xFF, 0x24, 0x8D }, 0x6098C4, 31 },

    // Same `lea` shape, found in the same sweep: which impact decal a bullet leaves. Cosmetic on
    // its own, but it costs nothing to route it with the rest.
    { "CWeapon::DoBulletImpact",
      0x73BF2F, { 0x83, 0xF8 }, 0x0E, 0x73BF3D, 0x73BF65,
      0x73BF3F, { 0x0F, 0xB6, 0x80 }, 0x73C1DC,
      0x73BF46, { 0xFF, 0x24, 0x85 }, 0x73C1D0, 24 },
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

// ---------------------------------------------------------------- standing in for the donor
//
// Not everything San Andreas keys on a weapon type is a jump table. A handful of places are plain
// equality tests against one constant - `cmp edx, 0x22` and the branch that reads it - and those
// cannot be lengthened, only rewritten, which would mean hand-assembled code dropped into the
// middle of two functions that cannot be tested here. Where the whole of what such a function does
// with the type is cosmetic and self-contained, there is a safer way with no assembly in it: hand
// it the donor's number for the length of the call and put ours back on the way out. It is the same
// trick as CWeapon::IsType2Handed below, and it is only used where nothing outside the call can see
// the swap.
//
// Sweeping the camera, HUD and player code for `cmp` against the sniper constants 0x21 and 0x22
// turns up fifteen sites, and eleven of them are not weapon types at all: they compare a CAMERA
// mode, read from TheCamera's PlayerWeaponMode at +0x830, from the global at 0xB6F858, or from the
// active cam's m_nMode at 0xB6F1A8. Those come right by themselves as soon as
// CPed::UpdateCameraWeaponModes hands the new type its donor's camera mode, which the switch above
// now does. Two more - Camera::Process_AimWeapon (0x52163D) and CHud::DrawCrossHairs (0x58E1BD) -
// are real weapon-type tests, but SNIPERRIFLE is not in either list, so the donor misses them too
// and a new type behaves identically without any help. That leaves exactly two:
//
//   CHud::DrawCrossHairs   0x58E703  `cmp edx, 0x22`  the branch that draws the scope's reticle;
//                                    the fall-through at 0x58E719 jumps past the CROSSHAIR texture
//                                    lookup entirely, so a scoped weapon that misses this test
//                                    zooms in and shows nothing in the middle of the screen.
//   CPlayerPed::ProcessAnimGroups     0x6099D4/0x6099D9 - which walk and run animations the player
//                                    uses while holding the weapon; rifles get group 0x3C.
//
// Both are hooked below and answered for the donor.

constexpr uintptr_t kFindPlayerPed = 0x56E210;   // __cdecl CPed* FindPlayerPed(int)
constexpr unsigned kPedActiveSlot = 0x718;       // CPed::m_nActiveWeaponSlot, a signed byte
constexpr unsigned kPedWeapons = 0x5A0;          // CPed::m_aWeapons[13]
constexpr unsigned kPedWeaponStride = 0x1C;
constexpr int kPedWeaponSlots = 13;
constexpr unsigned kWeaponModelType = 0x24;      // CWeaponModelInfo::m_nWeaponType

void* gModelInfo[kNewTypeCount] = {};

// ---------------------------------------------------------------- CHud::DrawCrossHairs

constexpr uintptr_t kDrawCrossHairs = 0x58E020;
const uint8_t kDrawCrossHairsOp[5] = { 0x83, 0xEC, 0x30, 0x0F, 0xB6 };

SafetyHookInline& CrossHairsOriginal() { static SafetyHookInline h; return h; }

// Puts a value back whatever happens on the way out, which matters here because the two fields
// being lent are the game's own live state and not copies of it.
struct Lent {
    int32_t* at = nullptr;
    int32_t was = 0;
    void Lend(int32_t* field, int32_t value) { at = field; was = *field; *field = value; }
    ~Lent() { if (at) *at = was; }
};

bool gSaidCrossHair = false;

// TheCamera.m_aCams[m_nActiveCam].m_nMode, for the one diagnostic line below.
int ActiveCameraMode() {
    const unsigned active = *(const uint8_t*)0xB6F081;
    return *(const uint16_t*)(0xB6F1A8 + (uintptr_t)active * 0x238);
}

void __cdecl DrawCrossHairsHook() {
    // Two things have to be true for a new gun to get its donor's reticle, and the first one is
    // the reason this is a memory swap rather than a return value.
    //
    // The branch that draws a weapon's own crosshair is at 0x58E7A0, and it is reached from three
    // tests: type == CAMERA at 0x58E6E0, type == SNIPERRIFLE at 0x58E703, and a script global at
    // 0x58E70C. Anything else falls through to a chain of CAMERA-mode tests at 0x58E719 that ends
    // on the shared siteM16 sprite out of hud.txd - which is precisely what a new sniper rifle was
    // drawing. Note that the type is read from the ped at 0x58E6D1 and read AGAIN, freshly, at
    // 0x58E6F9, and a third time inside 0x58E7A0, and a fourth at 0x58E81A. Substituting a value
    // once would be undone by the next read, so what is lent is the ped's stored field itself:
    // every read in the function sees it, and it goes back before the function returns.
    //
    // The second is the dictionary. At 0x58E81A the branch takes the weapon type it has, asks
    // GetWeaponInfo for m_nModelId1, and looks the CROSSHAIR key up in the dictionary bound to that
    // model. With only the type lent, that resolves to the DONOR's model - San Andreas' own
    // sniper.txd - which is not streamed in while the player is holding a v_laser, so
    // m_pRwDictionary is null at 0x58E87D and nothing is drawn at all. So the donor's record is
    // lent our model id for the same moment: the key is then built from our model's name and looked
    // up in our own dictionary, which is loaded because the player is holding it, and which the
    // importer has put v_lasercrosshair in.
    //
    // Both are cosmetic, both are restored on the way out, and nothing else runs in between: this
    // is a leaf of the HUD pass on the game's own thread.
    typedef void* (__cdecl* FindPlayerPedFn)(int);
    void* ped = reinterpret_cast<FindPlayerPedFn>(kFindPlayerPed)(-1);
    Lent type, model;
    if (ped) {
        const int slot = *(const int8_t*)((uintptr_t)ped + kPedActiveSlot);
        if (slot >= 0 && slot < kPedWeaponSlots) {
            int32_t* held = (int32_t*)((uintptr_t)ped + kPedWeapons +
                                       (unsigned)slot * kPedWeaponStride);
            const int mine = *held;
            if (mine >= kFirstNewType && mine <= kLastNewType) {
                const int donor = gDonor[mine - kFirstNewType];
                const int32_t ours = *(const int32_t*)(kWeaponInfoArray +
                                                       (uintptr_t)mine * kWeaponInfoStride + kModelId1);
                if (donor >= 0) {
                    type.Lend(held, donor);
                    if (ours > 0)
                        model.Lend((int32_t*)(kWeaponInfoArray + (uintptr_t)donor * kWeaponInfoStride +
                                              kModelId1), ours);
                    if (!gSaidCrossHair) {
                        gSaidCrossHair = true;
                        Log::Write("weapon types: the crosshair stand-in is running - weapon type "
                                   "%d is drawn as %d for one HUD pass, out of model %d's own "
                                   "dictionary; camera mode %d",
                                   mine, donor, (int)ours, ActiveCameraMode());
                    }
                }
            }
        }
    }
    CrossHairsOriginal().ccall<void>();
}

// ---------------------------------------------------------------- CPlayerPed::ProcessAnimGroups

constexpr uintptr_t kProcessAnimGroups = 0x6098F0;
const uint8_t kProcessAnimGroupsOp[5] = { 0x51, 0x55, 0x56, 0x8B, 0xF1 };

SafetyHookInline& AnimGroupsOriginal() { static SafetyHookInline h; return h; }

void __fastcall ProcessAnimGroupsHook(void* self, void*) {
    // This one reads CWeaponModelInfo::m_nWeaponType off a model info rather than off the ped, and
    // which model info it reaches depends on the branch it takes, so every new weapon's model wears
    // its donor's number for the length of the call. Nothing else reads that field from in here -
    // CPickups::WeaponForModel, the other reader, is nowhere near an animation function.
    int32_t* field[kNewTypeCount];
    int32_t saved[kNewTypeCount];
    int held = 0;
    for (int i = 0; i < kNewTypeCount; i++) {
        if (gDonor[i] < 0 || !gModelInfo[i]) continue;
        int32_t* p = (int32_t*)((uintptr_t)gModelInfo[i] + kWeaponModelType);
        if (*p != kFirstNewType + i) continue;      // somebody else owns it now; leave it alone
        field[held] = p;
        saved[held] = *p;
        *p = gDonor[i];
        held++;
    }
    AnimGroupsOriginal().thiscall<void>(self);
    for (int i = 0; i < held; i++) *field[i] = saved[i];
}

bool gStandIns = false;

// One inline hook, installed only while the function still begins with the bytes San Andreas ships,
// and never fatal: a hook that will not go on costs a cosmetic detail and is said so in the log.
void HookStandIn(uintptr_t address, const uint8_t* expected, size_t length, void* replacement,
                 SafetyHookInline& store, const char* what, const char* consequence) {
    if (!BytesAre(address, expected, length)) {
        Log::Write("weapon types: %s is not what San Andreas ships, so it is left alone; %s",
                   what, consequence);
        return;
    }
    store = safetyhook::create_inline(reinterpret_cast<void*>(address), replacement);
    // Said either way. A hook that quietly did not go on and a hook that went on and never fires
    // look identical from the outside, and the last round of this was spent guessing between them.
    if (store)
        Log::Write("weapon types: %s (0x%X) is hooked, so a new gun answers it as its donor would",
                   what, (unsigned)address);
    else
        Log::Write("weapon types: %s could not be hooked; %s", what, consequence);
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

void SetModelInfo(int type, void* modelInfo) {
    if (type < kFirstNewType || type > kLastNewType) return;
    gModelInfo[type - kFirstNewType] = modelInfo;
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

    Log::Write("weapon types: San Andreas' weapon system widened - names to %d, and all %d switches "
               "on weapon type (CWeapon::Fire, its noise, CWeapon::FireInstantHit, the gunshot, the "
               "reload, the camera weapon mode that makes a scope and the bullet impact) now reach "
               "type %d. %d new types are free (%d..%d); San Andreas' own 47 weapons are untouched",
               kLastNewType, kDispatchCount, kLastNewType, kNewTypeCount, kFirstNewType, kLastNewType);
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

    // The two functions that test the type against a fixed constant rather than switching on it.
    // Installed on the first gun and not before, so a game with no new weapons carries no hooks.
    if (!gStandIns) {
        gStandIns = true;
        HookStandIn(kDrawCrossHairs, kDrawCrossHairsOp, sizeof(kDrawCrossHairsOp),
                    reinterpret_cast<void*>(&DrawCrossHairsHook), CrossHairsOriginal(),
                    "CHud::DrawCrossHairs", "a scoped gun will zoom in and show no reticle");
        HookStandIn(kProcessAnimGroups, kProcessAnimGroupsOp, sizeof(kProcessAnimGroupsOp),
                    reinterpret_cast<void*>(&ProcessAnimGroupsHook), AnimGroupsOriginal(),
                    "CPlayerPed::ProcessAnimGroups", "the player will walk with the wrong stance");
    }

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
