// Loads III/VC .zon files (and SA IPL "zone" sections) offset by a region's world origin, so SA
// shows that map's own area names on it.
//
// Two things stand in the way. CTheZones::NavigationZoneArray holds 380 zones and vanilla SA fills
// 379 of them (378 lines in info.zon plus SAN_AND from CTheZones::Init), so every added zone has to
// go into a bigger array -- the array is a fixed global, so all 62 code sites that name it are
// re-pointed at load. And the zone labels are GXT keys that only exist in the other game's
// american.gxt, so the display names are registered with Gxt, which merges them into the game's text table.
//
// Those labels are namespaced. A zone label is a GXT key and San Andreas has thousands of its own;
// left as they came, Vice City's DTOWN and GTA III's PARK would answer for San Andreas' keys of the
// same name. tools/import_city.py therefore rewrites the name column of every .zon line to the
// region's tagged key ("VDTOWN", "L~GTBK2") in step with the GXT it emits, and writes a ZoneNames
// file giving each of those keys its display name -- so a region reads correctly whether or not
// the whole donor GXT is loaded. Nothing here has to know the scheme: a label is a key, and both
// ends of it were renamed together. See docs/asset-namespacing.md.
#include <plugin.h>
#include <windows.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <safetyhook.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "Zones.h"
#include "Gxt.h"
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kNaviArray = 0xBA3798;   // CTheZones::NavigationZoneArray[380]
constexpr uintptr_t kNaviTotal = 0xBA3794;   // CTheZones::TotalNumberOfNavigationZones (int16)
constexpr uintptr_t kInfoTotal = 0xBA1DE8;   // CTheZones::TotalNumberOfZoneInfos (int16)
constexpr uintptr_t kCreateZone = 0x5728A0;
constexpr uintptr_t kZonesUpdate = 0x572D10;  // CTheZones::Update, once per frame from CGame::Process
constexpr uintptr_t kZoneRevealed = 0x572130; // CTheZones::GetCurrentZoneLockedOrUnlocked(x, y)
constexpr uintptr_t kFindSmallest = 0x572360; // CTheZones::FindSmallestZoneForPosition
constexpr uintptr_t kLevelFromPos = 0x572300; // CTheZones::GetLevelFromPosition
constexpr uintptr_t kGetForMap = 0x571D90;    // CPlaceName::GetForMap, the pause map's hover name
constexpr uintptr_t kTextGet = 0x6A0050;      // CText::Get, __thiscall
constexpr uintptr_t kTheText = 0xC1B340;
constexpr uintptr_t kCurrLevel = 0xBA6718;    // CTheZones::m_CurrLevel
constexpr uintptr_t kZonesRevealed = 0xBA372C;

// Every lookup that names a place tests z (CTheZones::PointLiesWithinZone, 0x572270) and the pause
// map's CPlaceName::GetForMap (0x571D90) uses the *player's* z for the hovered x/y, so a zone file's
// ground-level band (Vice City: 0..300) would lose the name in the air or in interiors. Navigation
// zones are ranked by x/y size alone, so the full altitude band costs nothing.
constexpr float kZoneFloor = -2000.0f;
constexpr float kZoneCeiling = 2000.0f;

constexpr int kVanillaNaviCapacity = 380;
constexpr int kInfoCapacity = 380;           // CTheZones::ZoneInfoArray, not relocated
constexpr float kVanillaMapEdge = 3000.0f;

// SA's CZone. plugin-sdk's CZone.h describes the III/VC layout (0x30 bytes, CZoneInfo inline);
// SA stores an index into ZoneInfoArray instead and is 0x20 bytes per zone.
struct SaZone {
    char    infoLabel[8];
    char    textLabel[8];
    int16_t x1, y1, z1, x2, y2, z2;
    int16_t infoIndex;
    uint8_t type;
    uint8_t level;
};
static_assert(sizeof(SaZone) == 0x20, "CZone must be 0x20 bytes");

using CreateZoneFn = void(__cdecl*)(const char* infoLabel, int type, float x1, float y1, float z1,
                                    float x2, float y2, float z2, int level, const char* textLabel);

