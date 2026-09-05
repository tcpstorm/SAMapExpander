// Loads extra IPL files for a region with the region origin added to every coordinate, so an
// unmodified Vice City / Liberty City item placement file works where the region actually sits.
//
// SA parses cull/enex/tcyc/jump/pick/cars/grge/auzo exactly once, inside CGame::Initialise ->
// CFileLoader::LoadLevel; CGame::ReloadIPLs (0x53BED0) is a NOP and nothing re-reads an IPL later.
// Each line is rewritten and handed to the vanilla per-line loader so every parsing quirk stays
// vanilla; we only touch the coordinate fields.
#include <plugin.h>
#include <CFileLoader.h>
#include <safetyhook.hpp>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "IplLoader.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

enum SectionId {
    SEC_CULL, SEC_ENEX, SEC_TCYC, SEC_JUMP, SEC_PICK, SEC_CARS, SEC_GRGE, SEC_AUZO,
    SEC_COUNT,
    SEC_NONE = -1,
    SEC_IGNORED = -2,
};

constexpr unsigned MASK_ALL = (1u << SEC_COUNT) - 1;
// The only three pools a new game / load game wipes; everything else survives and must not be
// added twice. See CGame::ShutDownForRestart (CWorld::ClearForRestart -> CPickups::ReInit,
// CStuntJumpManager::ShutdownForRestart) and CGame::ReInitGameObjectVariables
// (CTheCarGenerators::Init).
constexpr unsigned MASK_RESTART = (1u << SEC_JUMP) | (1u << SEC_PICK) | (1u << SEC_CARS);

const char* const kSectionName[SEC_COUNT] = { "cull", "enex", "tcyc", "jump", "pick", "cars", "grge", "auzo" };

// Fixed-size storage the game uses for each section. Verified against gta-reversed / plugin-sdk;
// none of the Add* functions except CTheCarGenerators::CreateCarGenerator bounds-checks anything.
constexpr int MAX_ATTRIBUTE_ZONES = 1300;   // CCullZones::aAttributeZones       0xC81F50
constexpr int MAX_TUNNEL_ZONES    = 40;     // CCullZones::aTunnelAttributeZones 0xC81C80
constexpr int MAX_MIRROR_ZONES    = 72;     // CCullZones::aMirrorAttributeZones 0xC815C0
constexpr int MAX_TCYC_BOXES      = 20;     // CTimeCycle::m_aBoxes              0xB7C550
constexpr int MAX_GARAGES         = 50;     // CGarages::aGarages                0x96C048
constexpr int MAX_AUDIO_BOXES     = 158;    // CAudioZones::m_aBoxes             0xB6DCD0
constexpr int MAX_AUDIO_SPHERES   = 3;      // CAudioZones::m_aSpheres           0xB6EBA8

int&      NumAttributeZones       = *reinterpret_cast<int*>(0xC87AC8);
int&      NumTunnelAttributeZones = *reinterpret_cast<int*>(0xC87AC0);
int&      NumMirrorAttributeZones = *reinterpret_cast<int*>(0xC87AC4);
uint32_t& NumTimeCycleBoxes       = *reinterpret_cast<uint32_t*>(0xB7C480);
int&      NumGarages              = *reinterpret_cast<int*>(0x96C024);
uint32_t& NumAudioBoxes           = *reinterpret_cast<uint32_t*>(0xB6DCC4);
uint32_t& NumAudioSpheres         = *reinterpret_cast<uint32_t*>(0xB6DCC8);
uint32_t& NumOfCarGenerators      = *reinterpret_cast<uint32_t*>(0xC279D4);

constexpr uintptr_t ENEX_POOL     = 0x96A7D8;   // CEntryExitManager::mp_poolEntryExits (400 entries)
constexpr uintptr_t ENEX_QUADTREE = 0x96A7D0;   // CEntryExitManager::mp_QuadTree
constexpr uintptr_t JUMP_POOL     = 0xA9A888;   // CStuntJumpManager::mp_poolStuntJumps (256 entries)

