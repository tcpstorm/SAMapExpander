// Loads a region's own map assets - IMG archives, IDE model definitions, IPL `inst` placements and
// collision - so a custom city exists in single player instead of having to be placed by a server.
//
// The ordering is dictated by CFileLoader::LoadLevel (0x5B9030), which walks gta.dat top to bottom:
// IMG lines go to CStreaming::AddImageToList, IDE lines to CFileLoader::LoadObjectTypes, and on the
// FIRST IPL line it calls CStreaming::Init2 (0x5B8AD0) -> CStreaming::LoadCdDirectory, which scans
// every registered archive's directory and binds each entry to a model info / TXD / col slot BY NAME
// (0x5B6170; an entry whose name matches nothing is dropped). So a region's archives and its model
// infos must both exist before Init2 runs, and the model infos must come after SA's own IDEs so
// `txdp` parents resolve - exactly the window a pre-hook on Init2 sits in. Instances then go in a
// post-hook on LoadLevel, after SA's own IPLs.
//
// Rather than reimplement the loaders, this rewrites the region's IDE and IPL files into
// <game>\MapExpander\cache with model ids remapped into a free range and inst positions offset by
// the region origin, then points the vanilla CFileLoader::LoadObjectTypes / LoadScene at the result.
// LoadScene is what resolves per-file LOD indices, sets up big buildings and calls CWorld::Add (via
// LinkLods, 0x5B51E0), and all of that stays vanilla. The rewritten files are also there to read
// when a region misbehaves.
#include <plugin.h>
#include <safetyhook.hpp>
#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "CityLoader.h"
#include "ModelLimits.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

typedef unsigned(__cdecl* AddImageToListFn)(const char*, bool);
typedef void(__cdecl* LoadFileFn)(const char*);
typedef void(__cdecl* LoadCollisionFileFn)(const char*, unsigned char);

const AddImageToListFn CallAddImageToList = reinterpret_cast<AddImageToListFn>(0x407610);
const LoadFileFn CallLoadObjectTypes = reinterpret_cast<LoadFileFn>(0x5B8400);
const LoadFileFn CallLoadScene = reinterpret_cast<LoadFileFn>(0x5B8700);
const LoadCollisionFileFn CallLoadCollisionFile = reinterpret_cast<LoadCollisionFileFn>(0x5B4E60);

// CStreaming::ms_files: 8 x tStreamingFileDesc(0x30), the archive name at offset 0.
constexpr uintptr_t kStreamingFiles = 0x8E48D8;
constexpr unsigned kStreamingFileStride = 0x30;
constexpr unsigned kMaxArchives = 8;

// CStreaming::ms_aInfoForModel: 26316 x CStreamingInfo(0x14), m_CDSize at 0xC. Zero means no
// archive claimed this id, i.e. nothing in any IMG is named after the model.
constexpr unsigned kStreamingInfoStride = 0x14;
constexpr unsigned kCdSizeOffset = 0xC;

// ms_modelInfoPtrs, ms_aInfoForModel and the atomic store are read through ModelLimits, which
// knows whether they still sit at the vanilla globals or were moved to grow the DFF id range.

// CStore<T, N> keeps its element count in the first dword. The stores are what actually run out:
// vanilla SA leaves only a few dozen atomic slots free and fills the time and clump stores exactly.
constexpr uintptr_t kDamageStore = 0xB1BF58;
constexpr int kDamageStoreMax = 70;
constexpr uintptr_t kTimeStore = 0xB1C960;
constexpr int kTimeStoreMax = 169;
constexpr uintptr_t kClumpStore = 0xB1E958;
constexpr int kClumpStoreMax = 92;

constexpr uintptr_t kNumLoadedBuildings = 0xBCC0D8;
// CFileLoader::LoadScene collects entities in gpLoadedBuildings[4096] with no bounds check.
constexpr unsigned kMaxInstPerFile = 4096;

// Slot 0 ("generic") is the one col slot the streamer never touches: every CColStore loop starts at
// index 1. gta.dat's own COLFILE lines load into it, and so do we - the region's collision is then
// permanently in memory and never depends on the col quad tree, which cannot represent anything
// beyond +/-3000 anyway.
constexpr unsigned char kPermanentColSlot = 0;