// Every instruction that names NavigationZoneArray (or a field of its first elements), from a
// capstone sweep of .text plus the one relocated copy the HOODLUM loader keeps in its own section.
// {instruction, byte offset of the disp32/imm32 inside it, current value}.
struct RefSite {
    uintptr_t address;
    unsigned  dispOffset;
    uint32_t  oldValue;
};

constexpr RefSite kNaviRefs[] = {
    { 0x403C3C, 3, 0xBA37A8 },   // FindSmallestZoneForPosition entry, moved by the HOODLUM loader
    { 0x44398C, 3, 0xBA37B4 }, { 0x443B3B, 3, 0xBA37B4 }, { 0x443B87, 1, 0xBA37A8 },
    { 0x443C1A, 2, 0xBA3798 }, { 0x443E04, 1, 0xBA37B4 }, { 0x46B4F9, 3, 0xBA37B4 },
    { 0x46B546, 3, 0xBA37B4 }, { 0x46ECD9, 3, 0xBA37B4 }, { 0x46ED3B, 3, 0xBA37B4 },
    { 0x46ED6F, 3, 0xBA37B4 }, { 0x4718EA, 3, 0xBA37B4 }, { 0x47522D, 3, 0xBA37B4 },
    { 0x479642, 3, 0xBA37B4 }, { 0x5721B2, 2, 0xBA3798 }, { 0x5721BB, 2, 0xBA3798 },
    { 0x5721C3, 2, 0xBA379C }, { 0x5721C9, 2, 0xBA379C }, { 0x5721E9, 3, 0xBA37B4 },
    { 0x5721F5, 3, 0xBA37B4 }, { 0x57220C, 3, 0xBA37B4 }, { 0x572368, 3, 0xBA37AA },
    { 0x57236F, 3, 0xBA37B0 }, { 0x572378, 3, 0xBA37AE }, { 0x57238E, 1, 0xBA3798 },
    { 0x57239B, 1, 0xBA37CA }, { 0x572432, 2, 0xBA3798 }, { 0x572598, 1, 0xBA3798 },
    { 0x572674, 1, 0xBA37A0 }, { 0x5726DD, 1, 0xBA3798 }, { 0x5726E2, 2, 0xBA379C },
    { 0x5726E8, 1, 0xBA37A0 }, { 0x5726ED, 2, 0xBA37A4 }, { 0x572707, 3, 0xBA37A8 },
    { 0x57270E, 3, 0xBA37AA }, { 0x572715, 3, 0xBA37AC }, { 0x57271E, 3, 0xBA37AE },
    { 0x572725, 3, 0xBA37B0 }, { 0x57272C, 3, 0xBA37B2 }, { 0x572735, 2, 0xBA37B7 },
    { 0x57273B, 2, 0xBA37B6 }, { 0x572A98, 2, 0xBA3798 }, { 0x572AB4, 2, 0xBA37A0 },
    { 0x572ACE, 2, 0xBA37B6 }, { 0x572ADD, 3, 0xBA37A8 }, { 0x572AED, 3, 0xBA37AA },
    { 0x572AFD, 3, 0xBA37AC }, { 0x572B0D, 3, 0xBA37AE }, { 0x572B1D, 3, 0xBA37B0 },
    { 0x572B2E, 3, 0xBA37B2 }, { 0x572B39, 2, 0xBA37B7 }, { 0x572BB6, 1, 0xBA3798 },
    { 0x572C0A, 1, 0xBA3798 }, { 0x572C96, 2, 0xBA3798 }, { 0x572CA0, 2, 0xBA379C },
    { 0x572CCC, 3, 0xBA37B4 }, { 0x5866C1, 1, 0xBA37A8 }, { 0x5D268F, 1, 0xBA3798 },
    { 0x5D278A, 1, 0xBA3798 }, { 0x5D2EA6, 1, 0xBA3798 }, { 0x5D2F8A, 1, 0xBA3798 },
    { 0x1569A00, 3, 0xBA37B4 },  // CanPlayerStartAGangWarHere, moved by the HOODLUM loader
};