// CAudioZones stores box corners as CompressedLargeVector (int16 * 1/8): a hard +/-4095.9 that
// PickupFix's scale patch does not cover, because those sites are not the pickup/car generator ones.
constexpr float AUZO_SCALE = 8.0f;

struct PoolHeader {
    void*          objects;
    unsigned char* byteMap;      // bit 7 = slot is empty
    int            size;
    int            firstFree;
};

int PoolFreeSlots(uintptr_t poolPointer) {
    auto* pool = *reinterpret_cast<PoolHeader**>(poolPointer);
    if (!pool || !pool->byteMap || pool->size <= 0) return -1;
    int free = 0;
    for (int i = 0; i < pool->size; i++)
        if (pool->byteMap[i] & 0x80) free++;
    return free;
}

// One accepted field layout for a section: how many whitespace-separated tokens it has and which
// of them are absolute X / Y / Z world coordinates. Everything else (sizes, angles, ranges, flags)
// is relative and must be left alone.
struct Variant {
    int           tokens;
    unsigned char nx, ny, nz;
    unsigned char x[5], y[5], z[5];
};

const Variant CULL_V[] = {
    { 14, 1, 1, 1, { 0 }, { 1 }, { 2 } },                                       // mirror zone: centre, then vectors + mirror plane
    { 11, 1, 1, 1, { 0 }, { 1 }, { 2 } },                                       // attribute / tunnel zone: centre, then vectors
};
const Variant ENEX_V[] = {
    { 18, 2, 2, 2, { 0, 7 }, { 1, 8 }, { 2, 9 } },                              // entrance xyz, ..., exit xyz
};
const Variant TCYC_V[] = {
    { 12, 2, 2, 2, { 0, 3 }, { 1, 4 }, { 2, 5 } },                              // box corner A, corner B
    { 11, 2, 2, 2, { 0, 3 }, { 1, 4 }, { 2, 5 } },
};
const Variant JUMP_V[] = {
    { 16, 5, 5, 5, { 0, 3, 6, 9, 12 }, { 1, 4, 7, 10, 13 }, { 2, 5, 8, 11, 14 } },   // start min/max, target min/max, camera
};
const Variant PICK_V[] = {
    { 4, 1, 1, 1, { 1 }, { 2 }, { 3 } },                                        // weapon id, xyz
};
const Variant CARS_V[] = {
    { 12, 1, 1, 1, { 0 }, { 1 }, { 2 } },                                       // xyz, angle, ...
};
const Variant GRGE_V[] = {
    { 11, 3, 3, 2, { 0, 3, 5 }, { 1, 4, 6 }, { 2, 7 } },                        // corner xyz, front point xy, opposite corner xyz
};
const Variant AUZO_V[] = {
    { 9, 2, 2, 2, { 3, 6 }, { 4, 7 }, { 5, 8 } },                               // name, id, flags, box min, box max
    { 7, 1, 1, 1, { 3 }, { 4 }, { 5 } },                                        // name, id, flags, centre, radius
};

struct SectionSpec {
    const Variant* variants;
    int            variantCount;
    void          (*load)(const char* line);
};

void LoadCarsLine(const char* line) { CFileLoader::LoadCarGenerator(line, 0); }

const SectionSpec kSpec[SEC_COUNT] = {
    { CULL_V, 2, &CFileLoader::LoadCullZone },
    { ENEX_V, 1, &CFileLoader::LoadEntryExit },
    { TCYC_V, 2, &CFileLoader::LoadTimeCyclesModifier },
    { JUMP_V, 1, &CFileLoader::LoadStuntJump },
    { PICK_V, 1, &CFileLoader::LoadPickup },
    { CARS_V, 1, &LoadCarsLine },
    { GRGE_V, 1, &CFileLoader::LoadGarage },
    { AUZO_V, 2, &CFileLoader::LoadAudioZone },
};

constexpr int kMaxComplaints = 5;   // per file and section, before falling back to the totals

