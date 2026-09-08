// Die in Vice City and you wake in Vice City. Get arrested in Liberty City and you come out of a
// Liberty City cell.
//
//
// WHAT SAN ANDREAS DOES, AND WHY IT IS WRONG HERE
//
// Death and arrest both go through CGameLogic::Update (0x442AD0), which asks CRestart where to put
// the player back and hands the answer to CGameLogic::RestorePlayerStuffDuringResurrection
// (0x442060). There are exactly three such calls in the whole executable, confirmed by a full
// cross-reference of both functions:
//
//     0x442F70  CGameLogic::Update+0x4A0  CRestart::FindClosestHospitalRestartPoint (0x460850)
//     0x44331B  CGameLogic::Update+0x84B  CRestart::FindClosestPoliceRestartPoint   (0x460A50)
//     0x4435C6  CGameLogic::Update+0xAF6  the same, the second arrest path
//
// so this is death and arrest and nothing else. Both functions are plain __cdecl and read
//
//     void FindClosest...RestartPoint(CVector where, CVector* outPosition, float* outHeading)
//
// - the vector by value, twelve bytes of stack, the two results by pointer.
//
// What they search is San Andreas' own restart arrays, and every point in them was registered from
// San Andreas' own main.scm, so "closest" from Vice City is by definition a San Andreas one: eight
// hospitals and seven police stations, none nearer than the mainland. There is no distance limit -
// the search starts from a best-so-far of 9999999.0 for the hospital and 99999.99 for the police -
// and no fallback, so from an imported city the answer is always All Saints or a Los Santos cell,
// thousands of units away and across the sea.
//
// The arrays themselves, read off the two functions:
//
//                      hospital    police
//     count (uint16)   0xA4326C    0xA43268
//     positions        0xA43318    0xA43390    CVector[10]
//     headings         0xA432E8    0xA43298    float[10]
//     town id          0xA432C0    0xA43270    int[10]
//
//
// WHY NOTHING OF OURS GOES IN THEM
//
// Because CRestart::Save (0x5D3620) copies those counts and then that many points straight into a
// savegame block, and a San Andreas savegame has no framing between blocks at all -
// CGenericGameStorage::LoadDataFromWorkBuffer (0x5D1300) is a plain sequential byte reader, so a
// block that changes size silently shifts every block behind it. Putting a Vice City hospital into
// that array would write Vice City coordinates into a vanilla-shaped save and hand a game without
// this plugin two hospitals in the middle of the ocean. docs/save-compatibility.md is explicit that
// imported content goes above San Andreas' own window and never inside it, and restart points have
// no window above: there are ten slots and the save copies every one in use.
//
// So the arrays are never touched. The two search functions are hooked instead and answered from
// the region's own table when the player is inside a region, which leaves the savegame byte for
// byte what vanilla San Andreas writes. It is also the smaller change: no second hook on Save and
// Load to take our points back out again, and no ordering to get wrong.
//
//
// WHERE THE POINTS COME FROM
//
// The region's own restarts.dat, written by tools/import_city.py out of the donor game's main.scm -
// both games register their hospitals and police stations from the script with the same opcodes San
// Andreas uses (0x016C, 0x016D), and this plugin does not run other games' scripts. Three hospital
// and three police points for GTA III, one of each per island; four and four for Vice City. The
// coordinates are the region's own and the origin is added here.
//
// A z of -100 or below is the script's "put it on the ground here" sentinel rather than a height:
// San Andreas' own handler for 0x016C implements it at 0x47C869 by replacing z with
// CWorld::FindGroundZForCoord(x, y), and GTA III uses it for the Portland police station. It is
// resolved here rather than at import time, because the answer is in the region's collision and
// only the running game has that. CWorld::FindGroundZFor3DCoord (0x5696C0) is used rather than
// FindGroundZForCoord, for the reason Teleport.cpp gives: the simpler one returns a flat 20.0 when
// it finds nothing, which is indistinguishable from a real answer.
//
//
// WHAT HAPPENS WHEN THERE IS NO REGION POINT
//
// Three cases, and they are deliberately different.
//
//   * Outside every region - the whole of San Andreas - nothing is answered at all: the call goes
//     straight through to the game's own function, which does what it always did. This is the case
//     that must not change and the hook is a plain forward for it.
//   * Inside a region that has no point of the kind asked for, but has points of the other kind,
//     the other kind is used - an arrest in a city with hospitals but no police station wakes you
//     at a hospital in that city. It is the wrong building, but it is the right city, and the
//     alternative is the exact bug this exists to fix. Said once in the log so it can be corrected
//     by adding a line to restarts.dat.
//   * Inside a region with no restarts.dat at all, the call goes through to San Andreas, which is
//     what that region did before this existed. Nothing is invented.
//
// The script's own overrides win over all of it. San Andreas' script can name the next restart
// point outright (the flag at 0xA43264, cleared by CRestart::CancelOverrideRestart at 0x460800),
// substitute the position the search runs from (0xA43248), or declare a radius around a point
// inside which every restart is that point (0xA43258 for hospitals, 0xA43250 for police, both
// compared against 0.0 at the head of each function). Any of those active and the call is handed
// to the game untouched, because a mission that has said where the player belongs means it.
//
//
// THE TOWN ID
//
// San Andreas' points carry one and a region's cannot. It gates a point behind an island being
// unlocked - the search skips any point whose town is above CStats::GetStatValue(0xB5), the
// comparison at 0x460973 - and an imported city is open from the first frame, so there is no
// progression for it to describe. Since a region point never enters the array, nothing ever reads
// one: the field is not stored and not written to restarts.dat. The donors' own island gating lives
// in their scripts rather than in these instructions, and all of their points are taken.
//
//
// THE METRIC
//
// Straight 3D distance, which is what the game measures. The game also multiplies a candidate's
// distance by 6.0 (the constant at 0x858B44) when it is not in the same CTheZones zone as the
// player, to prefer a restart point in the district you died in. That is not carried across: every
// zone a region creates shares one CZoneInfo slot (see ZoneInfoLabel in MapExpander.ini), so the
// comparison cannot mean for a region what it means for San Andreas, and a city's four hospitals
// are close enough together that plain distance picks the same one anyway.
#include <plugin.h>
#include <CWorld.h>
#include <CPlayerPed.h>
#include <safetyhook.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "RegionRestarts.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