// CPools::ms_pBuildingPool / ms_pDummyPool. Every inst line consumes one entry, and CPool::New
// returns null when full, which LoadObjectInstance would dereference straight away.
constexpr uintptr_t kBuildingPool = 0xB74498;
constexpr uintptr_t kDummyPool = 0xB744A0;
constexpr int kBuildingPoolReserve = 128;

unsigned StoreCount(uintptr_t store) { return *reinterpret_cast<unsigned*>(store); }

// FreeModelIdEnd defaults to the last DFF id (ModelIdLimit - 1); AtomicModelInfoLimit to the
// store's real capacity, which ModelIdLimit grows along with the id range.
int FreeIdEnd() {
    int end = Settings::world.freeModelIdEnd;
    int last = ModelLimits::ModelCount() - 1;
    return (end < 0 || end > last) ? last : end;
}
int AtomicLimit() {
    int limit = Settings::world.atomicModelInfoLimit;
    return limit < 0 ? ModelLimits::AtomicCapacity() : limit;
}

// CPool: objects, byteMap, size, firstFree. A byteMap entry's top bit marks a free slot.
int PoolFree(uintptr_t poolPointer) {
    const void** pool = *reinterpret_cast<const void***>(poolPointer);
    if (!pool) return 0;
    const unsigned char* map = reinterpret_cast<const unsigned char*>(pool[1]);
    int size = reinterpret_cast<const int*>(pool)[2];
    if (!map || size <= 0) return 0;
    int free = 0;
    for (int i = 0; i < size; i++)
        if (map[i] & 0x80) free++;
    return free;
}
void* ModelInfoAt(int id) { return reinterpret_cast<void**>(ModelLimits::ModelInfoPtrs())[id]; }
bool ModelHasArchiveEntry(int id) {
    return *reinterpret_cast<unsigned*>(ModelLimits::StreamingInfo() + (unsigned)id * kStreamingInfoStride + kCdSizeOffset) != 0;
}

struct RegionState {
    const MapRegion* region;
    std::map<int, int> ids;   // source model id -> SA model id
    int added = 0;
    int droppedNoId = 0;
    int droppedNoStore = 0;
};

// Install() runs from Main.cpp's static constructor, and MinGW initialises this translation unit's
// globals after that one, so anything reached at install time has to be a function-local static.
std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }
std::string& CacheDir() { static std::string s; return s; }

int gNextFreeId = 0;
bool gAssetsRegistered = false;
bool gInstancesPlaced = false;

// Function-local too: a SafetyHookInline owns a mutex and an allocator handle, so move-assigning
// create_inline's result into a global that has not been constructed yet crashes in the assignment.
SafetyHookInline& Init2Original() { static SafetyHookInline h; return h; }
SafetyHookInline& LoadLevelOriginal() { static SafetyHookInline h; return h; }

// ---------------------------------------------------------------- text helpers

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// CFileLoader::LoadLine (0x536F80) turns commas and every control character into spaces before the
// section matchers and sscanf calls see the line; do the same so one tokeniser fits every dialect.
std::string Normalise(const std::string& line) {
    std::string out = line;
    for (char& c : out)
        if ((unsigned char)c < (unsigned char)' ' || c == ',') c = ' ';
    return Trim(out);
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return lines;
    std::string current;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { lines.push_back(current); current.clear(); }
        else current.push_back((char)c);
    }
    if (!current.empty()) lines.push_back(current);
    fclose(f);
    return lines;
}

bool WriteWholeFile(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = text.empty() || fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok;
}

bool FileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && isspace((unsigned char)line[pos])) pos++;
        size_t start = pos;
        while (pos < line.size() && !isspace((unsigned char)line[pos])) pos++;
        if (pos > start) out.push_back(line.substr(start, pos - start));
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string SectionOf(const std::string& line) {
    return Lower(line.substr(0, 4));
}

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string SafeName(const std::string& s) {
    std::string out;
    for (char c : s)
        out.push_back(isalnum((unsigned char)c) ? c : '_');
    return out;
}

// ---------------------------------------------------------------- model ids

// Only ids with an empty ms_modelInfoPtrs slot are handed out, so the configured range is checked
// against what SA's own IDEs actually claimed rather than trusted.
int AllocateModelId() {
    int end = FreeIdEnd();
    while (gNextFreeId <= end) {
        int id = gNextFreeId++;
        if (id >= 0 && !ModelInfoAt(id)) return id;
    }
    return -1;
}

int FreeIdsLeft() {
    int end = FreeIdEnd();
    int count = 0;
    for (int id = gNextFreeId; id <= end; id++)
        if (id >= 0 && !ModelInfoAt(id)) count++;
    return count;
}

