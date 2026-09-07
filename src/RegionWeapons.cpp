// Gives San Andreas the guns Vice City and Liberty City had and it does not - the Python, the
// Ruger, the M60 - in one of two ways, and the difference between them is the whole point of the
// [World] RegionWeaponMode key.
//
//   Add      (the default, and what you want) - the gun becomes a weapon type of its own,
//            alongside San Andreas' forty-seven. The Desert Eagle stays a Desert Eagle; the Python
//            is a new gun that happens to take the Desert Eagle's code paths. WeaponTypes.cpp does
//            the widening that makes this possible and explains exactly what it patches.
//
//   Replace  the older behaviour, kept because it is what shipped: the gun is written over the San
//            Andreas weapon nearest to it. There is one weapon table and it is global, so this
//            changes that weapon in Los Santos too. Off unless you ask for it.
//
// In both modes a region's weapons.dat names, per line, the San Andreas weapon involved. Under
// Replace that weapon's records are overwritten. Under Add it is only a donor: the new type's
// record starts as a copy of the donor's STD record - which is where its target range, weapon slot,
// animation group, skill levels and animation frame numbers come from, none of which the source
// game has a column for - and then the file's own columns are written on top. The donor is also
// what tells WeaponTypes which case to route the new type to in each of the game's five switches on
// weapon type, so a Python fires down the Desert Eagle's path and sounds like one until its own
// record says otherwise.
//
// A line may name the new gun explicitly, as `PYTHON/DESERT_EAGLE`; a bare `DESERT_EAGLE` still
// works and Add mode then makes a name up from the model, which is how a weapons.dat written before
// this existed keeps working.
//
// Everything spelled `*` in the file is left alone: under Replace that means San Andreas' own
// value, under Add it means the donor's. The animation numbers in particular index San Andreas' own
// IFP animations, which the source game's numbers do not address.
//
// The models come in as ordinary `weap` IDE lines through CityLoader, which needs somewhere to put
// them: Models::AddWeaponModel (0x4C6710) appends to CStore<CWeaponModelInfo, 51> at 0xB1E158
// without checking the count, and San Andreas' own default.ide fills 50 of the 51. So the store is
// moved to the heap with room to spare first, in the same style as ModelLimits' time store - eight
// instructions name it, and none of them is written unless all eight still hold the vanilla value.
//
// None of this runs under SA-MP. The server owns weapon behaviour there: a client whose Desert
// Eagle had quietly become a Python, or which knew about a gun nobody else has, would be
// desynchronised from everyone else.
#include <plugin.h>
#include <CModelInfo.h>
#include <safetyhook.hpp>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "RegionWeapons.h"
#include "WeaponTypes.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// ---------------------------------------------------------------- the game's weapon tables

constexpr uintptr_t kWeaponInfoArray = 0xC8AAB8;   // CWeaponInfo::aWeaponInfo[80]
constexpr unsigned kWeaponInfoStride = 0x70;
constexpr int kWeaponInfoCount = 80;
constexpr uintptr_t kWeaponNames = 0x8D6150;       // CWeaponInfo::ms_aWeaponNames[49], char*
constexpr int kWeaponNameCount = 49;               // FindWeaponType compares indices 0..0x30
constexpr int kLastWeaponType = 46;                // 47 is the empty sentinel, 48 is ARMOUR

// CWeaponInfo, as CWeaponInfo::Initialise (0x5BF750) leaves it and LoadWeaponData fills it in.
constexpr unsigned kFireType = 0x00, kTargetRange = 0x04, kWeaponRange = 0x08;
constexpr unsigned kModelId1 = 0x0C, kModelId2 = 0x10, kSlot = 0x14, kFlags = 0x18;
constexpr unsigned kAnimGroup = 0x1C, kAmmoClip = 0x20, kDamage = 0x22, kFireOffset = 0x24;
constexpr unsigned kSpeed = 0x5C, kRadius = 0x60, kLifespan = 0x64, kSpread = 0x68;