struct Stats {
    int loaded[SEC_COUNT] = {};
    int malformed[SEC_COUNT] = {};
    int outOfRange[SEC_COUNT] = {};
    int full[SEC_COUNT] = {};
    int ignoredLines = 0;
};

// ---------------------------------------------------------------------------------------------

SectionId SectionFromLine(const std::string& line, bool& ignored) {
    static const char* const kIgnored[] = { "inst", "zone", "occl", "path", "mult" };
    for (int i = 0; i < SEC_COUNT; i++)
        if (line.compare(0, 4, kSectionName[i]) == 0) return (SectionId)i;
    for (const char* name : kIgnored)
        if (line.compare(0, 4, name) == 0) { ignored = true; return SEC_NONE; }
    return SEC_NONE;
}

// CFileLoader::LoadLine turns every control character and every comma into a space before the
// per-line loaders ever see the text, so do the same and then split on whitespace.
void Sanitise(std::string& line) {
    for (char& c : line)
        if ((unsigned char)c < (unsigned char)' ' || c == ',') c = ' ';
}

std::vector<std::string> Tokenise(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') i++;
        size_t start = i;
        while (i < line.size() && line[i] != ' ') i++;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

double Num(const std::vector<std::string>& t, size_t i) {
    return i < t.size() ? strtod(t[i].c_str(), nullptr) : 0.0;
}

void SetNum(std::vector<std::string>& t, size_t i, double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.5f", v);
    t[i] = buf;
}

std::string Join(const std::vector<std::string>& t) {
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (i) out += ' ';
        out += t[i];
    }
    return out;
}

// Room left for one more line of this section. Cull is special: the flags decide which of the
// three arrays the line lands in (CCullZones::AddCullZone strips the tunnel bits and only fills
// an attribute slot if anything is left).
bool HasRoom(SectionId section, const std::vector<std::string>& t) {
    switch (section) {
    case SEC_CULL: {
        if (t.size() == 14) return NumMirrorAttributeZones < MAX_MIRROR_ZONES;
        int flags = (int)strtol(t[9].c_str(), nullptr, 0);
        bool tunnel = (flags & 0x880) != 0;
        if (tunnel && NumTunnelAttributeZones >= MAX_TUNNEL_ZONES) return false;
        return (flags & ~0x880) == 0 || NumAttributeZones < MAX_ATTRIBUTE_ZONES;
    }
    case SEC_ENEX: { int free = PoolFreeSlots(ENEX_POOL); return free != 0; }
    case SEC_TCYC:  return NumTimeCycleBoxes < (uint32_t)MAX_TCYC_BOXES;
    case SEC_JUMP: { int free = PoolFreeSlots(JUMP_POOL); return free != 0; }
    case SEC_GRGE:  return NumGarages < MAX_GARAGES;
    case SEC_AUZO:
        return t.size() == 9 ? NumAudioBoxes < (uint32_t)MAX_AUDIO_BOXES
                             : NumAudioSpheres < (uint32_t)MAX_AUDIO_SPHERES;
    default:
        return true;   // pick and cars fail safely: they drop the entry and return -1
    }
}