// ---------------------------------------------------------------- IDE

enum class IdeSection { None, Objs, Tobj, Anim, Txdp, Skip };

// Mirrors CFileLoader::LoadObject (0x5B3C60) closely enough to recover the flag word, which decides
// whether the line lands in the atomic store or in the 70-entry damage-atomic store.
bool ObjsIsDamageable(const std::string& line) {
    int id = 0, flags = 0, objType = 0;
    char name[64], txd[64];
    float dist = 0.0f, d2 = 0.0f, d3 = 0.0f;

    if (sscanf(line.c_str(), "%d %63s %63s %f %d", &id, name, txd, &dist, &flags) == 5 && dist >= 4.0f)
        return (flags & 0x1000) != 0;
    if (sscanf(line.c_str(), "%d %63s %63s %d", &id, name, txd, &objType) != 4) return false;

    flags = 0;
    if (objType == 1) sscanf(line.c_str(), "%d %63s %63s %d %f %d", &id, name, txd, &objType, &dist, &flags);
    else if (objType == 2) sscanf(line.c_str(), "%d %63s %63s %d %f %f %d", &id, name, txd, &objType, &dist, &d2, &flags);
    else if (objType == 3) sscanf(line.c_str(), "%d %63s %63s %d %f %f %f %d", &id, name, txd, &objType, &dist, &d2, &d3, &flags);
    return (flags & 0x1000) != 0;
}

struct StoreBudget {
    int atomic, damage, time, clump;
};

StoreBudget CurrentBudget() {
    StoreBudget b;
    b.atomic = AtomicLimit() - (int)StoreCount(ModelLimits::AtomicStore());
    b.damage = kDamageStoreMax - (int)StoreCount(kDamageStore);
    b.time = kTimeStoreMax - (int)StoreCount(kTimeStore);
    b.clump = kClumpStoreMax - (int)StoreCount(kClumpStore);
    return b;
}

std::string TranslateIde(RegionState& rs, const std::string& source) {
    std::vector<std::string> lines = ReadLines(source);
    if (lines.empty()) {
        Log::Write("[%s] ide: %s is missing or empty", rs.region->name.c_str(), source.c_str());
        return std::string();
    }

    StoreBudget budget = CurrentBudget();
    std::string out;
    IdeSection section = IdeSection::None;

    for (const std::string& raw : lines) {
        std::string line = Normalise(raw);
        if (line.empty() || line[0] == '#') continue;
        std::string head = SectionOf(line);

        if (section == IdeSection::None) {
            if (head == "objs") section = IdeSection::Objs;
            else if (head == "tobj") section = IdeSection::Tobj;
            else if (head == "anim") section = IdeSection::Anim;
            else if (head == "txdp") section = IdeSection::Txdp;
            else if (head == "peds" || head == "cars" || head == "weap" || head == "hier" ||
                     head == "path" || head == "2dfx")
                section = IdeSection::Skip;
            else continue;
            if (section != IdeSection::Skip) out += head + "\n";
            continue;
        }

        if (head == "end") {
            if (section != IdeSection::Skip) out += "end\n";
            section = IdeSection::None;
            continue;
        }
        if (section == IdeSection::Skip) continue;
        if (section == IdeSection::Txdp) { out += line + "\n"; continue; }

        size_t idEnd = line.find_first_of(" \t");
        if (idEnd == std::string::npos) { rs.droppedNoId++; continue; }
        char* stop = nullptr;
        long oldId = strtol(line.c_str(), &stop, 10);
        if (stop != line.c_str() + idEnd) { rs.droppedNoId++; continue; }

        int* pool = nullptr;
        if (section == IdeSection::Tobj) pool = &budget.time;
        else if (section == IdeSection::Anim) pool = &budget.clump;
        else pool = ObjsIsDamageable(line) ? &budget.damage : &budget.atomic;
        if (*pool <= 0) { rs.droppedNoStore++; continue; }

        int newId;
        auto known = rs.ids.find((int)oldId);
        if (known != rs.ids.end()) {
            newId = known->second;
        } else {
            newId = AllocateModelId();
            if (newId < 0) { rs.droppedNoId++; continue; }
            rs.ids[(int)oldId] = newId;
        }

        (*pool)--;
        out += std::to_string(newId) + line.substr(idEnd) + "\n";
        rs.added++;
    }
    return out;
}