constexpr uintptr_t kFindHospital = 0x460850;
constexpr uintptr_t kFindPolice = 0x460A50;

// The first sixteen bytes of both functions, which in San Andreas 1.0 US are identical:
//   mov al, [0xA43264] / sub esp, 8 / push ebx / or ebx, -1 / test al, al / je +0x36
// Identical bytes are no use for telling the two apart, so each is also checked at +0x73, where the
// hospital one loads its own override radius (0xA43258) and the police one loads 0xA43250. Both
// have to match or nothing at all is hooked.
const uint8_t kPrologue[] = { 0xA0, 0x64, 0x32, 0xA4, 0x00, 0x83, 0xEC, 0x08,
                              0x53, 0x83, 0xCB, 0xFF, 0x84, 0xC0, 0x74, 0x36 };
constexpr uintptr_t kRadiusLoadOffset = 0x73;
const uint8_t kHospitalRadiusLoad[] = { 0xD9, 0x05, 0x58, 0x32, 0xA4, 0x00 };   // fld [0xA43258]
const uint8_t kPoliceRadiusLoad[]   = { 0xD9, 0x05, 0x50, 0x32, 0xA4, 0x00 };   // fld [0xA43250]

// The script's overrides, all read and never written. See the note above.
const uint8_t* const kOverrideRestart = reinterpret_cast<const uint8_t*>(0xA43264);
const uint8_t* const kOverrideSearchFrom = reinterpret_cast<const uint8_t*>(0xA43248);
const float* const kHospitalAreaRadius = reinterpret_cast<const float*>(0xA43258);
const float* const kPoliceAreaRadius = reinterpret_cast<const float*>(0xA43250);

// A twelve-byte plain struct, not plugin-sdk's CVector, and the reason is the one Teleport.cpp
// spells out at length: gta_sa.exe was built with MSVC, which passes a by-value vector as three
// floats on the stack, while clang building for i686 passes any class with a user-declared copy
// constructor - which CVector has - by invisible reference instead. Write CVector here and the
// twelve bytes the game pushed would be read as a pointer and two floats of nothing. This struct is
// trivially copyable and is passed exactly the way the game passes it.
struct RawVector { float x, y, z; };
typedef void(__cdecl* FindRestartFn)(RawVector where, RawVector* outPosition, float* outHeading);

