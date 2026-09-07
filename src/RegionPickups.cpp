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
// and 0 meaning free; CPickups::GenerateNewOne (0x456F20) takes the first free entry it finds,
// counting up from index 0, and returns -1 when there is none. San Andreas' own script wants a
// large part of that array - fifty oysters, fifty horseshoes, its own street weapons - and it asks
// for them before this runs, so a region used to meet 69 free slots against the 291 the two shipped
// regions want, and every single one of them was dropped.
//
// So the array is enlarged instead, by PickupSlots, which moves it to a block sized from what the
// regions actually ask for. San Andreas' own entries are not touched, moved or recycled: what it
// has, it keeps. The savegame keeps looking at exactly the first 620 entries, so it stays byte for
// byte what vanilla writes - that is the whole reason the move is safe, and PickupSlots.cpp is
// where it is explained.
//
// A region's pickups therefore have to end up ABOVE those first 620, or a save made inside the
// region would carry them - and their region model ids - into a vanilla San Andreas that has no
// such models. GenerateNewOne takes the first free slot from index 0 and there is no telling it
// otherwise, so while this places, the free entries below 620 are made to look occupied and put
// back afterwards. Nothing of San Andreas' is written: an entry it is using is left alone, and an
// entry it is not using is free again by the time the function returns.
//
// PickupReserve is still what protects San Andreas' script from us, but from the other end now: it
// is counted into the array's new size, so that after the region has taken everything it wants
// there are still that many free entries for the script to find. It is no longer the thing that
// stops a region getting in at all.
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
#include "PickupSlots.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr unsigned kPickupStride = 0x20;
constexpr unsigned kPickupTypeOffset = 0x1C;   // 0 is PICKUP_NONE, i.e. a free slot
constexpr unsigned kPickupModelOffset = 0x18;  // uint16, written by GenerateNewOne at 0x45729A

// What a free entry below San Andreas' own 620 is set to while a region is being placed. Any
// non-zero value would do: GenerateNewOne's first search wants a 0, and the two searches behind it
// - which only run when the first found nothing - want an 8 (PICKUP_MONEY) or a 4
// (PICKUP_ONCE_TIMEOUT), the two kinds it is allowed to take over. This is none of the three, and
// it is not a type any switch in CPickup::Update has a case for either, so an entry wearing it for
// the length of one call is invisible to everything.
constexpr uint8_t kBlockedType = 0xFF;

uintptr_t PickupEntry(int index) {
    return PickupSlots::Base() + (uintptr_t)index * kPickupStride;
}
uint8_t PickupType(int index) {
    return *(uint8_t*)(PickupEntry(index) + kPickupTypeOffset);
}
void SetPickupType(int index, uint8_t type) {
    *(uint8_t*)(PickupEntry(index) + kPickupTypeOffset) = type;
}

// CPickups::GenerateNewOne. The position is a CVector passed by value, which for __cdecl is three
// floats on the stack and nothing else - spelling it that way keeps the cross-compiler out of the
// argument-passing rules for a class type, which it does not have to agree with MSVC about.
typedef int(__cdecl* GenerateNewOneFn)(float x, float y, float z, unsigned int modelIndex,
                                       unsigned char type, unsigned int ammo, unsigned int moneyPerDay,
                                       bool minigame, const char* message);
const GenerateNewOneFn CallGenerateNewOne = reinterpret_cast<GenerateNewOneFn>(0x456F20);

// CPickups::RemovePickUp, taking the same packed handle GenerateNewOne returns - the slot in the
// low half, that entry's reference index in the high half, which is how it tells a live pickup from
// a slot that has since been reused.
typedef void(__cdecl* RemovePickUpFn)(int handle);
const RemovePickUpFn CallRemovePickUp = reinterpret_cast<RemovePickUpFn>(0x4573D0);