// ---------------------------------------------------------------- IPL inst

struct Inst {
    int model;
    std::string name;
    unsigned interior;
    float x, y, z;
    float rx, ry, rz, rw;
    int lod;
};

struct IplStats {
    int unmappedModel = 0;
    int malformed = 0;
    int lodLost = 0;
    int scaled = 0;
    int interior = 0;
};

// GTA III (12 fields), Vice City (13, with an interior column) and San Andreas (11, with an
// interior column and a LOD index) all store the same thing in rx/ry/rz/rw: the conjugate of the
// object's rotation. III and VC turn it into a matrix with RwMatrixRotate(axis, -2*acos(w)), SA
// conjugates the quaternion and calls CMatrix::SetRotate - the same rotation - so the four
// components pass through untouched. The scale columns are dropped: San Andreas has no field for
// them, and III and VC parse them without ever using them either.
bool ParseInst(const std::string& line, Inst& inst, IplStats& stats) {
    std::vector<std::string> t = Tokens(line);
    int pos, scale = -1;
    if (t.size() == 11) pos = 3;        // SA:  id name interior x y z rx ry rz rw lod
    else if (t.size() == 12) pos = 2;   // III: id name x y z sx sy sz rx ry rz rw
    else if (t.size() == 13) pos = 3;   // VC:  id name interior x y z sx sy sz rx ry rz rw
    else { stats.malformed++; return false; }
    if (t.size() != 11) scale = pos + 3;

    inst.model = atoi(t[0].c_str());
    inst.name = t[1];
    inst.interior = t.size() == 12 ? 0u : (unsigned)strtoul(t[2].c_str(), nullptr, 10);
    inst.x = (float)atof(t[pos].c_str());
    inst.y = (float)atof(t[pos + 1].c_str());
    inst.z = (float)atof(t[pos + 2].c_str());

    int rot = scale < 0 ? pos + 3 : scale + 3;
    if (scale >= 0)
        for (int i = scale; i < scale + 3; i++)
            if (atof(t[i].c_str()) != 1.0) { stats.scaled++; break; }
    inst.rx = (float)atof(t[rot].c_str());
    inst.ry = (float)atof(t[rot + 1].c_str());
    inst.rz = (float)atof(t[rot + 2].c_str());
    inst.rw = (float)atof(t[rot + 3].c_str());
    inst.lod = t.size() == 11 ? atoi(t[10].c_str()) : -1;

    if (inst.interior) stats.interior++;
    // SA throws away small X/Y tilts unless bit 512 (CFileObjectInstance::m_bDontStream) is set,
    // and its check needs both components non-zero. III and VC always keep the tilt, so set the bit
    // where it makes the difference; SA's own lines are left exactly as authored.
    if (t.size() != 11 && inst.rx != 0.0f && inst.ry != 0.0f &&
        inst.rx >= -0.05f && inst.rx <= 0.05f && inst.ry >= -0.05f && inst.ry <= 0.05f)
        inst.interior |= 512u;
    return true;
}

std::vector<Inst> CollectInsts(const RegionState& rs, const std::string& source, IplStats& stats) {
    std::vector<Inst> kept;
    std::vector<int> newIndex;  // source inst index -> index in `kept`, -1 if dropped
    std::vector<std::string> lines = ReadLines(source);
    bool inInst = false;

    for (const std::string& raw : lines) {
        std::string line = Normalise(raw);
        if (line.empty() || line[0] == '#') continue;
        std::string head = SectionOf(line);

        if (!inInst) {
            if (head == "inst") inInst = true;
            continue;
        }
        if (head == "end") { inInst = false; continue; }

        Inst inst;
        if (!ParseInst(line, inst, stats)) { newIndex.push_back(-1); continue; }

        auto mapped = rs.ids.find(inst.model);
        if (mapped == rs.ids.end() || !ModelInfoAt(mapped->second)) {
            stats.unmappedModel++;
            newIndex.push_back(-1);
            continue;
        }
        inst.model = mapped->second;
        inst.x += rs.region->originX;
        inst.y += rs.region->originY;
        newIndex.push_back((int)kept.size());
        kept.push_back(inst);
    }

    // LOD parents are referenced by inst index within the file, so every surviving reference has to
    // follow the lines that were dropped.
    for (size_t i = 0, source_i = 0; source_i < newIndex.size(); source_i++) {
        if (newIndex[source_i] < 0) continue;
        Inst& inst = kept[i++];
        if (inst.lod < 0) continue;
        if ((size_t)inst.lod >= newIndex.size() || newIndex[inst.lod] < 0) {
            inst.lod = -1;
            stats.lodLost++;
        } else {
            inst.lod = newIndex[inst.lod];
        }
    }
    return kept;
}

