// Loads III/VC .zon files (and SA IPL "zone" sections) offset by a region's world origin, so SA
// shows that map's own area names on it.
//
// Two things stand in the way. CTheZones::NavigationZoneArray holds 380 zones and vanilla SA fills
// 379 of them (378 lines in info.zon plus SAN_AND from CTheZones::Init), so every added zone has to
// go into a bigger array -- the array is a fixed global, so all 62 code sites that name it are
// re-pointed at load. And the zone labels are GXT keys that only exist in the other game's
// american.gxt, so CText::Get is hooked to answer for the keys we registered.
#include <plugin.h>
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
#include "MemPatch.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kNaviArray = 0xBA3798;   // CTheZones::NavigationZoneArray[380]
constexpr uintptr_t kNaviTotal = 0xBA3794;   // CTheZones::TotalNumberOfNavigationZones (int16)
constexpr uintptr_t kInfoTotal = 0xBA1DE8;   // CTheZones::TotalNumberOfZoneInfos (int16)
constexpr uintptr_t kCreateZone = 0x5728A0;
constexpr uintptr_t kTextGet = 0x6A0050;     // CText::Get(const char*), __thiscall
constexpr uintptr_t kZonesUpdate = 0x572D10; // CTheZones::Update, once per frame from CGame::Process

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

struct NameEntry {
    char        key[8];
    const char* text;
};

std::vector<ZoneDef> gDefs;
std::map<std::string, std::string> gNameMap;  // node based: the strings we hand out never move
std::vector<NameEntry> gNames;                // sorted by key, searched from the CText::Get hook

SaZone* gZones = reinterpret_cast<SaZone*>(kNaviArray);
int gCapacity = kVanillaNaviCapacity;
bool gAudited = false;

// What the array looked like when we last filled it. Anything else (a new game, a savegame that
// restored a different set) means the zones have to be put back.
int gExpectedTotal = -1;
SaZone gExpectedLast{};

SafetyHookInline gTextGetHook;
SafetyHookInline gUpdateHook;

int16_t& NaviTotal() { return *reinterpret_cast<int16_t*>(kNaviTotal); }
int16_t& InfoTotal() { return *reinterpret_cast<int16_t*>(kInfoTotal); }

// ---------------------------------------------------------------- keys and names

// Zone labels are GXT keys. CTheZones::CreateZone copies at most 7 characters and upper-cases them,
// so that is what a zone can ever be looked up by.
void MakeKey(const char* in, char out[8]) {
    memset(out, 0, 8);
    for (int i = 0; i < 7 && in[i]; i++) out[i] = (char)toupper((unsigned char)in[i]);
}

void PadKey(const char* in, char out[8]) {
    int i = 0;
    for (; i < 8 && in[i]; i++) out[i] = in[i];
    for (; i < 8; i++) out[i] = 0;
}