struct ZoneDef {
    char  infoLabel[8];
    char  textLabel[8];
    float x1, y1, z1, x2, y2, z2;
    int   level;
};

// Install() runs during the DLL's static initialisation from Main.cpp, before this file's globals
// are guaranteed to be constructed; function-local statics are constructed on first use.
std::vector<ZoneDef>& Defs() { static std::vector<ZoneDef> v; return v; }
std::map<std::string, std::string>& NameMap() { static std::map<std::string, std::string> m; return m; }  // GXT key -> display name

SaZone* gZones = reinterpret_cast<SaZone*>(kNaviArray);
int gCapacity = kVanillaNaviCapacity;

// What the array looked like when we last filled it. Anything else (a new game, a savegame that
// restored a different set) means the zones have to be put back.
int gExpectedTotal = -1;
SaZone gExpectedLast{};

SafetyHookInline gUpdateHook;
SafetyHookInline gRevealedHook;

int16_t& NaviTotal() { return *reinterpret_cast<int16_t*>(kNaviTotal); }
int16_t& InfoTotal() { return *reinterpret_cast<int16_t*>(kInfoTotal); }

// ---------------------------------------------------------------- keys and names

// Zone labels are GXT keys. CTheZones::CreateZone copies at most 7 characters and upper-cases them,
// so that is what a zone can ever be looked up by.
void MakeKey(const char* in, char out[8]) {
    memset(out, 0, 8);
    for (int i = 0; i < 7 && in[i]; i++) out[i] = (char)toupper((unsigned char)in[i]);
}

// Last resort when nothing names a zone: spell its key out. The importer's keys are namespaced
// (docs/asset-namespacing.md), so this reads as "V Beach3" rather than "Beach3" and is meant to
// look wrong -- a region imported by tools/import_city.py ships a ZoneNames file that covers every
// label, and this only shows up for a zone file somebody wrote by hand.
//
// The '~' a hashed key carries is turned into a space along with '_', and not because it is ugly:
// CFont reads '~' as the start of a colour or button token and would eat the rest of the line,
// which is the one way a bad zone name could do more than look silly.
std::string DeriveName(const char* key) {
    std::string out;
    bool wordStart = true;
    for (const char* p = key; *p; p++) {
        char c = *p;
        if (c == '_' || c == '~') { out.push_back(' '); wordStart = true; continue; }
        out.push_back(wordStart ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c));
        wordStart = false;
    }
    return out;
}

// ---------------------------------------------------------------- file parsing