// Below this a mined z means "find the ground here", not a height - the same constant and the same
// convention as San Andreas' own script handler at 0x47C869 and as the fall floor at 0x859014.
constexpr float kGroundSentinelZ = -100.0f;
// Where the downward line starts when a point's height has to be found, measured off the region's
// own floor. Same height San Andreas' FindGroundZForCoord casts from.
constexpr float kProbeHeight = 1000.0f;
// And where the player is put if the region's collision answers nothing at all there, so a point
// with no ground under it is still in the right city rather than in Los Santos.
constexpr float kNoGroundHeight = 20.0f;

// How far outside its tile grid a region still counts as its ground. The same margin FallRecovery
// uses, and for the same reason: dying just off the edge of an imported map is exactly the case
// this exists for.
constexpr float kFootprintMargin = 300.0f;

enum Kind { kHospital = 0, kPolice = 1, kKinds = 2 };
const char* const kKindName[kKinds] = { "hospital", "police" };

struct RestartPoint {
    float x, y, z;      // the region's own coordinates; the origin is added when one is answered
    float heading;      // degrees, as both games' scripts write them
    std::string what;   // the zone the importer found it in, for the log
};

struct RegionState {
    const MapRegion* region = nullptr;
    std::vector<RestartPoint> points[kKinds];
    bool saidBorrowed[kKinds] = { false, false };   // "no police points, using a hospital" - once
    bool saidEmpty[kKinds] = { false, false };      // "nothing of either kind" - once
};

std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }

SafetyHookInline& HospitalHook() { static SafetyHookInline h; return h; }
SafetyHookInline& PoliceHook() { static SafetyHookInline h; return h; }

bool gInstalled = false;
int gAnswered[kKinds] = { 0, 0 };

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

// kind, x, y, z, heading - see MapExpander.ini for the column meanings.
bool ReadRestartFile(RegionState& rs) {
    FILE* f = fopen(rs.region->restartFile.c_str(), "rb");
    if (!f) {
        Log::Write("[%s] restarts: %s cannot be opened", rs.region->name.c_str(),
                   rs.region->restartFile.c_str());
        return false;
    }
    std::string text;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);

    int malformed = 0;
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
        if (line.empty() || line[0] == ';') continue;   // ';' comments out a point, as in the ini
        std::vector<std::string> t = Tokens(line);
        if (t.size() != 5) { malformed++; continue; }

        int kind;
        if (_stricmp(t[0].c_str(), "hospital") == 0) kind = kHospital;
        else if (_stricmp(t[0].c_str(), "police") == 0) kind = kPolice;
        else { malformed++; continue; }

        RestartPoint p;
        p.x = (float)atof(t[1].c_str());
        p.y = (float)atof(t[2].c_str());
        p.z = (float)atof(t[3].c_str());
        p.heading = (float)atof(t[4].c_str());
        p.what = comment;
        rs.points[kind].push_back(p);
    }

    if (malformed)
        Log::Write("[%s] restarts: %d malformed line(s); expected \"kind, x, y, z, heading\"",
                   rs.region->name.c_str(), malformed);
    return !rs.points[kHospital].empty() || !rs.points[kPolice].empty();
}

RegionState* RegionAt(float x, float y) {
    for (RegionState& rs : Regions()) {
        const MapRegion& r = *rs.region;
        if (r.tilesX <= 0 || r.tilesY <= 0) continue;
        if (x < r.TileWest(0) - kFootprintMargin || x > r.TileWest(r.tilesX) + kFootprintMargin) continue;
        if (y < r.TileNorth(r.tilesY) - kFootprintMargin || y > r.TileNorth(0) + kFootprintMargin) continue;
        return &rs;
    }
    return nullptr;
}

