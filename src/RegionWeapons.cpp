// Gives San Andreas the guns Vice City and Liberty City had and it does not - the Python, the
// Ruger, the M60 - by handing their models and their numbers to weapon types San Andreas already
// knows, rather than by inventing new ones.
//
// The reason it has to be that way is that a San Andreas weapon is not an open-ended data file the
// way text is. CWeaponInfo::LoadWeaponData (0x5BE670) does parse data/weapon.dat, but the name in
// column A is looked up in a fixed table of 49 char* at 0x8D6150 by CWeaponInfo::FindWeaponType
// (0x743D10), whose loop ends at `cmp esi, 0x30`; a name the table does not hold returns 0, so a
// line for a weapon San Andreas never had is not ignored - it silently overwrites UNARMED. The
// records themselves are aWeaponInfo[80] at 0xC8AAB8, 0x70 bytes each, and CWeaponInfo::GetWeaponInfo
// (0x743C60) indexes it as `type + {POOR 25, STD 0, PRO 36, COP 47}` with no bounds check at all,
// which packs the eleven skill-varied weapons in immediately behind the 47 real ones and leaves no
// free type index.
//
// Growing all of that is not the hard part - only 54 instructions name the array, against the 1407
// ModelLimits rewrites for the model id space. The hard part is that every switch keyed on weapon
// type - CWeapon::Fire (0x742880), CWeapon::FireInstantHit (0x740C85), the firing audio at
// 0x504F9F - is a `cmp` against the vanilla maximum followed by `ja default`. A genuinely new type
// would therefore be a gun that draws in the player's hand, appears in the HUD, and does nothing
// when the trigger is pulled, in silence, until each of those functions is reimplemented. That is
// the trade this file refuses to make.
//
// So a region's weapons.dat says, per line, which San Andreas weapon type it takes over, and this
// writes the source game's model, range, clip, damage and fire offset into every skill variant of
// that type. The cost is stated plainly in MapExpander.ini and the file ships commented out: the
// Desert Eagle becomes a Python in Los Santos as well as in Vice City, because there is one
// weapon table and it is global.
//
// Everything the source game has no column for - San Andreas' target range, weapon slot, animation
// group, skill levels, accuracy, move speed and animation frame numbers - is left as San Andreas
// wrote it, spelled `*` in the file. The animation numbers in particular index San Andreas' own
// IFP animations, which the source game's numbers do not address.
//
// The models come in as ordinary `weap` IDE lines through CityLoader, which needs somewhere to put
// them: Models::AddWeaponModel (0x4C6710) appends to CStore<CWeaponModelInfo, 51> at 0xB1E158
// without checking the count, and San Andreas' own default.ide fills 50 of the 51. So the store is
// moved to the heap with room to spare first, in the same style as ModelLimits' time store - eight
// instructions name it, and none of them is written unless all eight still hold the vanilla value.
//
// None of this runs under SA-MP. The server owns weapon behaviour there, and a client that quietly
// turned its Desert Eagle into a Python would be desynchronised from everyone else.
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
    int type = -1;
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
        std::string line = Trim(raw);
        if (line.empty()) continue;

        std::vector<std::string> t = Tokens(line);
        if (t.size() < kFieldsMin || t.size() > kFieldsMax) { malformed++; continue; }

        Line entry;
        entry.type = WeaponTypeByName(t[0]);
        if (entry.type < 0) {
            if (unknown < 5)
                Log::Write("[%s] weapons: San Andreas has no weapon type called %s; the line is "
                           "dropped rather than written over UNARMED, which is where its own parser "
                           "would have put it", arms.region->name.c_str(), t[0].c_str());
            unknown++;
            continue;
        }
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

    int written = 0;
    for (int skill = 0; skill < 4; skill++) {
        uintptr_t record = WeaponInfoAt(line.type, skill);
        if (!record || !RecordInUse(record)) continue;

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
    if (model && written) *(int32_t*)((uintptr_t)model + kWeaponModelType) = line.type;
    return written;
}

void ApplyAll() {
    for (const RegionArms& arms : Arms()) {
        int weapons = 0, records = 0;
        for (const Line& line : arms.lines) {
            int n = ApplyLine(arms, line);
            if (n) { weapons++; records += n; }
        }
        Log::Write("[%s] weapons: %d San Andreas weapon type(s) taken over, %d skill record(s) "
                   "written - these apply to the whole game, not only inside the region",
                   arms.region->name.c_str(), weapons, records);
        for (const Line& line : arms.lines)
            Log::Debug("[%s] weapons: %s now %s", arms.region->name.c_str(),
                       *(const char* const*)(kWeaponNames + (uintptr_t)line.type * 4),
                       line.sourceName.empty() ? "from the region" : line.sourceName.c_str());
    }
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

int WeaponType(const char* name) { return WeaponTypeByName(name); }

int WeaponModel(int weaponType) {
    uintptr_t record = WeaponInfoAt(weaponType, 1);   // the STD record, which every weapon has
    return record ? *(int32_t*)(record + kModelId1) : -1;
}

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.weaponFile.empty();
    if (!any || !Settings::world.regionWeapons) return;

    // Under SA-MP the server owns what every weapon is and does, and a client whose Desert Eagle
    // had quietly become a Python would be shooting a different gun from everyone else.
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
    Log::Write("weapons: %d line(s) from %u region file(s) will be written over San Andreas' own "
               "weapon table after it loads", lines, (unsigned)Arms().size());
}

}