// GetWeaponInfo's four skill blocks, as the four immediates in 0x743C60 give them. They are only
// meaningful for the eleven weapons that have skill levels at all - PISTOL through TEC9, which is
// the range LoadWeaponData itself checks with `cmp ebx,0x16` and `cmp ebx,0x20`. For any other
// weapon `type + 25` is not a skill variant of it but the STD record of an unrelated weapon:
// KATANA is type 8, and 8 + 25 is COUNTRYRIFLE.
const int kSkillOffset[4] = { 25, 0, 36, 47 };
constexpr int kFirstSkilledWeapon = 22;
constexpr int kLastSkilledWeapon = 32;

// Animation::GetAnimGroupName (0x4D3A20): the AnimAssocDefinition table, name inline at +0.
constexpr uintptr_t kAnimGroups = 0x8AA5A8;
constexpr unsigned kAnimGroupStride = 0x30;
constexpr uintptr_t kAnimGroupCount = 0xB4EA28;

// CWeaponModelInfo::m_nWeaponType, which LoadWeaponData writes so that CPickups::WeaponForModel
// (0x454AE0) and CHud::DrawWeaponIcon (0x58D7D0) can get from a model back to its weapon.
constexpr unsigned kWeaponModelType = 0x24;
constexpr unsigned char kWeaponModelInfoType = 4;   // what the model info's GetModelType() returns

// ---------------------------------------------------------------- the weapon model info store

// CStore<CWeaponModelInfo, 51>: an int32 count followed by the objects. Models::AddWeaponModel
// (0x4C6710) multiplies the count by 0x28 and increments it without ever comparing it to 51, so a
// bigger array is all it takes - the same shape as ModelLimits' time store.
constexpr uintptr_t kStoreCount = 0xB1E158;
constexpr uintptr_t kStoreObjects = 0xB1E15C;
constexpr unsigned kStoreElem = 0x28;
constexpr int kVanillaStoreCapacity = 51;
constexpr int kStoreHeadroom = 128;
constexpr uintptr_t kWeaponModelInfoCtor = 0x4C5760;   // __thiscall, sets the two base fields and the vtable

struct StoreSite {
    uintptr_t address;
    uint32_t  oldValue;
    uint8_t   operandOffset;
};

// Every instruction in the first .text that names the store. The two remaining sites - the CRT's
// vector constructor at 0x84BC90 and its atexit destructor - are deliberately left alone: they
// build the original static block, which simply goes unused.
const StoreSite kStoreSites[] = {
    { 0x4C64A4, kStoreCount,   1 },   // Models::ShutDown
    { 0x4C64AF, kStoreObjects, 1 },
    { 0x4C64BB, kStoreCount,   1 },
    { 0x4C65F6, kStoreCount,   2 },
    { 0x4C6710, kStoreCount,   1 },   // Models::AddWeaponModel
    { 0x4C671A, kStoreObjects, 3 },
    { 0x4C6721, kStoreCount,   1 },
    { 0x4C6845, kStoreCount,   2 },   // Models::Initialise, `mov [imm32], edi`
};

uint8_t* gStore = nullptr;
int gStoreCapacity = kVanillaStoreCapacity;
bool gActive = false;
bool gAdding = false;   // Add mode: new weapon types, San Andreas' own left alone

bool RelocateStore() {
    for (const StoreSite& s : kStoreSites) {
        uint32_t have = patch::GetUInt(s.address + s.operandOffset);
        if (have != s.oldValue) {
            Log::Write("ERROR: the weapon model store was not moved: 0x%X holds 0x%X, expected 0x%X "
                       "(another mod patched it); a region's weapon models are left out",
                       (unsigned)s.address, have, s.oldValue);
            return false;
        }
    }

    const int capacity = kVanillaStoreCapacity + kStoreHeadroom;
    gStore = (uint8_t*)calloc(1, 4 + (size_t)capacity * kStoreElem);
    if (!gStore) {
        Log::Write("ERROR: the weapon model store was not moved: out of memory");
        return false;
    }

    // The vanilla store's objects are built by a static initialiser that has not run yet, so ours
    // are built the same way the game builds them: CWeaponModelInfo's constructor writes the base
    // model info's two fields and then its own vtable, and touches nothing else.
    typedef void(__thiscall* WeaponModelInfoCtor)(void*);
    const WeaponModelInfoCtor construct = reinterpret_cast<WeaponModelInfoCtor>(kWeaponModelInfoCtor);
    for (int i = 0; i < capacity; i++) construct(gStore + 4 + (size_t)i * kStoreElem);

    const uint32_t newCount = (uint32_t)(uintptr_t)gStore;
    const uint32_t newObjects = newCount + 4;
    for (const StoreSite& s : kStoreSites)
        patch::SetUInt(s.address + s.operandOffset, s.oldValue == kStoreCount ? newCount : newObjects);

    gStoreCapacity = capacity;
    Log::Write("weapon model store moved to the heap: %d entries at %p (vanilla %d, of which San "
               "Andreas' own weapons take 50)", capacity, (void*)(gStore + 4), kVanillaStoreCapacity);
    return true;
}