std::string FormatInstFile(const std::vector<Inst>& insts, size_t first, size_t count, IplStats& stats) {
    std::string out = "inst\n";
    char buffer[256];
    for (size_t i = 0; i < count; i++) {
        const Inst& inst = insts[first + i];
        int lod = inst.lod;
        if (lod >= 0) {
            if ((size_t)lod < first || (size_t)lod >= first + count) { lod = -1; stats.lodLost++; }
            else lod -= (int)first;
        }
        snprintf(buffer, sizeof(buffer), "%d %s %u %.5f %.5f %.5f %.6f %.6f %.6f %.6f %d\n",
                 inst.model, inst.name.c_str(), inst.interior, inst.x, inst.y, inst.z,
                 inst.rx, inst.ry, inst.rz, inst.rw, lod);
        out += buffer;
    }
    out += "end\n";
    return out;
}

// ---------------------------------------------------------------- stages

void RegisterArchives() {
    unsigned used = 0;
    for (unsigned i = 0; i < kMaxArchives; i++)
        if (*reinterpret_cast<char*>(kStreamingFiles + i * kStreamingFileStride)) used++;

    for (RegionState& rs : Regions()) {
        for (const std::string& path : rs.region->imgFiles) {
            if (!FileExists(path)) {
                Log::Write("[%s] img: %s not found", rs.region->name.c_str(), path.c_str());
                continue;
            }
            // CClothes::Init registers player.img later, in CGame::Init2, so one slot has to be
            // left for it - it would otherwise get archive 0 back and stream clothes from gta3.img.
            if (used + 1 >= kMaxArchives) {
                Log::Write("[%s] img: %s NOT registered - CStreaming::ms_files is full (%u of %u "
                           "archives used, one reserved for player.img)",
                           rs.region->name.c_str(), path.c_str(), used, kMaxArchives);
                continue;
            }
            unsigned slot = CallAddImageToList(path.c_str(), true);
            used++;
            Log::Write("[%s] img: %s registered as archive %u (%u/%u slots used)",
                       rs.region->name.c_str(), path.c_str(), slot, used, kMaxArchives);
        }
    }
}

void LoadModelDefinitions() {
    gNextFreeId = Settings::world.freeModelIdStart;
    int available = FreeIdsLeft();
    StoreBudget budget = CurrentBudget();
    Log::Write("models: %d free id(s) in %d..%d; store headroom atomic %d, damage %d, time %d, clump %d",
               available, Settings::world.freeModelIdStart, FreeIdEnd(),
               budget.atomic, budget.damage, budget.time, budget.clump);

    for (RegionState& rs : Regions()) {
        for (const std::string& source : rs.region->ideFiles) {
            std::string text = TranslateIde(rs, source);
            if (text.empty()) continue;

            std::string cached = CacheDir() + "\\" + SafeName(rs.region->name) + "_" +
                                 SafeName(BaseName(source)) + ".ide";
            if (!WriteWholeFile(cached, text)) {
                Log::Write("[%s] ide: cannot write %s", rs.region->name.c_str(), cached.c_str());
                continue;
            }
            CallLoadObjectTypes(cached.c_str());
        }

        // A line the vanilla parser refused leaves an empty slot behind; drop it so no inst line can
        // later hand LoadScene a model id with no model info (LinkLods would dereference nullptr).
        int missing = 0;
        for (auto it = rs.ids.begin(); it != rs.ids.end();) {
            if (ModelInfoAt(it->second)) { ++it; continue; }
            it = rs.ids.erase(it);
            missing++;
            rs.added--;
        }

        if (rs.added || rs.droppedNoId || rs.droppedNoStore || missing)
            Log::Write("[%s] models: %d registered, %d dropped (no free id), %d dropped (model-info "
                       "store full), %d rejected by the game's own IDE parser",
                       rs.region->name.c_str(), rs.added, rs.droppedNoId, rs.droppedNoStore, missing);
        if (rs.droppedNoId)
            Log::Write("[%s] models: the free id range %d..%d ran out - raise ModelIdLimit (or "
                       "FreeModelIdEnd if it was lowered)", rs.region->name.c_str(),
                       Settings::world.freeModelIdStart, FreeIdEnd());
        if (rs.droppedNoStore)
            Log::Write("[%s] models: SA's model-info stores ran out (atomic limit %d) - this is the "
                       "hard wall, not the id range", rs.region->name.c_str(), AtomicLimit());
    }
}