struct Pickup {
    int  weaponType;   // resolved from weaponName at placement; -1 when the line names a model
    std::string weaponName;   // as written in the file, looked up when placing - see ReadPickupFile
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

// Free entries in [from, PickupSlots::Count()). `from` is 0 for the whole array and 620 for the
// part above San Andreas' own, which is the only part a region is allowed into once the array has
// been enlarged.
int FreePickupSlots(int from = 0) {
    int free = 0;
    for (int i = from; i < PickupSlots::Count(); i++)
        if (PickupType(i) == 0) free++;
    return free;
}

// The free entries below San Andreas' own end, made to look occupied so that GenerateNewOne - which
// always settles on the first free entry it finds from index 0 - cannot put a region's pickup
// anywhere the savegame would see it. Occupied entries are not touched: San Andreas keeps what it
// has, exactly where it has it.
std::vector<int> gBlocked;

void BlockVanillaSlots() {
    gBlocked.clear();
    if (!PickupSlots::Grown()) return;   // no room above 620: the old rules apply, see PlaceAll
    for (int i = 0; i < PickupSlots::VanillaCount(); i++) {
        if (PickupType(i) != 0) continue;
        SetPickupType(i, kBlockedType);
        gBlocked.push_back(i);
    }
}

void UnblockVanillaSlots() {
    for (int i : gBlocked)
        if (PickupType(i) == kBlockedType) SetPickupType(i, 0);
    gBlocked.clear();
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
            // Kept as written and looked up when the pickup is placed, not now.
            //
            // A file names the import rather than the San Andreas weapon it borrows from -
            // LASERSCOPE, not SNIPERRIFLE - because two imports can share a donor, and when the
            // .308 moved onto the sniper rifle Vice City's own sniper was already there, so both
            // lines read the same and both handed over the .308. Asking for the import by its own
            // name settles that. But those names do not exist yet at this point: this file is read
            // while the plugin starts, and the new weapon types are not claimed until the region's
            // weapons.dat is applied much later. Resolving here dropped every one of them as a
            // weapon San Andreas has never heard of, which took the guns out of the streets
            // altogether.
            p.weaponName = t[1];
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
    const bool grown = PickupSlots::Grown();
    const int ours = grown ? PickupSlots::VanillaCount() : 0;   // the first entry we may write
    int freeAtStart = FreePickupSlots();

    // Hide San Andreas' end of the array for the length of this function, so every placement below
    // lands above it. When the array could not be enlarged there is no "above it" and this does
    // nothing: the old rule then applies unchanged, PickupReserve slots are counted out and kept,
    // and what does not fit is dropped.
    BlockVanillaSlots();

    // A CPickup keeps its position as int16 * 1/CompressedPositionScale, and PickupFix widens that
    // scale for exactly this reason. A region beyond what the scale reaches would still take a
    // slot and then sit somewhere else entirely, which looks like a bug in the placements rather
    // than in the ini, so say so instead.
    const float reach = 32767.0f / (Settings::world.fixPickupPositions
                                    ? Settings::world.compressedPositionScale : 8.0f);

    for (RegionState& rs : Regions()) {
        int placed = 0, noRoom = 0, noModel = 0, refused = 0, outOfReach = 0, strayed = 0;
        for (const Pickup& p : rs.pickups) {
            // Above San Andreas' own entries we may use everything that is free, because the
            // reserve was already counted into the array's size; without the enlarged array we are
            // sharing its 620 and have to stop short of the reserve.
            if (grown ? FreePickupSlots(ours) <= 0 : FreePickupSlots() <= reserve) {
                noRoom++;
                continue;
            }

            // Now that the region's weapons.dat has been applied, an imported gun answers to its
            // own name, so this is the first moment the lookup can succeed.
            int weaponType = p.weaponName.empty() ? -1 : RegionWeapons::WeaponType(p.weaponName.c_str());
            if (!p.weaponName.empty() && weaponType < 0) { noModel++; continue; }
            int model = p.modelId;
            if (weaponType >= 0) model = RegionWeapons::WeaponModel(weaponType);
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
            int slot = index & 0xFFFF;
            // Belt and braces. This cannot happen while the blocking above holds - and if it ever
            // did, the pickup would be inside the part of the array the savegame copies, which is
            // the one thing this whole design exists to prevent. So take it straight back out.
            if (slot < ours) {
                CallRemovePickUp(index);
                strayed++;
                continue;
            }
            if (gWitnessSlot < 0) { gWitnessSlot = slot; gWitnessModel = model; }
            placed++;
        }

        Log::Write("[%s] pickups: %d placed of %u; %d dropped for want of a slot, %d with no model, "
                   "%d the game refused, %d past the +/-%.0f a compressed pickup position reaches",
                   rs.region->name.c_str(), placed, (unsigned)rs.pickups.size(), noRoom, noModel,
                   refused, outOfReach, reach);
        if (outOfReach)
            Log::Write("[%s] pickups: a lower CompressedPositionScale reaches further, at the cost "
                       "of a coarser step; the vanilla 8 only reaches +/-4096",
                       rs.region->name.c_str());
        if (strayed)
            Log::Write("[%s] pickups: %d landed among San Andreas' own entries and were taken back "
                       "out; the region's pickups must stay above them or a save game would carry "
                       "them", rs.region->name.c_str(), strayed);
        if (noRoom && grown)
            Log::Write("[%s] pickups: the array holds %d entries and %d of them are above San "
                       "Andreas' own %d, which is the only part a region may use - and they are "
                       "gone. The array was sized to fit every placement, so something else has "
                       "taken them: raise PickupReserve, which is counted into that size",
                       rs.region->name.c_str(), PickupSlots::Count(),
                       PickupSlots::Count() - PickupSlots::VanillaCount(),
                       PickupSlots::VanillaCount());
        if (noRoom && !grown)
            Log::Write("[%s] pickups: the array could not be enlarged, so the region is sharing San "
                       "Andreas' %d slots and its own script wants a large part of them; lower "
                       "PickupReserve to give the region more, knowing that what runs out next is "
                       "the oysters and horseshoes",
                       rs.region->name.c_str(), PickupSlots::VanillaCount());
    }

    UnblockVanillaSlots();
    Log::Debug("pickups: %d of %d slots were free before the regions, %d after (%d free above San "
               "Andreas' own %d)",
               freeAtStart, PickupSlots::Count(), FreePickupSlots(),
               FreePickupSlots(PickupSlots::VanillaCount()), PickupSlots::VanillaCount());
}

// CGame::ReInitGameObjectVariables is called from two places in CGame::InitialiseWhenRestarting -
// one for a new game, one for a loaded one - and only one of them runs, but nothing in the code
// says so. So the first pickup this places is remembered by slot and by model, and finding that
// slot still holding that model means these are our own pickups and the array has not been emptied
// since: cheaper, and more honest, than assuming which of the two call sites ran.
bool AlreadyPlaced() {
    if (gWitnessSlot < 0 || gWitnessSlot >= PickupSlots::Count()) return false;
    uintptr_t entry = PickupEntry(gWitnessSlot);
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

// And a frame check behind it, because the hook above is not enough on its own.
//
// CGame::ReInitGameObjectVariables has exactly two callers, both inside
// CGame::InitialiseWhenRestarting, and that did not run at all on a session that loaded a save and
// played for two minutes: the pickups were read, the hook was in place, and nothing was ever put
// in the world. Which of San Andreas' several ways into a game reaches that function is not worth
// establishing, because there is a check that does not care.
//
// So look once a frame instead. AlreadyPlaced already answers "are these still ours", by
// remembering the slot and model of the first pickup written and seeing whether that slot still
// holds it, so it distinguishes all three states this needs: ours are in (leave them), the array
// has been emptied under us (a new game - put them back), or nothing has been placed yet. Placing
// is attempted once per game, not once per frame, so a placement that legitimately puts nothing in
// - no room left, every model refused - says so once and is not retried in a loop.
SafetyHookInline& FrameOriginal() { static SafetyHookInline h; return h; }
bool gAttempted = false;
bool gWasPlaced = false;

void __cdecl FrameHook() {
    FrameOriginal().ccall<void>();
    if (!FindPlayerPed()) {          // in a menu or a loading screen; arm for the next game
        gAttempted = false;
        gWasPlaced = false;
        return;
    }
    if (AlreadyPlaced()) {           // in the world already, by this or by the hook above
        gAttempted = true;
        gWasPlaced = true;
        return;
    }
    if (gWasPlaced) {                // they were ours and are gone: the game restarted
        gAttempted = false;
        gWasPlaced = false;
    }
    if (gAttempted) return;
    gAttempted = true;
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

    // Enlarge the array before anything else touches it, and size it from what the regions turned
    // out to be asking for rather than from a number written down here. Nothing above has patched
    // the game yet, and this is well below the SA-MP check, so under SA-MP not a byte is written.
    // If it fails it says why and leaves the game vanilla; the placement below then works the way
    // it did before, sharing San Andreas' own 620 and keeping PickupReserve of them back.
    int wanted = 0;
    for (const RegionState& rs : Regions()) wanted += (int)rs.pickups.size();
    PickupSlots::Grow(wanted, Settings::world.pickupReserve < 0 ? 0 : Settings::world.pickupReserve);

    ReInitOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x53BCF0),
                                                reinterpret_cast<void*>(&ReInitHook));
    // CEntryExitManager::Update, once a frame from CGame::Process and only while a game is running.
    FrameOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x440D10),
                                               reinterpret_cast<void*>(&FrameHook));
    if (!ReInitOriginal() && !FrameOriginal()) {
        Log::Write("pickups: neither the restart hook nor the frame hook could be placed; the "
                   "region's pickups are left out");
        Regions().clear();
        return;
    }
    if (!FrameOriginal())
        Log::Write("pickups: the frame hook failed; pickups will only go in when the game restarts");
    int total = 0;
    for (const RegionState& rs : Regions()) total += (int)rs.pickups.size();
    Log::Write("pickups: %d placement(s) from %u region file(s), to go in when a game starts",
               total, (unsigned)Regions().size());
}

}