// The closest point of `kind` in `rs`, in San Andreas world coordinates, with its height resolved.
// False when the region has none of that kind.
bool ClosestOfKind(const RegionState& rs, int kind, const RawVector& where,
                   RawVector& outPosition, float& outHeading, const RestartPoint** which) {
    const std::vector<RestartPoint>& list = rs.points[kind];
    if (list.empty()) return false;

    const MapRegion& r = *rs.region;
    const RestartPoint* best = nullptr;
    float bestDistance = 0.0f;
    for (const RestartPoint& p : list) {
        float dx = where.x - (p.x + r.originX);
        float dy = where.y - (p.y + r.originY);
        // A point waiting for its height is compared in two dimensions only, because the third is
        // not known yet and would otherwise decide the answer with a made-up number.
        float dz = p.z <= kGroundSentinelZ ? 0.0f : where.z - (p.z + r.originZ);
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!best || d < bestDistance) { best = &p; bestDistance = d; }
    }

    outPosition.x = best->x + r.originX;
    outPosition.y = best->y + r.originY;
    outHeading = best->heading;
    if (best->z > kGroundSentinelZ) {
        outPosition.z = best->z + r.originZ;
    } else {
        // The script's "put it on the ground here". San Andreas resolves it with
        // CWorld::FindGroundZForCoord; we cast the same line from the region's own floor and can
        // tell a miss from a real answer, which the game's simpler call cannot.
        bool found = false;
        float g = CWorld::FindGroundZFor3DCoord(outPosition.x, outPosition.y,
                                                r.originZ + kProbeHeight, &found, nullptr);
        outPosition.z = found ? g : r.originZ + kNoGroundHeight;
        if (!found)
            Log::Write("[%s] restarts: the %s point at (%.1f, %.1f) asks for the ground and the "
                       "region's collision has none there; using z %.1f", r.name.c_str(),
                       kKindName[kind], best->x, best->y, outPosition.z);
    }
    if (which) *which = best;
    return true;
}

// True when the script has said where the player belongs and we must not argue. Read at the same
// three places the game reads them, and never written - the game's own function clears the flags
// it consumes, and it only gets to do that if the call reaches it.
bool ScriptOwnsThis(int kind) {
    if (*kOverrideRestart) return true;
    if (*kOverrideSearchFrom) return true;
    const float radius = kind == kHospital ? *kHospitalAreaRadius : *kPoliceAreaRadius;
    return radius > 0.0f;
}

// The whole of it: answer from the region, or say no and let the game have the call.
bool Answer(int kind, const RawVector& where, RawVector* outPosition, float* outHeading,
            bool quiet = false) {
    if (!outPosition || !outHeading) return false;
    if (ScriptOwnsThis(kind)) return false;

    RegionState* rs = RegionAt(where.x, where.y);
    if (!rs) return false;   // San Andreas' own map: its answer is the right one and is untouched

    const RestartPoint* point = nullptr;
    int used = kind;
    if (!ClosestOfKind(*rs, kind, where, *outPosition, *outHeading, &point)) {
        // No point of this kind. The other kind is the wrong building in the right city, which
        // beats the right building three thousand units away across the sea.
        used = kind == kHospital ? kPolice : kHospital;
        if (!ClosestOfKind(*rs, used, where, *outPosition, *outHeading, &point)) {
            if (!quiet && !rs->saidEmpty[kind]) {
                rs->saidEmpty[kind] = true;
                Log::Write("[%s] restarts: no %s point and none of the other kind either; San "
                           "Andreas answers this one, which means waking up on the mainland",
                           rs->region->name.c_str(), kKindName[kind]);
            }
            return false;
        }
        if (!quiet && !rs->saidBorrowed[kind]) {
            rs->saidBorrowed[kind] = true;
            Log::Write("[%s] restarts: this region has no %s point, so its %s point is used "
                       "instead - the wrong building, but the right city. Add a \"%s\" line to "
                       "restarts.dat to fix it properly.",
                       rs->region->name.c_str(), kKindName[kind], kKindName[used], kKindName[kind]);
        }
    }

    if (!quiet) {
        gAnswered[kind]++;
        Log::Write("[%s] restarts: %s at (%.1f, %.1f, %.1f) answered with the region's own %s at "
                   "(%.1f, %.1f, %.1f) heading %.0f%s%s",
                   rs->region->name.c_str(),
                   kind == kHospital ? "death" : "arrest", where.x, where.y, where.z,
                   kKindName[used], outPosition->x, outPosition->y, outPosition->z, *outHeading,
                   point->what.empty() ? "" : "  # ", point->what.c_str());
    }
    return true;
}

void __cdecl HookedFindHospital(RawVector where, RawVector* outPosition, float* outHeading) {
    if (Answer(kHospital, where, outPosition, outHeading)) return;
    HospitalHook().ccall<void>(where, outPosition, outHeading);
}

void __cdecl HookedFindPolice(RawVector where, RawVector* outPosition, float* outHeading) {
    if (Answer(kPolice, where, outPosition, outHeading)) return;
    PoliceHook().ccall<void>(where, outPosition, outHeading);
}

bool BytesMatch(uintptr_t at, const uint8_t* expected, size_t length) {
    return memcmp(reinterpret_cast<const void*>(at), expected, length) == 0;
}