std::string TrimCopy(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string StripComment(const std::string& line) {
    std::string s = line;
    size_t cut = s.find_first_of(";#");
    if (cut != std::string::npos) s.erase(cut);
    cut = s.find("//");
    if (cut != std::string::npos) s.erase(cut);
    return s;
}

void Tokenize(std::string s, std::vector<std::string>& out) {
    out.clear();
    for (char& c : s) if (c == ',' || c == '\t' || c == '\r') c = ' ';
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t start = i;
        while (i < s.size() && s[i] != ' ') i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
}

// III/VC .zon and SA IPL "zone" lines: NAME type x1 y1 z1 x2 y2 z2 island [gxtkey].
// Types 0 and 1 are the ones whose name the HUD shows (SA's CreateZone puts both in the navigation
// array); 2 (info) and 3 (map) are ignored.
int LoadZoneFile(const MapRegion& region, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        Log::Write("[%s] zone file not found: %s", region.name.c_str(), path.c_str());
        return 0;
    }

    bool sawSection = false, inZoneSection = false;
    int added = 0, ignoredType = 0, malformed = 0;
    std::string line;
    std::vector<std::string> t;
    while (std::getline(in, line)) {
        std::string clean = TrimCopy(StripComment(line));
        if (clean.empty()) continue;

        Tokenize(clean, t);
        if (t.size() == 1) {
            sawSection = true;
            std::string name = t[0];
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (name == "end") inZoneSection = false;
            else inZoneSection = (name == "zone");
            continue;
        }
        if (sawSection && !inZoneSection) continue;
        if (t.size() < 9) { malformed++; continue; }

        int type = atoi(t[1].c_str());
        if (type != 0 && type != 1) { ignoredType++; continue; }

        ZoneDef d{};
        // CTheZones::AssignZoneInfoForThisZone reuses the CZoneInfo of any existing zone with the
        // same info label and allocates a new one otherwise; vanilla SA fills 379 of the 380 slots,
        // so region zones share one existing label and keep only their own text label.
        MakeKey(region.zoneInfoLabel.c_str(), d.infoLabel);
        MakeKey(t.size() >= 10 ? t[9].c_str() : t[0].c_str(), d.textLabel);
        d.x1 = strtof(t[2].c_str(), nullptr) + region.originX;
        d.y1 = strtof(t[3].c_str(), nullptr) + region.originY;
        d.z1 = std::min(strtof(t[4].c_str(), nullptr) + region.originZ, kZoneFloor);
        d.x2 = strtof(t[5].c_str(), nullptr) + region.originX;
        d.y2 = strtof(t[6].c_str(), nullptr) + region.originY;
        d.z2 = std::max(strtof(t[7].c_str(), nullptr) + region.originZ, kZoneCeiling);
        d.level = atoi(t[8].c_str());
        if (d.level < 0 || d.level > 3) d.level = 0;

        // CreateZone truncates the corners to int16.
        float lo = std::min(d.x1, d.x2), hi = std::max(d.x1, d.x2);
        if (lo < -32000.0f || hi > 32000.0f) { malformed++; continue; }
        lo = std::min(d.y1, d.y2); hi = std::max(d.y1, d.y2);
        if (lo < -32000.0f || hi > 32000.0f) { malformed++; continue; }

        Defs().push_back(d);
        added++;
    }

    Log::Write("[%s] %s: %d zone(s), %d of other types, %d unusable line(s)",
               region.name.c_str(), path.c_str(), added, ignoredType, malformed);
    return added;
}

void LoadNameFile(const MapRegion& region, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        Log::Write("[%s] zone name file not found: %s", region.name.c_str(), path.c_str());
        return;
    }
    int count = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::string clean = TrimCopy(StripComment(line));
        if (clean.empty()) continue;
        size_t sep = clean.find('=');
        if (sep == std::string::npos) sep = clean.find_first_of(" \t");
        if (sep == std::string::npos) continue;
        char key[8];
        MakeKey(TrimCopy(clean.substr(0, sep)).c_str(), key);
        if (!key[0]) continue;
        std::string text = TrimCopy(clean.substr(sep + 1));
        if (text.empty()) continue;
        NameMap()[std::string(key, strnlen(key, 8))] = text;
        count++;
    }
    Log::Write("[%s] %s: %d display name(s)", region.name.c_str(), path.c_str(), count);
}

// ---------------------------------------------------------------- array relocation

bool Readable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return address + bytes <= regionEnd;
}

bool RelocateNaviArray(int capacity) {
    Log::Debug("zones: checking %d reference sites", (int)std::size(kNaviRefs));
    for (const RefSite& r : kNaviRefs) {
        if (!Readable(r.address, r.dispOffset + 4)) {
            Log::Write("zone array not relocated: 0x%X is not readable memory (packed or unmapped section)",
                       (unsigned)r.address);
            return false;
        }
        uint32_t current = (uint32_t)patch::GetUInt(r.address + r.dispOffset);
        if (current != r.oldValue) {
            Log::Write("zone array not relocated: 0x%X holds 0x%X, expected 0x%X (another mod moved it?)",
                       (unsigned)r.address, current, r.oldValue);
            return false;
        }
    }

    Log::Debug("zones: all sites verified, relocating");
    SaZone* moved = (SaZone*)calloc((size_t)capacity, sizeof(SaZone));
    if (!moved) return false;
    memcpy(moved, reinterpret_cast<const void*>(kNaviArray), kVanillaNaviCapacity * sizeof(SaZone));

    int patched = 0;
    for (const RefSite& r : kNaviRefs) {
        const void* target = reinterpret_cast<const uint8_t*>(moved) + (r.oldValue - kNaviArray);
        if (MemPatch::RetargetDisp32(r.address, r.dispOffset, r.oldValue, target)) patched++;
    }

    gZones = moved;
    gCapacity = capacity;
    Log::Write("zones: navigation zone array capacity %d -> %d, %d/%d references re-pointed",
               kVanillaNaviCapacity, capacity, patched, (int)std::size(kNaviRefs));
    Log::Debug("zones: array moved from 0x%X to %p", (unsigned)kNaviArray, (void*)moved);
    return patched == (int)std::size(kNaviRefs);
}

