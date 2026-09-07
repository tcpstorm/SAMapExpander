// Puts back the pickups a region's own game had lying in its streets - the guns on the rooftops,
// the health outside the hospital, the armour behind the police station - so walking the imported
// city feels like walking that game rather than an empty model of it.
//
// They are not in the map files. GTA III and Vice City create almost every world pickup from the
// main script, with CREATE_PICKUP-family opcodes and literal coordinates, and this plugin does not
// run their scripts. tools/import_city.py therefore walks the source main.scm offline, keeps the
// placements that are permanent rather than mission-owned, translates each one to the San Andreas
// weapon type or model that means the same thing, and writes them to the region's pickups.dat in
// the region's own coordinates. Everything difficult happens there; this file reads a plain table
// and calls the game.
//
// A weapon pickup names a weapon type rather than a model, deliberately. San Andreas takes the
// model a weapon pickup shows from CWeaponInfo, so naming the type means the pickup follows the
// region's own weapons.dat: with it loaded, the pickup that gives you a Desert Eagle is a Python
// lying in the street, and without it the same line still works and shows the Desert Eagle.
//
// CPickups::aPickUps is 620 fixed entries at 0x9788C0, 0x20 bytes each, with the type byte at +0x1C
// and 0 meaning free; CPickups::GenerateNewOne (0x456F20) scans it and returns -1 when there is no
// room. San Andreas' own script wants a large part of that - fifty oysters, fifty horseshoes, its
// own street weapons - and it asks for them after this runs, so a region is never allowed to fill
// the array: PickupReserve slots are counted out and kept, and whatever does not fit is dropped
// with a line in the log saying how many and why. The array is not relocated. It could be, the way
// ModelLimits relocates the streaming arrays, but the array is also what CPickups::Save (0x5D3540)
// writes into a save game, so growing it would quietly make saves that vanilla San Andreas and
// every other mod read wrongly - a poor trade for a few dozen more crates.
//
// The moment to place them is a post hook on CGame::ReInitGameObjectVariables (0x53BCF0). By then
// CGame::ShutDownForRestart has run CWorld::ClearForRestart -> CPickups::ReInit (0x456E60), which
// empties the array on every new game and every load, and the same function has already called
// CWeapon::InitialiseWeapons, so the region's own weapons.dat has been written and a weapon pickup
// resolves to the model it will actually show.
//
// Nothing here runs under SA-MP: the server owns the pickups there, and ours would exist only on
// this client, which is worse than not having them.
#include <plugin.h>
#include <safetyhook.hpp>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "RegionPickups.h"
#include "RegionWeapons.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kPickupArray = 0x9788C0;   // CPickups::aPickUps[620]
constexpr unsigned kPickupStride = 0x20;
constexpr int kPickupCount = 620;
constexpr unsigned kPickupTypeOffset = 0x1C;   // 0 is PICKUP_NONE, i.e. a free slot
constexpr unsigned kPickupModelOffset = 0x18;  // uint16, written by GenerateNewOne at 0x45729A

// CPickups::GenerateNewOne. The position is a CVector passed by value, which for __cdecl is three
// floats on the stack and nothing else - spelling it that way keeps the cross-compiler out of the
// argument-passing rules for a class type, which it does not have to agree with MSVC about.
typedef int(__cdecl* GenerateNewOneFn)(float x, float y, float z, unsigned int modelIndex,
                                       unsigned char type, unsigned int ammo, unsigned int moneyPerDay,
                                       bool minigame, const char* message);
const GenerateNewOneFn CallGenerateNewOne = reinterpret_cast<GenerateNewOneFn>(0x456F20);

struct Pickup {
    int  weaponType;   // -1 when the line names a model instead
    int  modelId;      // -1 when the line names a weapon
    int  type;         // ePickupType, 2 = PICKUP_ON_STREET
    int  ammo;
    float x, y, z;     // the region's own coordinates; the origin is added at placement
    std::string what;  // what the source game called it, for the log
};

struct RegionState {
    const MapRegion* region;
    std::vector<Pickup> pickups;
};

std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }

// The first slot this placed and what went in it - see AlreadyPlaced below.
int gWitnessSlot = -1;
int gWitnessModel = -1;

int FreePickupSlots() {
    int free = 0;
    for (int i = 0; i < kPickupCount; i++)
        if (*(uint8_t*)(kPickupArray + (uintptr_t)i * kPickupStride + kPickupTypeOffset) == 0) free++;
    return free;
}

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

// kind, what, pickup type, ammo, x, y, z - see MapExpander.ini for the column meanings.
bool ReadPickupFile(RegionState& rs) {
    FILE* f = fopen(rs.region->pickupFile.c_str(), "rb");
    if (!f) {
        Log::Write("[%s] pickups: %s cannot be opened", rs.region->name.c_str(),
                   rs.region->pickupFile.c_str());
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);

    int malformed = 0, unknownWeapon = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(at, end - at);
        at = end + 1;

        std::string comment;
        size_t hash = raw.find('#');
        if (hash != std::string::npos) { comment = Trim(raw.substr(hash + 1)); raw = raw.substr(0, hash); }
        std::string line = Trim(raw);
        if (line.empty() || line[0] == ';') continue;   // ';' comments out a placement, as in the ini
        std::vector<std::string> t = Tokens(line);
        if (t.size() != 7) { malformed++; continue; }

        Pickup p;
        p.weaponType = -1;
        p.modelId = -1;
        if (_stricmp(t[0].c_str(), "weapon") == 0) {
            p.weaponType = RegionWeapons::WeaponType(t[1].c_str());
            if (p.weaponType < 0) {
                if (unknownWeapon < 5)
                    Log::Write("[%s] pickups: San Andreas has no weapon called %s; the pickup is "
                               "left out", rs.region->name.c_str(), t[1].c_str());
                unknownWeapon++;
                continue;
            }
        } else if (_stricmp(t[0].c_str(), "model") == 0) {
            p.modelId = atoi(t[1].c_str());
        } else {
            malformed++;
            continue;
        }
        p.type = atoi(t[2].c_str());
        p.ammo = atoi(t[3].c_str());
        p.x = (float)atof(t[4].c_str());
        p.y = (float)atof(t[5].c_str());
        p.z = (float)atof(t[6].c_str());
        p.what = comment.empty() ? t[1] : comment;
        rs.pickups.push_back(p);
    }

    if (malformed || unknownWeapon)
        Log::Write("[%s] pickups: %d malformed line(s), %d naming a weapon San Andreas does not have",
                   rs.region->name.c_str(), malformed, unknownWeapon);
    return !rs.pickups.empty();
}