const char* FindName(const char* key) {
    if (gNames.empty()) return nullptr;
    char padded[8];
    PadKey(key, padded);
    size_t lo = 0, hi = gNames.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int cmp = memcmp(padded, gNames[mid].key, 8);
        if (cmp == 0) return gNames[mid].text;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

std::string DeriveName(const char* key) {
    std::string out;
    bool wordStart = true;
    for (const char* p = key; *p; p++) {
        char c = *p;
        if (c == '_') { out.push_back(' '); wordStart = true; continue; }
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
        MakeKey(t[0].c_str(), d.infoLabel);
        MakeKey(t.size() >= 10 ? t[9].c_str() : t[0].c_str(), d.textLabel);
        d.x1 = strtof(t[2].c_str(), nullptr) + region.originX;
        d.y1 = strtof(t[3].c_str(), nullptr) + region.originY;
        d.z1 = strtof(t[4].c_str(), nullptr);
        d.x2 = strtof(t[5].c_str(), nullptr) + region.originX;
        d.y2 = strtof(t[6].c_str(), nullptr) + region.originY;
        d.z2 = strtof(t[7].c_str(), nullptr);
        d.level = atoi(t[8].c_str());
        if (d.level < 0 || d.level > 3) d.level = 0;

        // CreateZone truncates the corners to int16.
        float lo = std::min(d.x1, d.x2), hi = std::max(d.x1, d.x2);
        if (lo < -32000.0f || hi > 32000.0f) { malformed++; continue; }
        lo = std::min(d.y1, d.y2); hi = std::max(d.y1, d.y2);
        if (lo < -32000.0f || hi > 32000.0f) { malformed++; continue; }

        gDefs.push_back(d);
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
        gNameMap[std::string(key, strnlen(key, 8))] = text;
        count++;
    }
    Log::Write("[%s] %s: %d display name(s)", region.name.c_str(), path.c_str(), count);
}

// ---------------------------------------------------------------- CText::Get

const char* __fastcall Hooked_TextGet(void* self, void* /*edx*/, const char* key) {
    if (key && key[0] && key[0] != ' ') {
        if (const char* mine = FindName(key)) return mine;
    }
    // unsafe_ skips safetyhook's per-call mutex; this runs several times per frame and the hook
    // lives until the process ends, so there is nothing to guard against.
    return gTextGetHook.unsafe_thiscall<const char*>(self, key);
}

// The game's own GXT wins nothing here: our key would shadow it. Say so once, when the text is
// loaded, so a clash between a region's labels and SA's own is visible in the log.
void AuditKeys() {
    if (gAudited) return;
    gAudited = true;
    void* text = reinterpret_cast<void*>(0xC1B340);  // TheText
    const char* missing = gTextGetHook.thiscall<const char*>(text, "\x01ZZZZZZ");
    int clashes = 0;
    for (const NameEntry& e : gNames) {
        char key[9] = { 0 };
        memcpy(key, e.key, 8);
        if (gTextGetHook.thiscall<const char*>(text, key) != missing) {
            if (clashes < 8) Log::Write("zone name key '%s' also exists in the game's GXT; ours is used", key);
            clashes++;
        }
    }
    if (clashes) Log::Write("%d zone name key(s) shadow an entry in the game's GXT", clashes);
}

// ---------------------------------------------------------------- array relocation

bool RelocateNaviArray(int capacity) {
    for (const RefSite& r : kNaviRefs) {
        uint32_t current = (uint32_t)patch::GetUInt(r.address + r.dispOffset);
        if (current != r.oldValue) {
            Log::Write("zone array not relocated: 0x%X holds 0x%X, expected 0x%X (another mod moved it?)",
                       (unsigned)r.address, current, r.oldValue);
            return false;
        }
    }

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
    Log::Write("NavigationZoneArray 0x%X -> %p, capacity %d -> %d, %d/%d references re-pointed",
               (unsigned)kNaviArray, (void*)moved, kVanillaNaviCapacity, capacity,
               patched, (int)std::size(kNaviRefs));
    return patched == (int)std::size(kNaviRefs);
}

// ---------------------------------------------------------------- zone creation

bool IsOneOfOurs(const SaZone& z) {
    // Our zones sit outside the vanilla map; every zone SA loads itself is inside it. Together with
    // the label that is enough to recognise ours after a savegame restored them.
    bool insideVanillaMap = z.x1 >= -kVanillaMapEdge && z.x2 <= kVanillaMapEdge &&
                            z.y1 >= -kVanillaMapEdge && z.y2 <= kVanillaMapEdge;
    if (insideVanillaMap) return false;
    for (const ZoneDef& d : gDefs)
        if (memcmp(z.textLabel, d.textLabel, 8) == 0 && memcmp(z.infoLabel, d.infoLabel, 8) == 0)
            return true;
    return false;
}

void EnsureZones() {
    int16_t& total = NaviTotal();
    if (total == gExpectedTotal && total > 0 &&
        memcmp(&gZones[total - 1], &gExpectedLast, sizeof(SaZone)) == 0)
        return;

    AuditKeys();

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
    for (const ZoneDef& d : gDefs) {
        if (total >= gCapacity || InfoTotal() >= kInfoCapacity - 1) { dropped++; continue; }
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

void __cdecl Hooked_ZonesUpdate() {
    gUpdateHook.ccall<void>();
    EnsureZones();
}

}  // namespace

namespace Zones {
    void Install() {
        for (const MapRegion& r : Settings::regions) {
            if (r.zoneFiles.empty()) continue;
            if (!r.zoneNamesFile.empty()) LoadNameFile(r, r.zoneNamesFile);
            for (const std::string& f : r.zoneFiles) LoadZoneFile(r, f);
        }
        if (gDefs.empty()) return;

        for (const ZoneDef& d : gDefs) {
            std::string key(d.textLabel, strnlen(d.textLabel, 8));
            if (!gNameMap.count(key)) gNameMap[key] = DeriveName(key.c_str());
        }
        for (const auto& kv : gNameMap) {
            NameEntry e{};
            PadKey(kv.first.c_str(), e.key);
            e.text = kv.second.c_str();
            gNames.push_back(e);
        }
        std::sort(gNames.begin(), gNames.end(),
                  [](const NameEntry& a, const NameEntry& b) { return memcmp(a.key, b.key, 8) < 0; });

        gTextGetHook = safetyhook::create_inline(reinterpret_cast<void*>(kTextGet),
                                                 reinterpret_cast<void*>(&Hooked_TextGet));
        if (!gTextGetHook) {
            Log::Write("could not hook CText::Get; zone names would come out as the GXT error string");
            gDefs.clear();
            return;
        }

        // Vanilla SA already fills 379 of the 380 navigation zones, so anything we add needs a
        // bigger array. The spare room absorbs a savegame written with more regions than are
        // configured right now.
        if (RelocateNaviArray(kVanillaNaviCapacity + (int)gDefs.size() + 128))
            Log::Write("note: savegames written from now on carry these zones; loading one without "
                       "MapExpander would overrun the game's 380-zone array");
        else
            Log::Write("ERROR: the zone array stays at 380 entries, which vanilla SA already fills; "
                       "expect almost none of the region's zones to be created");

        gUpdateHook = safetyhook::create_inline(reinterpret_cast<void*>(kZonesUpdate),
                                                reinterpret_cast<void*>(&Hooked_ZonesUpdate));
        if (!gUpdateHook) {
            Log::Write("could not hook CTheZones::Update; zones will not be created");
            return;
        }

        Log::Write("zones: %u to add, %u display name(s) registered",
                   (unsigned)gDefs.size(), (unsigned)gNames.size());
    }
}