// ---------------------------------------------------------------- zone creation

bool IsOneOfOurs(const SaZone& z) {
    // Our zones sit outside the vanilla map; every zone SA loads itself is inside it. Together with
    // the label that is enough to recognise ours after a savegame restored them.
    bool insideVanillaMap = z.x1 >= -kVanillaMapEdge && z.x2 <= kVanillaMapEdge &&
                            z.y1 >= -kVanillaMapEdge && z.y2 <= kVanillaMapEdge;
    if (insideVanillaMap) return false;
    for (const ZoneDef& d : Defs())
        if (memcmp(z.textLabel, d.textLabel, 8) == 0 && memcmp(z.infoLabel, d.infoLabel, 8) == 0)
            return true;
    return false;
}

void EnsureZones() {
    int16_t& total = NaviTotal();
    if (total == gExpectedTotal && total > 0 &&
        memcmp(&gZones[total - 1], &gExpectedLast, sizeof(SaZone)) == 0)
        return;

    int removed = 0;
    while (total > 1 && IsOneOfOurs(gZones[total - 1])) { total--; removed++; }
    if (removed) {
        // Zone infos are handed out in order, one per distinct info label, so the highest index the
        // remaining zones use is the last one the game itself created.
        int16_t highest = 0;
        for (int i = 0; i < total; i++)
            if (gZones[i].infoIndex > highest) highest = gZones[i].infoIndex;
        if (InfoTotal() > highest + 1) InfoTotal() = (int16_t)(highest + 1);
    }

    int vanilla = total;
    CreateZoneFn CreateZone = reinterpret_cast<CreateZoneFn>(kCreateZone);
    int created = 0, dropped = 0;
    for (const ZoneDef& d : Defs()) {
        bool infoExists = false;
        for (int i = 0; i < total && !infoExists; i++)
            infoExists = memcmp(gZones[i].infoLabel, d.infoLabel, 8) == 0;
        if (total >= gCapacity || (!infoExists && InfoTotal() >= kInfoCapacity)) { dropped++; continue; }
        CreateZone(d.infoLabel, 0, d.x1, d.y1, d.z1, d.x2, d.y2, d.z2, d.level, d.textLabel);
        created++;
    }

    gExpectedTotal = total;
    if (total > 0) gExpectedLast = gZones[total - 1];
    Log::Write("zones: game had %d (%d of ours dropped first), added %d, total %d/%d, zone infos %d/%d",
               vanilla, removed, created, (int)total, gCapacity, (int)InfoTotal(), kInfoCapacity);
    if (dropped)
        Log::Write("ERROR: %d zone(s) did not fit and were skipped; the last ones in the file are missing",
                   dropped);
}

// CTheZones::Update marks ZonesVisited[10][10] with an unclamped (pos + 3000) / 600 index, so a
// player beyond the vanilla map writes past the array into the zone globals behind it. The only
// other thing it does is refresh m_CurrLevel from the map zones, which do not exist out there.
bool PlayerOutsideVanillaMap() {
    CPlayerPed* player = FindPlayerPed();
    if (!player) return false;
    CVector pos = player->GetPosition();
    return pos.x <= -kVanillaMapEdge || pos.x >= kVanillaMapEdge ||
           pos.y <= -kVanillaMapEdge || pos.y >= kVanillaMapEdge;
}

void __cdecl Hooked_ZonesUpdate() {
    if (!PlayerOutsideVanillaMap()) gUpdateHook.ccall<void>();
    EnsureZones();
}