void PlaceAll() {
    const int reserve = Settings::world.pickupReserve < 0 ? 0 : Settings::world.pickupReserve;
    int freeAtStart = FreePickupSlots();

    // A CPickup keeps its position as int16 * 1/CompressedPositionScale, and PickupFix widens that
    // scale for exactly this reason. A region beyond what the scale reaches would still take a
    // slot and then sit somewhere else entirely, which looks like a bug in the placements rather
    // than in the ini, so say so instead.
    const float reach = 32767.0f / (Settings::world.fixPickupPositions
                                    ? Settings::world.compressedPositionScale : 8.0f);

    for (RegionState& rs : Regions()) {
        int placed = 0, noRoom = 0, noModel = 0, refused = 0, outOfReach = 0;
        for (const Pickup& p : rs.pickups) {
            if (FreePickupSlots() <= reserve) { noRoom++; continue; }

            int model = p.modelId;
            if (p.weaponType >= 0) model = RegionWeapons::WeaponModel(p.weaponType);
            if (model <= 0) { noModel++; continue; }

            float x = p.x + rs.region->originX;
            float y = p.y + rs.region->originY;
            float z = p.z + rs.region->originZ;
            if (x < -reach || x > reach || y < -reach || y > reach || z < -reach || z > reach) {
                outOfReach++;
                continue;
            }

            int index = CallGenerateNewOne(x, y, z, (unsigned)model, (unsigned char)p.type,
                                           (unsigned)p.ammo, 0, false, nullptr);
            if (index < 0) { refused++; continue; }
            // The low half of the return is the slot, the high half its reference index.
            if (gWitnessSlot < 0) { gWitnessSlot = index & 0xFFFF; gWitnessModel = model; }
            placed++;
        }

        Log::Write("[%s] pickups: %d placed of %u; %d dropped to keep %d slot(s) for San Andreas' "
                   "own script, %d with no model, %d the game refused, %d past the +/-%.0f a "
                   "compressed pickup position reaches",
                   rs.region->name.c_str(), placed, (unsigned)rs.pickups.size(), noRoom, reserve,
                   noModel, refused, outOfReach, reach);
        if (outOfReach)
            Log::Write("[%s] pickups: a lower CompressedPositionScale reaches further, at the cost "
                       "of a coarser step; the vanilla 8 only reaches +/-4096",
                       rs.region->name.c_str());
        if (noRoom)
            Log::Write("[%s] pickups: San Andreas has %d pickup slots in all and its own script "
                       "wants a large part of them; lower PickupReserve to give the region more, "
                       "knowing that what runs out next is the oysters and horseshoes",
                       rs.region->name.c_str(), kPickupCount);
    }
    Log::Debug("pickups: %d of %d slots were free before the regions, %d after",
               freeAtStart, kPickupCount, FreePickupSlots());
}

// CGame::ReInitGameObjectVariables is called from two places in CGame::InitialiseWhenRestarting -
// one for a new game, one for a loaded one - and only one of them runs, but nothing in the code
// says so. So the first pickup this places is remembered by slot and by model, and finding that
// slot still holding that model means these are our own pickups and the array has not been emptied
// since: cheaper, and more honest, than assuming which of the two call sites ran.
bool AlreadyPlaced() {
    if (gWitnessSlot < 0) return false;
    uintptr_t entry = kPickupArray + (uintptr_t)gWitnessSlot * kPickupStride;
    return *(uint8_t*)(entry + kPickupTypeOffset) != 0 &&
           *(uint16_t*)(entry + kPickupModelOffset) == (uint16_t)gWitnessModel;
}

SafetyHookInline& ReInitOriginal() { static SafetyHookInline h; return h; }

void __cdecl ReInitHook() {
    ReInitOriginal().ccall<void>();
    if (AlreadyPlaced()) return;
    gWitnessSlot = -1;
    PlaceAll();
}

}   // namespace

namespace RegionPickups {

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.pickupFile.empty();
    if (!any || !Settings::world.regionPickups) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region pickup import is off, the server places the pickups");
        return;
    }

    for (const MapRegion& r : Settings::regions) {
        if (r.pickupFile.empty()) continue;
        RegionState rs{ &r };
        if (ReadPickupFile(rs)) Regions().push_back(rs);
    }
    if (Regions().empty()) return;

    ReInitOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x53BCF0),
                                                reinterpret_cast<void*>(&ReInitHook));
    if (!ReInitOriginal()) {
        Log::Write("pickups: the hook on CGame::ReInitGameObjectVariables failed; the region's "
                   "pickups are left out");
        Regions().clear();
        return;
    }
    int total = 0;
    for (const RegionState& rs : Regions()) total += (int)rs.pickups.size();
    Log::Write("pickups: %d placement(s) from %u region file(s), to go in when a game starts",
               total, (unsigned)Regions().size());
}

}