// ---------------------------------------------------------------- reading a region's weapons.dat

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (c == ',' || (unsigned char)c <= (unsigned char)' ') {
            if (!current.empty()) { out.push_back(current); current.clear(); }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

bool Keep(const std::string& token) { return token == "*"; }

// The weapon type a name stands for, read out of the game's own table rather than through
// CWeaponInfo::FindWeaponType - which answers 0 for a name it does not know, and would have us
// quietly rewrite UNARMED.
int WeaponTypeByName(const std::string& name) {
    for (int i = 0; i <= kLastWeaponType; i++) {
        const char* have = *(const char* const*)(kWeaponNames + (uintptr_t)i * 4);
        if (have && _stricmp(have, name.c_str()) == 0) return i;
    }
    return -1;
}

int AnimGroupByName(const std::string& name) {
    const int count = (int)patch::GetUInt(kAnimGroupCount);
    for (int i = 0; i < count; i++) {
        const char* have = (const char*)(kAnimGroups + (uintptr_t)i * kAnimGroupStride);
        if (_stricmp(have, name.c_str()) == 0) return i;
    }
    return -1;
}

uintptr_t WeaponInfoAt(int type, int skill) {
    // A type WeaponTypes handed out is a record of its own at the end of the array and has no skill
    // variants: nothing in San Andreas would ever ask for one, because CPed::GetWeaponSkill
    // (0x5E3B60) answers STD for anything outside PISTOL..TEC9.
    if (type >= WeaponTypes::FirstType() && type <= WeaponTypes::LastType())
        return skill == 1 ? kWeaponInfoArray + (uintptr_t)type * kWeaponInfoStride : 0;
    if (type < 0 || type > kLastWeaponType) return 0;
    if (skill != 1 && (type < kFirstSkilledWeapon || type > kLastSkilledWeapon)) return 0;
    int index = type + kSkillOffset[skill];
    if (index < 0 || index >= kWeaponInfoCount) return 0;
    return kWeaponInfoArray + (uintptr_t)index * kWeaponInfoStride;
}

// A record San Andreas' own weapon.dat never filled still holds CWeaponInfo::Initialise's default
// slot of -1, and every line the parser accepts writes a real one. That is how we tell which of a
// type's four skill records exist without having to know which weapons have skill variants.
bool RecordInUse(uintptr_t record) { return *(int32_t*)(record + kSlot) != -1; }

// CBaseModelInfo's GetModelType() is the fifth virtual; CPickups::WeaponForModel calls it the same
// way to decide whether a model id really is a weapon.
unsigned char ModelType(void* modelInfo) {
    typedef unsigned char(__thiscall* GetModelTypeFn)(void*);
    void** vtable = *(void***)modelInfo;
    return reinterpret_cast<GetModelTypeFn>(vtable[4])(modelInfo);
}

struct Line {
    std::string sourceName;   // what the source game called this gun, for the log only
    std::string newName;      // what Add mode will call it; empty means "make one up from the model"
    int donor = -1;           // the San Andreas weapon type: overwritten in Replace, borrowed in Add
    int type = -1;            // where this line's numbers actually go - the donor, or a new type
    std::vector<std::string> field;
};

struct RegionArms {
    const MapRegion* region;
    std::vector<Line> lines;
};

std::vector<RegionArms>& Arms() { static std::vector<RegionArms> v; return v; }

// type, fire type, target range, weapon range, model, model2, slot, anim group, clip, damage,
// offset x, offset y, offset z, flags, then optionally speed, radius, lifespan, spread.
constexpr size_t kFieldsMin = 14;
constexpr size_t kFieldsMax = 18;

const char* const kFireTypeName[] = { "MELEE", "INSTANT_HIT", "PROJECTILE", "AREA_EFFECT", "CAMERA", "USE_3RD_PERSON_AIM" };

int FireTypeByName(const std::string& name) {
    for (int i = 0; i < 6; i++)
        if (_stricmp(kFireTypeName[i], name.c_str()) == 0) return i;
    return -1;
}

bool ReadWeaponFile(RegionArms& arms) {
    const std::string& path = arms.region->weaponFile;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        Log::Write("[%s] weapons: %s cannot be opened", arms.region->name.c_str(), path.c_str());
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);

    int malformed = 0, unknown = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(at, end - at);
        at = end + 1;

        // A trailing comment is where the importer records what the line was in the source game;
        // keep it for the log, since "DESERT_EAGLE" alone would not say that it is now a Python.
        std::string comment;
        size_t hash = raw.find('#');
        if (hash != std::string::npos) { comment = Trim(raw.substr(hash + 1)); raw = raw.substr(0, hash); }
        // ';' is how the importer ships a line it has written but does not recommend - the trades
        // where San Andreas' own weapon is worth more than the import - so it has to mean nothing
        // rather than mean a weapon called ";KATANA".
        std::string line = Trim(raw);
        if (line.empty() || line[0] == ';') continue;

        std::vector<std::string> t = Tokens(line);
        if (t.size() < kFieldsMin || t.size() > kFieldsMax) { malformed++; continue; }

        Line entry;
        // `PYTHON/DESERT_EAGLE` names the new gun and its donor; a bare name is the donor alone,
        // which is all Replace mode ever needed and all a weapons.dat written before Add mode has.
        std::string donorName = t[0];
        size_t slash = donorName.find('/');
        if (slash != std::string::npos) {
            entry.newName = Trim(donorName.substr(0, slash));
            donorName = Trim(donorName.substr(slash + 1));
        }
        entry.donor = WeaponTypeByName(donorName);
        if (entry.donor < 0) {
            if (unknown < 5)
                Log::Write("[%s] weapons: San Andreas has no weapon type called %s; the line is "
                           "dropped rather than written over UNARMED, which is where its own parser "
                           "would have put it", arms.region->name.c_str(), donorName.c_str());
            unknown++;
            continue;
        }
        entry.type = entry.donor;
        entry.sourceName = comment;
        entry.field.assign(t.begin() + 1, t.end());
        arms.lines.push_back(entry);
    }

    if (malformed || unknown)
        Log::Write("[%s] weapons: %d line(s) with the wrong number of columns, %d naming a weapon "
                   "San Andreas does not have", arms.region->name.c_str(), malformed, unknown);
    return !arms.lines.empty();
}