// Sections whose storage is int16-compressed. Returns false (with a reason) when the rewritten
// line would wrap around.
bool WithinCompressedRange(SectionId section, const std::vector<std::string>& t, float& limit) {
    switch (section) {
    case SEC_PICK:
    case SEC_CARS: {
        bool patched = (section == SEC_PICK) ? Settings::world.fixPickupPositions
                                             : Settings::world.fixCarGeneratorPositions;
        float scale = patched ? Settings::world.compressedPositionScale : 8.0f;
        limit = 32767.0f / (scale > 0.0f ? scale : 8.0f);
        size_t first = (section == SEC_PICK) ? 1 : 0;
        for (size_t i = first; i < first + 3; i++)
            if (fabs(Num(t, i)) >= limit) return false;
        return true;
    }
    case SEC_AUZO: {
        limit = 32767.0f / AUZO_SCALE;
        size_t count = (t.size() == 9) ? 6 : 3;
        for (size_t i = 3; i < 3 + count; i++)
            if (fabs(Num(t, i)) >= limit) return false;
        return true;
    }
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------------------------
// The entry/exit spatial index is a quad tree built over CWorld's -3000..3000 box, and
// CQuadTreeNode::AddItem silently drops anything that intersects none of its sectors. Widening the
// root rectangle before the first enex is inserted is enough (child rectangles are derived from
// the parent's on every descent), so do it in front of the very first CFileLoader::LoadLevel.
float gEnexMinX = -3000.0f, gEnexMaxX = 3000.0f, gEnexMinY = -3000.0f, gEnexMaxY = 3000.0f;
bool gEnexBoundsDone = false;

void RegionBounds(float& minX, float& maxX, float& minY, float& maxY) {
    minX = -3000.0f; maxX = 3000.0f; minY = -3000.0f; maxY = 3000.0f;
    for (const MapRegion& r : Settings::regions) {
        if (r.iplFiles.empty()) continue;
        if (r.TileWest(0) - 500.0f < minX)          minX = r.TileWest(0) - 500.0f;
        if (r.TileWest(r.tilesX) + 500.0f > maxX)   maxX = r.TileWest(r.tilesX) + 500.0f;
        if (r.TileNorth(r.tilesY) - 500.0f < minY)  minY = r.TileNorth(r.tilesY) - 500.0f;
        if (r.TileNorth(0) + 500.0f > maxY)         maxY = r.TileNorth(0) + 500.0f;
    }
}

void ExpandEnexQuadTree() {
    if (gEnexBoundsDone) return;
    gEnexBoundsDone = true;

    float minX, maxX, minY, maxY;
    RegionBounds(minX, maxX, minY, maxY);
    if (minX >= -3000.0f && maxX <= 3000.0f && minY >= -3000.0f && maxY <= 3000.0f) return;

    float* rect = *reinterpret_cast<float**>(ENEX_QUADTREE);
    if (!rect) {
        Log::Write("ipl: entry/exit quad tree not built yet; enex outside +/-3000 will be dropped");
        return;
    }
    // CEntryExitManager::Init builds the tree over CRect(-3000, 3000, 3000, -3000): the game's
    // CRect is (left, bottom, right, top) with bottom holding the larger y for world rects.
    if (rect[0] != -3000.0f || rect[1] != 3000.0f || rect[2] != 3000.0f || rect[3] != -3000.0f) {
        Log::Write("ipl: entry/exit quad tree bounds are (%.1f, %.1f)-(%.1f, %.1f), not the vanilla "
                   "-3000..3000; left alone, enex outside them will be dropped",
                   rect[0], rect[3], rect[2], rect[1]);
        return;
    }
    rect[0] = minX; rect[1] = maxY; rect[2] = maxX; rect[3] = minY;
    gEnexMinX = minX; gEnexMinY = minY; gEnexMaxX = maxX; gEnexMaxY = maxY;
    Log::Write("ipl: entry/exit quad tree widened to (%.0f, %.0f)-(%.0f, %.0f)", minX, minY, maxX, maxY);
}

// ---------------------------------------------------------------------------------------------

void ProcessFile(const MapRegion& region, const std::string& path, unsigned mask) {
    std::ifstream in(path);
    if (!in) {
        Log::Write("[%s] ipl '%s' could not be opened", region.name.c_str(), path.c_str());
        return;
    }

    Stats stats;
    SectionId section = SEC_NONE;
    std::string raw;
    int lineNo = 0;

    while (std::getline(in, raw)) {
        lineNo++;
        Sanitise(raw);
        size_t begin = raw.find_first_not_of(' ');
        if (begin == std::string::npos) continue;
        std::string line = raw.substr(begin);
        if (line[0] == '#') continue;

        if (section == SEC_NONE) {
            bool ignored = false;
            if (line.size() >= 4) section = SectionFromLine(line, ignored);
            if (section == SEC_NONE && ignored) section = SEC_IGNORED;
            continue;
        }
        if (line.compare(0, 3, "end") == 0) { section = SEC_NONE; continue; }
        if (section == SEC_IGNORED) { stats.ignoredLines++; continue; }
        if (!(mask & (1u << section))) continue;

        std::vector<std::string> tokens = Tokenise(line);
        const SectionSpec& spec = kSpec[section];
        const Variant* variant = nullptr;
        for (int i = 0; i < spec.variantCount; i++)
            if ((int)tokens.size() == spec.variants[i].tokens) variant = &spec.variants[i];
        if (!variant) {
            if (++stats.malformed[section] <= kMaxComplaints)
                Log::Write("  %s:%d [%s] %u fields, not a recognised layout; skipped",
                           path.c_str(), lineNo, kSectionName[section], (unsigned)tokens.size());
            continue;
        }

        for (int i = 0; i < variant->nx; i++) SetNum(tokens, variant->x[i], Num(tokens, variant->x[i]) + region.originX);
        for (int i = 0; i < variant->ny; i++) SetNum(tokens, variant->y[i], Num(tokens, variant->y[i]) + region.originY);
        for (int i = 0; i < variant->nz; i++) SetNum(tokens, variant->z[i], Num(tokens, variant->z[i]) + region.originZ);

        // A mirror cull zone also carries the mirror plane's own coordinate along its normal axis.
        if (section == SEC_CULL && tokens.size() == 14) {
            double nx = Num(tokens, 10), ny = Num(tokens, 11);
            if (fabs(nx) > 0.5)      SetNum(tokens, 13, Num(tokens, 13) + region.originX);
            else if (fabs(ny) > 0.5) SetNum(tokens, 13, Num(tokens, 13) + region.originY);
            else                     SetNum(tokens, 13, Num(tokens, 13) + region.originZ);
        }

        float limit = 0.0f;
        if (!WithinCompressedRange(section, tokens, limit)) {
            if (++stats.outOfRange[section] <= kMaxComplaints)
                Log::Write("  %s:%d [%s] outside the +/-%.0f the game can store for this section; skipped",
                           path.c_str(), lineNo, kSectionName[section], limit);
            continue;
        }
        if (section == SEC_ENEX) {
            double ex = Num(tokens, 0), ey = Num(tokens, 1);
            if (ex < gEnexMinX || ex > gEnexMaxX || ey < gEnexMinY || ey > gEnexMaxY) {
                if (++stats.outOfRange[section] <= kMaxComplaints)
                    Log::Write("  %s:%d [enex] entrance (%.1f, %.1f) is outside the entry/exit quad tree; skipped",
                               path.c_str(), lineNo, ex, ey);
                continue;
            }
        }
        if (!HasRoom(section, tokens)) {
            stats.full[section]++;
            continue;
        }

        std::string rewritten = Join(tokens);
        spec.load(rewritten.c_str());
        stats.loaded[section]++;
    }

    std::string summary;
    char buf[64];
    for (int i = 0; i < SEC_COUNT; i++) {
        if (!stats.loaded[i] && !stats.malformed[i] && !stats.outOfRange[i] && !stats.full[i]) continue;
        snprintf(buf, sizeof(buf), "%s%s %d", summary.empty() ? "" : ", ", kSectionName[i], stats.loaded[i]);
        summary += buf;
    }
    if (summary.empty()) summary = "nothing";
    Log::Write("[%s] %s: %s (%d line(s) in unsupported sections)",
               region.name.c_str(), path.c_str(), summary.c_str(), stats.ignoredLines);

    for (int i = 0; i < SEC_COUNT; i++) {
        if (stats.full[i])
            Log::Write("[%s] %s: %s is full, %d line(s) dropped - the game's array cannot hold more",
                       region.name.c_str(), path.c_str(), kSectionName[i], stats.full[i]);
        if (stats.malformed[i] || stats.outOfRange[i])
            Log::Write("[%s] %s: %s skipped %d malformed and %d out-of-range line(s)",
                       region.name.c_str(), path.c_str(), kSectionName[i], stats.malformed[i], stats.outOfRange[i]);
    }
}

bool gPermanentLoaded = false;

void LoadAll(unsigned mask) {
    bool anyFile = false;
    for (const MapRegion& region : Settings::regions) {
        for (const std::string& file : region.iplFiles) {
            ProcessFile(region, file, mask);
            anyFile = true;
        }
    }
    // AddOne only back-links a pair whose partner already existed, so the first half of every pair
    // still needs the vanilla post pass that CGame::Init2 ran before we added anything.
    if (anyFile && (mask & (1u << SEC_ENEX)))
        Call<0x43F0A0>();   // CEntryExitManager::PostEntryExitsCreation

    Log::Debug("ipl pools now: cull %d/%d (tunnel %d/%d, mirror %d/%d), tcyc %u/%d, grge %d/%d, "
               "auzo %u/%d boxes %u/%d spheres, cars %u, enex %d free, jump %d free",
               NumAttributeZones, MAX_ATTRIBUTE_ZONES, NumTunnelAttributeZones, MAX_TUNNEL_ZONES,
               NumMirrorAttributeZones, MAX_MIRROR_ZONES, NumTimeCycleBoxes, MAX_TCYC_BOXES,
               NumGarages, MAX_GARAGES, NumAudioBoxes, MAX_AUDIO_BOXES, NumAudioSpheres,
               MAX_AUDIO_SPHERES, NumOfCarGenerators, PoolFreeSlots(ENEX_POOL), PoolFreeSlots(JUMP_POOL));
}

// Install() runs during the DLL's static initialisation, before this file's globals are
// constructed; a SafetyHookInline must exist before it is assigned, hence function-local statics.
SafetyHookInline& LoadLevelOriginal() { static SafetyHookInline h; return h; }
SafetyHookInline& Init3Original() { static SafetyHookInline h; return h; }
SafetyHookInline& RestartOriginal() { static SafetyHookInline h; return h; }

void __cdecl LoadLevelHook(const char* datFile) {
    ExpandEnexQuadTree();
    LoadLevelOriginal().ccall<void>(datFile);
}

bool __cdecl Init3Hook(const char* datFile) {
    bool result = Init3Original().ccall<bool>(datFile);
    Log::Debug("ipl: loading region IPLs after CGame::Init3");
    LoadAll(gPermanentLoaded ? MASK_RESTART : MASK_ALL);
    gPermanentLoaded = true;
    return result;
}

void __cdecl InitialiseWhenRestartingHook() {
    RestartOriginal().ccall<void>();
    if (!gPermanentLoaded) return;   // no game has been started yet; nothing was wiped
    Log::Debug("ipl: re-adding jump/pick/cars after CGame::InitialiseWhenRestarting");
    LoadAll(MASK_RESTART);
}

}   // namespace

namespace IplLoader {
    void Install() {
        bool any = false;
        for (const MapRegion& r : Settings::regions) any |= !r.iplFiles.empty();
        if (!any) return;

        LoadLevelOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x5B9030), reinterpret_cast<void*>(&LoadLevelHook));
        Init3Original() = safetyhook::create_inline(reinterpret_cast<void*>(0x5BA400), reinterpret_cast<void*>(&Init3Hook));
        RestartOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x53C680), reinterpret_cast<void*>(&InitialiseWhenRestartingHook));

        if (LoadLevelOriginal() && Init3Original() && RestartOriginal())
            Log::Debug("ipl loader installed");
        else
            Log::Write("ipl loader: hooks failed (LoadLevel %d, Init3 %d, InitialiseWhenRestarting %d); region IPLs may not load",
                       (int)(bool)LoadLevelOriginal(), (int)(bool)Init3Original(), (int)(bool)RestartOriginal());
    }
}