// CFileLoader::LoadCollisionFile walks the chunk chain (4cc, size, then 24 bytes of name and model
// id, with `size` counted from after the 4cc/size pair) into a fixed 32 KB scratch buffer at
// 0xBC40D8, and it checks neither the size nor the 4cc. A model too big for the buffer, or the
// zero padding an IMG extractor leaves at the end of a .col, would both send it off the rails - so
// walk the chunks here first and hand the game a copy trimmed to the chunks that are actually
// sound. Returns the path to load, or an empty string if there is nothing usable.
std::string PrepareColFile(const RegionState& rs, const std::string& path, int& models) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();

    models = 0;
    long good = 0;
    unsigned char header[32];
    while (fread(header, 1, sizeof(header), f) == sizeof(header)) {
        if (memcmp(header, "COL", 3) != 0 || header[3] < '1' || header[3] > '4') break;
        unsigned size = *reinterpret_cast<unsigned*>(header + 4);
        if (size < 24) break;
        if (size - 24 > 0x8000) {
            Log::Write("[%s] col: %s stops at '%.22s' - %u bytes of collision data, more than the "
                       "game's 32768-byte buffer", rs.region->name.c_str(), path.c_str(),
                       (const char*)header + 8, size - 24);
            break;
        }
        if (header[3] == '4')
            Log::Write("[%s] col: %s contains COL4 data, which this loader path does not read",
                       rs.region->name.c_str(), path.c_str());
        if (fseek(f, (long)(size - 24), SEEK_CUR) != 0) break;
        good = ftell(f);
        models++;
    }

    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    if (models == 0) {
        fclose(f);
        Log::Write("[%s] col: %s holds no readable collision models", rs.region->name.c_str(), path.c_str());
        return std::string();
    }
    if (good == total) { fclose(f); return path; }

    std::string trimmed;
    trimmed.resize((size_t)good);
    fseek(f, 0, SEEK_SET);
    bool read = fread(&trimmed[0], 1, trimmed.size(), f) == trimmed.size();
    fclose(f);

    std::string cached = CacheDir() + "\\" + SafeName(rs.region->name) + "_" +
                         SafeName(BaseName(path)) + ".col";
    if (!read || !WriteWholeFile(cached, trimmed)) {
        Log::Write("[%s] col: cannot write the trimmed copy %s", rs.region->name.c_str(), cached.c_str());
        return std::string();
    }
    Log::Write("[%s] col: %s has %ld byte(s) of trailing data after its last collision model; "
               "loading the trimmed copy %s", rs.region->name.c_str(), path.c_str(), total - good,
               cached.c_str());
    return cached;
}

void LoadCollisionArchives() {
    for (RegionState& rs : Regions()) {
        for (const std::string& path : rs.region->colFiles) {
            if (!FileExists(path)) {
                Log::Write("[%s] col: %s not found", rs.region->name.c_str(), path.c_str());
                continue;
            }
            int models = 0;
            std::string load = PrepareColFile(rs, path, models);
            if (load.empty()) continue;
            CallLoadCollisionFile(load.c_str(), kPermanentColSlot);
            Log::Write("[%s] col: %s loaded into col slot %u - %d collision model(s), permanently in "
                       "memory (the streamer never touches slot 0)",
                       rs.region->name.c_str(), path.c_str(), kPermanentColSlot, models);
        }
    }
}

void ReportMissingArchiveEntries() {
    for (RegionState& rs : Regions()) {
        int missing = 0;
        for (const auto& pair : rs.ids)
            if (!ModelHasArchiveEntry(pair.second)) missing++;
        if (missing)
            Log::Write("[%s] models: %d of %d have no .dff in any registered archive - they will be "
                       "invisible (check the IMG contents and that no SA model shares the name)",
                       rs.region->name.c_str(), missing, (int)rs.ids.size());
    }
}