// ---------------------------------------------------------------- applying it

int ApplyLine(const RegionArms& arms, const Line& line) {
    // Add mode leaves a line it could not give a type of its own at -1 rather than at the donor,
    // precisely so that it cannot be written over San Andreas' weapon by accident.
    if (line.type < 0) return 0;
    const std::vector<std::string>& f = line.field;

    // The model is named, not numbered: the importer renames every asset into the region's own
    // namespace and CityLoader hands out the model ids, so the only stable thing to write down is
    // the name. See docs/asset-namespacing.md.
    int modelId = -1;
    CBaseModelInfo* model = nullptr;
    if (!Keep(f[3]) && f[3] != "-1") {
        model = CModelInfo::GetModelInfo(f[3].c_str(), &modelId);
        if (!model) {
            Log::Write("[%s] weapons: there is no model called %s - the region's weap IDE either "
                       "did not load or the name is not the renamed one; San Andreas' own weapon is "
                       "left alone", arms.region->name.c_str(), f[3].c_str());
            return 0;
        }
        if (ModelType(model) != kWeaponModelInfoType) {
            Log::Write("[%s] weapons: %s is not a weapon model - it came from an objs line rather "
                       "than a weap one, so there is nowhere on it to record the weapon type; skipped",
                       arms.region->name.c_str(), f[3].c_str());
            return 0;
        }
    }

    int fireType = Keep(f[0]) ? -1 : FireTypeByName(f[0]);
    if (!Keep(f[0]) && fireType < 0)
        Log::Write("[%s] weapons: fire type %s is not one San Andreas knows; its own is kept",
                   arms.region->name.c_str(), f[0].c_str());
    int animGroup = Keep(f[6]) ? -1 : AnimGroupByName(f[6]);
    if (!Keep(f[6]) && animGroup < 0)
        Log::Write("[%s] weapons: animation group %s is not one San Andreas knows; its own is kept",
                   arms.region->name.c_str(), f[6].c_str());

    // Add mode gave this line a type of its own; Replace mode left it pointing at the donor.
    const bool adding = line.type != line.donor;

    int written = 0;
    for (int skill = 0; skill < 4; skill++) {
        uintptr_t record = WeaponInfoAt(line.type, skill);
        if (!record) continue;
        if (adding) {
            // The record has to still be the empty one CWeaponInfo::Initialise leaves behind. A
            // weapon.dat that gives some weapon other than the pistol a $COP line would have
            // written this far up the array, and taking it would break that weapon silently.
            if (!WeaponTypes::RecordIsFree(line.type)) {
                Log::Write("[%s] weapons: aWeaponInfo[%d] has been claimed by a weapon.dat line of "
                           "its own, so %s is left out rather than written over it",
                           arms.region->name.c_str(), line.type, line.newName.c_str());
                return 0;
            }
            // Start from the donor, so that everything the source game has no column for - the
            // target range, the slot, the animation group and its frame numbers, the skill
            // thresholds, the accuracy and move speed - is a working San Andreas weapon's rather
            // than a zeroed record. The columns below then say where the new gun differs.
            const uintptr_t from = WeaponInfoAt(line.donor, 1);
            if (!from) return 0;
            memcpy((void*)record, (const void*)from, kWeaponInfoStride);
        } else if (!RecordInUse(record)) {
            continue;
        }

        if (fireType >= 0) *(int32_t*)(record + kFireType) = fireType;
        if (!Keep(f[1])) *(float*)(record + kTargetRange) = (float)atof(f[1].c_str());
        if (!Keep(f[2])) *(float*)(record + kWeaponRange) = (float)atof(f[2].c_str());
        if (modelId >= 0) *(int32_t*)(record + kModelId1) = modelId;
        if (!Keep(f[4])) *(int32_t*)(record + kModelId2) = atoi(f[4].c_str());
        if (!Keep(f[5])) *(int32_t*)(record + kSlot) = atoi(f[5].c_str());
        if (animGroup >= 0) *(int32_t*)(record + kAnimGroup) = animGroup;
        if (!Keep(f[7])) *(int16_t*)(record + kAmmoClip) = (int16_t)atoi(f[7].c_str());
        if (!Keep(f[8])) *(int16_t*)(record + kDamage) = (int16_t)atoi(f[8].c_str());
        for (int axis = 0; axis < 3; axis++)
            if (!Keep(f[9 + axis])) *(float*)(record + kFireOffset + axis * 4) = (float)atof(f[9 + axis].c_str());
        if (!Keep(f[12])) *(uint32_t*)(record + kFlags) = (uint32_t)strtoul(f[12].c_str(), nullptr, 16);
        if (f.size() >= kFieldsMax - 1) {
            if (!Keep(f[13])) *(float*)(record + kSpeed) = (float)atof(f[13].c_str());
            if (!Keep(f[14])) *(float*)(record + kRadius) = (float)atof(f[14].c_str());
            if (!Keep(f[15])) *(float*)(record + kLifespan) = (float)atof(f[15].c_str());
            if (!Keep(f[16])) *(float*)(record + kSpread) = (float)atof(f[16].c_str());
        }
        written++;
    }

    // The reverse link the game keeps on the model itself. Without it the pickup lying in the
    // street and the icon in the weapon wheel both fail to find the weapon the model belongs to.
    if (model && written) {
        *(int32_t*)((uintptr_t)model + kWeaponModelType) = line.type;
        // CPlayerPed::ProcessAnimGroups reads that same field and compares it against a fixed list
        // of rifle types, so WeaponTypes needs the pointer to lend it the donor's number while that
        // function runs. Handed over rather than computed: ModelLimits has moved the array these
        // pointers usually come out of.
        if (adding) WeaponTypes::SetModelInfo(line.type, model);
    }
    return written;
}