bool FunctionIsVanilla(uintptr_t at, const uint8_t* radiusLoad) {
    return BytesMatch(at, kPrologue, sizeof(kPrologue)) &&
           BytesMatch(at + kRadiusLoadOffset, radiusLoad, sizeof(kHospitalRadiusLoad));
}

}   // namespace

namespace RegionRestarts {

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.restartFile.empty();
    if (!any || !Settings::world.regionRestarts) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region restart points are off, the server owns where a dead or "
                   "arrested player comes back");
        return;
    }

    for (const MapRegion& r : Settings::regions) {
        if (r.restartFile.empty()) continue;
        RegionState rs;
        rs.region = &r;
        if (ReadRestartFile(rs)) Regions().push_back(rs);
    }
    if (Regions().empty()) return;

    if (!FunctionIsVanilla(kFindHospital, kHospitalRadiusLoad) ||
        !FunctionIsVanilla(kFindPolice, kPoliceRadiusLoad)) {
        Log::Write("restarts: CRestart::FindClosestHospitalRestartPoint/FindClosestPoliceRestartPoint "
                   "do not start with the bytes San Andreas 1.0 US ships, so something else has "
                   "them; nothing patched, and dying in an imported city will still wake you in "
                   "San Andreas");
        Regions().clear();
        return;
    }

    HospitalHook() = safetyhook::create_inline(reinterpret_cast<void*>(kFindHospital),
                                               reinterpret_cast<void*>(&HookedFindHospital));
    PoliceHook() = safetyhook::create_inline(reinterpret_cast<void*>(kFindPolice),
                                             reinterpret_cast<void*>(&HookedFindPolice));
    if (!HospitalHook() && !PoliceHook()) {
        Log::Write("restarts: neither restart-point search could be hooked; dying or being arrested "
                   "in an imported city will still wake you in San Andreas");
        Regions().clear();
        return;
    }
    if (!HospitalHook())
        Log::Write("restarts: the hospital search could not be hooked; arrest is answered from the "
                   "region but death is still answered by San Andreas");
    if (!PoliceHook())
        Log::Write("restarts: the police search could not be hooked; death is answered from the "
                   "region but arrest is still answered by San Andreas");

    gInstalled = true;
    for (const RegionState& rs : Regions())
        Log::Write("[%s] restarts: %u hospital and %u police point(s) of its own; San Andreas' own "
                   "arrays are not touched and the savegame is unchanged", rs.region->name.c_str(),
                   (unsigned)rs.points[kHospital].size(), (unsigned)rs.points[kPolice].size());
}

// What F8 says: where the player would wake if they died, and if they were arrested, from exactly
// where they stand - the same call the game will make, answered without anything being written.
void Describe(std::string& out) {
    if (!gInstalled) {
        out = "not installed";
        return;
    }
    CPlayerPed* player = FindPlayerPed();
    if (!player) {
        out = "installed; no player to ask about";
        return;
    }
    CVector pos = player->GetPosition();
    RawVector where = { pos.x, pos.y, pos.z };
    const RegionState* rs = RegionAt(where.x, where.y);
    char buffer[512];
    if (!rs) {
        snprintf(buffer, sizeof(buffer),
                 "outside every region - San Andreas answers death and arrest here, as it always "
                 "did (%d death(s) and %d arrest(s) answered from a region so far)",
                 gAnswered[kHospital], gAnswered[kPolice]);
        out = buffer;
        return;
    }

    // The same call the game would make from exactly here, run quietly: nothing is written and
    // nothing is logged, so pressing F8 does not disturb the counters or the once-only warnings.
    RawVector place[kKinds] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    float heading[kKinds] = { 0.0f, 0.0f };
    char said[kKinds][160];
    for (int kind = 0; kind < kKinds; kind++) {
        if (Answer(kind, where, &place[kind], &heading[kind], true))
            snprintf(said[kind], sizeof(said[kind]), "(%.1f, %.1f, %.1f) heading %.0f",
                     place[kind].x, place[kind].y, place[kind].z, heading[kind]);
        else
            snprintf(said[kind], sizeof(said[kind]),
                     "San Andreas (the script owns the next restart, or this region has no points)");
    }
    snprintf(buffer, sizeof(buffer), "[%s] die here -> %s; arrested here -> %s",
             rs->region->name.c_str(), said[kHospital], said[kPolice]);
    out = buffer;
}

}   // namespace RegionRestarts