void PlaceInstances() {
    int budget = PoolFree(kBuildingPool) - kBuildingPoolReserve;
    Log::Write("inst: %d free building-pool entries, %d free dummy-pool entries",
               budget + kBuildingPoolReserve, PoolFree(kDummyPool));

    for (RegionState& rs : Regions()) {
        int placed = 0, files = 0, refused = 0;
        IplStats stats;

        for (const std::string& source : rs.region->iplFiles) {
            std::vector<Inst> insts = CollectInsts(rs, source, stats);
            if (insts.empty()) continue;
            if ((int)insts.size() > budget) {
                refused += (int)insts.size() - (budget > 0 ? budget : 0);
                insts.resize(budget > 0 ? (size_t)budget : 0);
                if (insts.empty()) continue;
            }
            budget -= (int)insts.size();

            for (size_t first = 0; first < insts.size(); first += kMaxInstPerFile) {
                size_t count = insts.size() - first;
                if (count > kMaxInstPerFile) count = kMaxInstPerFile;

                char suffix[32];
                snprintf(suffix, sizeof(suffix), "_%u", (unsigned)(first / kMaxInstPerFile));
                std::string cached = CacheDir() + "\\" + SafeName(rs.region->name) + "_" +
                                     SafeName(BaseName(source)) + suffix + ".ipl";
                if (!WriteWholeFile(cached, FormatInstFile(insts, first, count, stats))) {
                    Log::Write("[%s] ipl: cannot write %s", rs.region->name.c_str(), cached.c_str());
                    continue;
                }
                CallLoadScene(cached.c_str());
                placed += (int)*reinterpret_cast<unsigned*>(kNumLoadedBuildings);
                files++;
            }
        }

        if (placed || stats.unmappedModel || stats.malformed)
            Log::Write("[%s] inst: %d entity/entities placed from %d file(s); %d line(s) skipped "
                       "(model not defined by this region's IDEs), %d unrecognised, %d LOD link(s) lost",
                       rs.region->name.c_str(), placed, files, stats.unmappedModel, stats.malformed,
                       stats.lodLost);
        if (stats.scaled)
            Log::Write("[%s] inst: %d line(s) carried a non-unit scale, which San Andreas has no "
                       "field for - placed at scale 1", rs.region->name.c_str(), stats.scaled);
        if (refused)
            Log::Write("[%s] inst: %d line(s) dropped - SA's 13000-entry building pool has no room "
                       "left", rs.region->name.c_str(), refused);
        if (stats.interior)
            Log::Write("[%s] inst: %d line(s) name an interior; the number is kept as written, and "
                       "the source game's interior numbering is not San Andreas'",
                       rs.region->name.c_str(), stats.interior);
    }
}

// ---------------------------------------------------------------- hooks

void __cdecl Hooked_StreamingInit2() {
    if (!gAssetsRegistered) {
        gAssetsRegistered = true;
        RegisterArchives();
        LoadModelDefinitions();
        LoadCollisionArchives();
    }
    Init2Original().ccall<void>();
    ReportMissingArchiveEntries();
}

void __cdecl Hooked_LoadLevel(const char* filename) {
    LoadLevelOriginal().ccall<void>(filename);
    // Only the gta.dat pass reaches Init2 and loads IPLs; default.dat returns before any of it.
    // Nothing here runs a second time, so a restart keeps the entities and models already in place.
    if (gAssetsRegistered && !gInstancesPlaced) {
        gInstancesPlaced = true;
        PlaceInstances();
    }
}

}  // namespace

namespace CityLoader {
    void Install() {
        for (const MapRegion& r : Settings::regions)
            if (!r.imgFiles.empty() || !r.ideFiles.empty() || !r.colFiles.empty())
                Regions().push_back({ &r });
        if (Regions().empty()) return;

        CacheDir() = GAME_PATH("MapExpander\\cache");
        CreateDirectoryA(std::string(GAME_PATH("MapExpander")).c_str(), nullptr);
        if (!CreateDirectoryA(CacheDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            Log::Write("city: cannot create %s - region assets not loaded", CacheDir().c_str());
            Regions().clear();
            return;
        }

        Init2Original() = safetyhook::create_inline(reinterpret_cast<void*>(0x5B8AD0),
                                                    reinterpret_cast<void*>(&Hooked_StreamingInit2));
        LoadLevelOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x5B9030),
                                                       reinterpret_cast<void*>(&Hooked_LoadLevel));
        Log::Write("city: loading own assets for %u region(s), cache in %s",
                   (unsigned)Regions().size(), CacheDir().c_str());
    }
}