void ApplyAll() {
    for (const RegionArms& arms : Arms()) {
        int weapons = 0, records = 0;
        for (const Line& line : arms.lines) {
            int n = ApplyLine(arms, line);
            if (n) { weapons++; records += n; }
        }
        if (gAdding)
            Log::Write("[%s] weapons: %d gun(s) added as weapon types of their own; San Andreas' "
                       "own weapons are exactly as they were", arms.region->name.c_str(), weapons);
        else
            Log::Write("[%s] weapons: %d San Andreas weapon type(s) taken over, %d skill record(s) "
                       "written - these apply to the whole game, not only inside the region",
                       arms.region->name.c_str(), weapons, records);
        for (const Line& line : arms.lines)
            Log::Debug("[%s] weapons: %s is %s", arms.region->name.c_str(),
                       gAdding ? line.newName.c_str()
                               : *(const char* const*)(kWeaponNames + (uintptr_t)line.type * 4),
                       line.sourceName.empty() ? "from the region" : line.sourceName.c_str());
    }
}

// ---------------------------------------------------------------- claiming the new types

bool StartsWithNoCase(const std::string& s, const char* prefix) {
    size_t n = strlen(prefix);
    if (s.size() <= n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

// A line that did not name its gun is named after its mesh instead - v_python becomes PYTHON -
// stripping the prefix the importer puts on every asset it renames. That is what makes a
// weapons.dat written before Add mode existed still add rather than replace.
std::string NameFromModel(const std::string& model) {
    std::string s = model;
    for (const char* p : { "v_", "vc_", "lc_", "w_" })
        if (StartsWithNoCase(s, p)) { s = s.substr(strlen(p)); break; }
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// Ask WeaponTypes to widen the game, then hand every line a weapon type of its own. This runs while
// the ASI loads, which is early enough that CWeaponInfo::FindWeaponType will already know the new
// names by the time data/weapon.dat is parsed, and long before anything can be fired.
bool ClaimNewTypes() {
    if (!WeaponTypes::Install()) return false;

    int added = 0;
    for (RegionArms& arms : Arms()) {
        for (Line& line : arms.lines) {
            // Until a type of its own has been claimed the line points at the donor, and a line
            // left pointing at the donor is a line that would overwrite San Andreas' own weapon.
            // Nothing here may end that way: a gun that cannot be added is dropped, not swapped.
            line.type = -1;
            if (line.field.size() <= 3 || Keep(line.field[3]) || line.field[3] == "-1") {
                Log::Write("[%s] weapons: a line with no mesh of its own cannot be a new weapon - "
                           "there would be nothing to hold; skipped", arms.region->name.c_str());
                continue;
            }
            std::string name = line.newName.empty() ? NameFromModel(line.field[3]) : line.newName;
            if (name.empty()) continue;
            // A new gun must not answer to a name San Andreas already uses, or FindWeaponType would
            // hand out whichever came first and a weapon.dat line meant for one would land on the
            // other. Namespacing it by region is the same rule the imported assets follow.
            if (WeaponTypeByName(name) >= 0) {
                std::string qualified = arms.region->name + "_" + name;
                for (char& c : qualified) c = (char)toupper((unsigned char)c);
                Log::Write("[%s] weapons: San Andreas already has a weapon called %s, so the new "
                           "one is called %s", arms.region->name.c_str(), name.c_str(),
                           qualified.c_str());
                name = qualified;
            }
            // Deliberately leaked: the game's name table holds the pointer for as long as it runs,
            // and there is nowhere sensible to free it that is not after the game has stopped.
            char* owned = strdup(name.c_str());
            if (!owned) continue;
            const int type = WeaponTypes::Add(owned, line.donor);
            if (type < 0) { free(owned); continue; }
            line.type = type;
            line.newName = name;
            added++;
        }
    }
    if (!added) {
        Log::Write("weapons: no line could be given a weapon type of its own");
        return false;
    }
    gAdding = true;
    return true;
}

// ---------------------------------------------------------------- the hook

// CWeaponInfo::Initialise tail-jumps here, and CWeapon::InitialiseWeapons (0x73A300) calls that
// from two places: CGame::Init3 (0x5BA2DA), once the level and so the region's IDEs have loaded,
// and CGame::ReInitGameObjectVariables (0x53BD97), which every new game and every load game runs.
// So this is both the earliest moment the region's models exist and the last word on the weapon
// table - a post hook here needs no restart handling of its own.
SafetyHookInline& LoadWeaponDataOriginal() { static SafetyHookInline h; return h; }

void __cdecl LoadWeaponDataHook() {
    LoadWeaponDataOriginal().ccall<void>();
    ApplyAll();
}

}   // namespace

namespace RegionWeapons {

bool Active() { return gActive && !Arms().empty(); }

uintptr_t Store() { return gActive ? (uintptr_t)gStore : kStoreCount; }
int Capacity() { return gStoreCapacity; }

// A region's pickups.dat names the San Andreas weapon each of its street pickups stood for, because
// that is all there was to name when the importer wrote it. Under Add mode that name is the donor,
// and what should really lie in Vice City's street is the gun that borrowed it - the Python, not
// the Desert Eagle. Only this lookup is redirected: San Andreas' own code never comes through here.
int WeaponType(const char* name) {
    const int donor = WeaponTypeByName(name);
    if (donor < 0 || !gAdding) return donor;
    for (const RegionArms& arms : Arms())
        for (const Line& line : arms.lines)
            if (line.donor == donor && line.type != donor) return line.type;
    return donor;
}

int WeaponModel(int weaponType) {
    uintptr_t record = WeaponInfoAt(weaponType, 1);   // the STD record, which every weapon has
    return record ? *(int32_t*)(record + kModelId1) : -1;
}

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.weaponFile.empty();
    if (!any || !Settings::world.regionWeapons) return;

    // Under SA-MP the server owns what every weapon is and does. A client whose Desert Eagle had
    // quietly become a Python, or which knew about a gun the server has never heard of, would be
    // shooting something nobody else can see.
    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region weapon import is off, the server owns the weapon table");
        return;
    }

    // The models have to have somewhere to go before CityLoader offers the region's weap lines to
    // the game's IDE parser, which is why this runs at load time rather than from the hook below.
    gActive = RelocateStore();

    for (const MapRegion& r : Settings::regions) {
        if (r.weaponFile.empty()) continue;
        RegionArms arms{ &r };
        if (ReadWeaponFile(arms)) Arms().push_back(arms);
    }
    if (Arms().empty()) return;

    if (Settings::world.regionWeaponMode == 0 && !ClaimNewTypes()) {
        // Adding is what was asked for and adding is not possible. Quietly falling back on Replace
        // would change San Andreas' own guns behind the player's back, which is the one thing this
        // must never do, so the region's weapons are simply left out.
        Log::Write("weapons: the region's guns are left out. Set [World] RegionWeaponMode = Replace "
                   "if you would rather have them written over San Andreas' own weapons, which "
                   "changes those weapons for the whole game");
        Arms().clear();
        return;
    }

    LoadWeaponDataOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x5BE670),
                                                        reinterpret_cast<void*>(&LoadWeaponDataHook));
    if (!LoadWeaponDataOriginal()) {
        Log::Write("weapons: the hook on CWeaponInfo::LoadWeaponData failed; the region's guns are "
                   "left out");
        Arms().clear();
        return;
    }
    int lines = 0;
    for (const RegionArms& arms : Arms()) lines += (int)arms.lines.size();
    Log::Write(gAdding ? "weapons: %d line(s) from %u region file(s) will be added to San Andreas' "
                         "weapon table after it loads, as types of their own"
                       : "weapons: %d line(s) from %u region file(s) will be written over San "
                         "Andreas' own weapon table after it loads",
               lines, (unsigned)Arms().size());
}

}