// CTheZones::GetCurrentZoneLockedOrUnlocked (0x572130) indexes the 10x10 ZonesVisited grid with
// (x + 3000) / 600 and never bounds-checks, so a point in a region lands well past the table. Its
// callers -- the pause map's area-name gate (0x575E38), CRadar::HasThisBlipBeenRevealed (0x583B23)
// and the "unlock the zone the player stands in" write (0x57283D) -- all want a plain "yes" out
// here: names and blips show, and nothing is written past the table.
bool __cdecl Hooked_ZoneRevealed(float x, float y) {
    if (x < -kVanillaMapEdge || x > kVanillaMapEdge || y < -kVanillaMapEdge || y > kVanillaMapEdge)
        return true;
    return gRevealedHook.ccall<bool>(x, y);
}

// ---------------------------------------------------------------- diagnostics

using FindSmallestFn = SaZone*(__cdecl*)(const float* point, int naviOnly);
using LevelFromPosFn = int(__cdecl*)(const float* point);
using GetForMapFn = const char*(__stdcall*)(float x, float y);  // CPlaceName::GetForMap ignores `this`

const char* Translate(const char* key) {
    char zeroTerminated[9] = { 0 };
    memcpy(zeroTerminated, key, 8);
    return plugin::CallMethodAndReturnDynGlobal<const char*>(kTextGet, (void*)kTheText, zeroTerminated);
}

}  // namespace

namespace Zones {
    void Install() {
        int regionIndex = 0;
        for (const MapRegion& r : Settings::regions) {
            if (!r.zoneNamesFile.empty()) LoadNameFile(r, r.zoneNamesFile);
            for (const std::string& f : r.zoneFiles) LoadZoneFile(r, f);

            // One zone over the whole tile grid so the region has a name where no district applies.
            // SA picks the smallest zone containing the player, so districts still win inside it.
            if (!r.regionName.empty()) {
                char key[8];
                snprintf(key, sizeof(key), "MXREG%d", regionIndex);
                ZoneDef d{};
                MakeKey(r.zoneInfoLabel.c_str(), d.infoLabel);
                MakeKey(key, d.textLabel);
                d.x1 = r.TileWest(0);        d.x2 = r.TileWest(r.tilesX);
                d.y1 = r.TileNorth(r.tilesY); d.y2 = r.TileNorth(0);
                d.z1 = -3000.0f;             d.z2 = 3000.0f;
                d.level = 0;
                Defs().push_back(d);
                NameMap()[std::string(d.textLabel, strnlen(d.textLabel, 8))] = r.regionName;
            }
            regionIndex++;
        }
        if (Defs().empty()) return;

        // A loaded GXT (the map's own american.gxt) or a ZoneNames file already names its zones;
        // only invent a name when nobody provides one. With a region the importer produced that
        // is never -- zone_names.txt covers every label it writes -- so an invented name is worth
        // naming in the log: it is the visible end of a zone file and a name file that disagree.
        int invented = 0;
        for (const ZoneDef& d : Defs()) {
            std::string key(d.textLabel, strnlen(d.textLabel, 8));
            if (NameMap().count(key) || Gxt::Has(key.c_str())) continue;
            NameMap()[key] = DeriveName(key.c_str());
            if (++invented <= 10)
                Log::Write("zones: nothing names the key '%s'; showing \"%s\". Add it to the "
                           "region's ZoneNames file, or put the region's american.gxt on [World] Gxt",
                           key.c_str(), NameMap()[key].c_str());
        }
        if (invented > 10) Log::Write("zones: ... and %d more key(s) with no name of their own", invented - 10);
        Log::Debug("zones: registering %u fallback name(s)", (unsigned)NameMap().size());
        int named = 0;
        for (const auto& kv : NameMap())
            if (Gxt::AddString(kv.first.c_str(), kv.second.c_str(), "zone names")) named++;

        // Vanilla SA already fills 379 of the 380 navigation zones, so anything we add needs a
        // bigger array. The spare room absorbs a savegame written with more regions than are
        // configured right now.
        if (RelocateNaviArray(kVanillaNaviCapacity + (int)Defs().size() + 128))
            Log::Write("note: savegames written from now on carry these zones; loading one without "
                       "MapExpander would overrun the game's 380-zone array");
        else
            Log::Write("ERROR: the zone array stays at 380 entries, which vanilla SA already fills; "
                       "expect almost none of the region's zones to be created");

        Log::Debug("zones: hooking CTheZones::Update at 0x%X", (unsigned)kZonesUpdate);
        gUpdateHook = safetyhook::create_inline(reinterpret_cast<void*>(kZonesUpdate),
                                                reinterpret_cast<void*>(&Hooked_ZonesUpdate));
        if (!gUpdateHook) {
            Log::Write("could not hook CTheZones::Update; zones will not be created");
            return;
        }

        gRevealedHook = safetyhook::create_inline(reinterpret_cast<void*>(kZoneRevealed),
                                                  reinterpret_cast<void*>(&Hooked_ZoneRevealed));
        Log::Write("zones: %u to add, %d display name(s) registered, revealed-cell hook %s",
                   (unsigned)Defs().size(), named,
                   gRevealedHook ? "installed" : "FAILED (region names may not show on the pause map)");
    }

    void LogPlayerZone(float x, float y, float z) {
        const float point[3] = { x, y, z };
        int16_t total = NaviTotal();
        Log::Write("F8 zones: %d navigation zone(s) of %d, %d zone info(s) of %d, ZonesRevealed %d, "
                   "m_CurrLevel %d, level here %d",
                   (int)total, gCapacity, (int)InfoTotal(), kInfoCapacity,
                   *reinterpret_cast<const int*>(kZonesRevealed),
                   *reinterpret_cast<const int*>(kCurrLevel),
                   reinterpret_cast<LevelFromPosFn>(kLevelFromPos)(point));

        // The HUD (CPlaceName::Process) and the pause map (CPlaceName::GetForMap) both land here.
        SaZone* zone = reinterpret_cast<FindSmallestFn>(kFindSmallest)(point, 0);
        if (!zone) {
            Log::Write("F8 zones: FindSmallestZoneForPosition returned nothing");
            return;
        }
        long index = (long)(zone - gZones);
        char label[9] = { 0 };
        memcpy(label, zone->textLabel, 8);
        const char* ours = Gxt::Find(label);
        Log::Write("F8 zones: here -> #%ld text \"%.8s\" info \"%.8s\" (info #%d) type %d level %d "
                   "box x %d..%d y %d..%d z %d..%d, shows as \"%s\", our table: %s%s%s",
                   index, zone->textLabel, zone->infoLabel, (int)zone->infoIndex, (int)zone->type,
                   (int)zone->level, (int)zone->x1, (int)zone->x2, (int)zone->y1, (int)zone->y2,
                   (int)zone->z1, (int)zone->z2, Translate(zone->textLabel),
                   ours ? "\"" : "", ours ? ours : "no entry", ours ? "\"" : "");
        Log::Write("F8 zones: pause map hover here -> \"%s\", cell %s the 10x10 visited grid",
                   reinterpret_cast<GetForMapFn>(kGetForMap)(x, y),
                   (x < -kVanillaMapEdge || x > kVanillaMapEdge ||
                    y < -kVanillaMapEdge || y > kVanillaMapEdge) ? "outside (forced revealed)" : "inside");

        int present = 0, coverXY = 0;
        for (const ZoneDef& d : Defs()) {
            for (int i = 0; i < total; i++)
                if (memcmp(gZones[i].textLabel, d.textLabel, 8) == 0) { present++; break; }
            if (x >= std::min(d.x1, d.x2) && x <= std::max(d.x1, d.x2) &&
                y >= std::min(d.y1, d.y2) && y <= std::max(d.y1, d.y2)) coverXY++;
        }
        Log::Write("F8 zones: %d of our %u zone(s) in the array, %d cover this x/y",
                   present, (unsigned)Defs().size(), coverXY);
    }
}
