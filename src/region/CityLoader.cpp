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
#include <set>
#include <string>
#include <vector>
#include "CityLoader.h"
#include "ArchiveSlots.h"
#include "ModelLimits.h"
#include "RegionWeapons.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

typedef unsigned(__cdecl* AddImageToListFn)(const char*, bool);
typedef void(__cdecl* LoadFileFn)(const char*);

const AddImageToListFn CallAddImageToList = reinterpret_cast<AddImageToListFn>(0x407610);
const LoadFileFn CallLoadObjectTypes = reinterpret_cast<LoadFileFn>(0x5B8400);
const LoadFileFn CallLoadScene = reinterpret_cast<LoadFileFn>(0x5B8700);

// CStreaming::ms_files: tStreamingFileDesc(0x30) with the archive name at offset 0. Vanilla San
// Andreas has eight of them; ArchiveSlots moves the table to 32, which is CdStream's own ceiling.
// Both the count and the address are asked for rather than assumed, so this is right either way.
// That name is 40 bytes, with the "not player.img" flag right behind it at 0x28, and
// CStreaming::AddImageToList copies into it without leaving room for a terminator. A longer path
// comes back out truncated, CFileMgr::OpenFile fails on it and CStreaming::LoadCdDirectory binds
// nothing at all - with no complaint of its own, so the whole archive's models simply stay
// invisible. gta.dat's own IMG lines are game-relative and CFileMgr resolves them against the game
// directory, so give a region's archive its path back in that form.
constexpr size_t kArchivePathMax = 39;

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
// The time store (tobj lines) is ModelLimits' now: it moves it to the heap with room for a region's
// timed models, since vanilla SA fills the original 169 exactly.
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
// returns null when full, which LoadObjectInstance dereferences straight away (0x5381A5).
//
// Vanilla San Andreas places about 9200 buildings from gta.dat and leaves the rest of the 13000 for
// the IPL sections it streams in as the camera moves, so a region's entities cannot come out of
// what is left: fitting them in there both loses most of the city and starves the streamer, which
// then dies on the first section it cannot place. Grow both pools by what the region's IPLs hold
// instead, before CPools::Initialise creates them, and SA keeps exactly the headroom it had.
constexpr uintptr_t kBuildingPool = 0xB74498;
constexpr uintptr_t kDummyPool = 0xB744A0;
constexpr int kBuildingPoolReserve = 128;
constexpr uintptr_t kBuildingPoolSizeOperand = 0x55105F;  // imm32 of `push 13000` in CPools::Initialise
constexpr uintptr_t kDummyPoolSizeOperand = 0x5510CF;     // imm32 of `push 2500`
constexpr int kVanillaBuildingPool = 13000;
constexpr int kVanillaDummyPool = 2500;
// An inst line becomes a building or a dummy depending on flags this pre-count cannot read, so both
// pools get room for all of them; the entries are 0x40-odd bytes each.
constexpr int kEntityPoolMargin = 256;

// CPools::ms_pPtrNodeSingleLinkPool. CEntity::Add files a static entity into every sector its bound
// rect covers, and each of those takes one node out of this pool; CPtrListSingleLink::AddItem then
// writes through whatever CPool::New handed back, so a full pool is a null store at 0x533606. A
// region's buildings need their own nodes on top of the ones San Andreas' map already holds, and a
// building spans a few 50-unit sectors, so budget several per inst line.
constexpr uintptr_t kPtrNodePoolSizeOperand = 0x550F46;  // imm32 of `push 70000` in CPools::Initialise
constexpr uintptr_t kPtrNodeDoubleLinkPoolSizeOperand = 0x550F82;  // imm32 of `push 3200` in CPools::Initialise
constexpr int kVanillaPtrNodePool = 70000;
constexpr int kVanillaPtrNodeDoubleLinkPool = 3200;
// Eight is a budget for VANILLA 50-unit sectors - a building spanning 3 x 3 of them, with slack -
// and it stays an over-estimate however the world grid is fitted, which is why nothing here has to
// know the cell size. WorldSectors stretches a fixed 120 x 120 grid over whatever ground is in use,
// so the cell is span/120; San Andreas' own map is already 6000 units, and any world with a region
// in it is wider, so the cell is always more than 6000/120 = 50 and a building always covers no
// more sectors than it would in vanilla. San Andreas itself gets by on 70000 nodes for a pool of
// 13000 entities - 5.4 each at 50 units - so eight a line has room in hand at any cell this can
// produce. It is the other direction that would need re-doing: if the grid ever covered less
// ground than the whole world at a time, cells could reach 50 and this would be the number to
// check first.
constexpr int kPtrNodesPerInstLine = 8;
constexpr int kPtrNodePoolMargin = 1024;

// CPools::ms_pColModelPool, same story: CColModel::operator new (0x40FC30) is CPool::New on this
// pool, and CFileLoader::LoadCollisionFileFirstTime hands whatever it returns straight to
// LoadCollisionModelVer2/3/4, which write the bounding sphere through it with no null check. Every
// collision model a region adds takes an entry, and vanilla San Andreas leaves only a few hundred
// of the 10150 spare - so the pool has to be grown before CPools::Initialise creates it, or SA's
// own collision runs out of entries halfway through CColStore::LoadAllBoundingBoxes and the game
// dies at CFileLoader::LoadCollisionModelVer3+0x32 on a model that has nothing to do with us.
constexpr uintptr_t kColModelPoolSizeOperand = 0x551107;  // imm32 of `push 10150` in CPools::Initialise

// CTxdStore keeps one TxdDef per dictionary name in a CPool of 5000, created by `push 5000` in
// CTxdStore::Initialise (0x731F5F). San Andreas' own map uses most of that, and AddTxdSlot writes
// into whatever the pool hands back without looking: with Vice City's 820 dictionaries there was
// room, with Liberty City's 422 on top there was not, and the 5001st slot was a write to address 0.
constexpr uintptr_t kTxdPoolSizeOperand = 0x731F60;
constexpr int kVanillaTxdPool = 5000;
constexpr int kTxdPoolMargin = 256;
constexpr int kVanillaColModelPool = 10150;
constexpr int kColModelPoolMargin = 128;

// ---------------------------------------------------------------- object data
//
// Whether a placed model is scenery or a thing the world can push, break and smash comes down to a
// single number: the int16 at CBaseModelInfo+0x10, the index of that model's row in
// CObjectData::ms_aObjectInfo. CFileLoader::LoadObjectInstance (0x538090) reads it at 0x5380DB:
//
//     cmp word ptr [ebp+0x10], bx    ; bx = -1, what CBaseModelInfo::Init writes
//     je  <0x538146>                 ; -1: new CBuilding (or CAnimatedBuilding)
//                                    ; anything else: new CDummyObject
//
// and that is the whole rule. It is not an IDE flag and not a model type. A dummy object is what
// CPopulation::ConvertToRealObject (0x614580) promotes to a real CObject once the player is close
// enough, and only a CObject has mass, buoyancy, damage or breakage; a CBuilding is geometry that
// nothing can touch. The index is written in exactly one place - CObjectData::Initialise (0x5B5360),
// which reads DATA\OBJECT.DAT and matches each line's first column to a model BY NAME through
// CModelInfo::GetModelInfo. So a model that no object.dat names is a building, always. That is why
// an imported city's glass, crates, cones and parking meters stood there like walls: the importer
// never brought object.dat across, so every single one of its models had index -1.
//
// Breakable glass is the same story from the other end. CGlass::IsObjectGlass (0x46A760) asks two
// questions - is the entity a CObject, and does the model info's special type (bits 11..14 of the
// word at +0x12, set from IDE flags 0x200 and 0x400) say glass - and every CGlass entry point in
// CPhysical::ApplyCollision, CWorld::TriggerExplosionSectorList and CWeapon::DoBulletImpact is
// gated on it. Vice City writes those same two flag bits with those same two meanings, so they
// already survive the import untouched; what was missing was the object.dat line that makes the
// pane a CObject in the first place.
//
// The table itself is 160 entries of 0x50 bytes at 0xBB4A90, ending exactly where the next global
// (a CObject.cpp static float, written by the CRT thunk at 0x84FCE0) begins. Initialise fills
// entries 0..4 itself from one template and appends parsed lines from index 5; it de-duplicates,
// both through a fast path (mass 99999, damage multiplier 1, damage effect 0 and special response
// 0 or 4 reuse one of the four built-in entries) and through a full field-by-field scan of what it
// has already stored, so San Andreas' 995 lines collapse into exactly the 160 entries the array
// holds. Vanilla therefore leaves NO room at all - and there is no bounds check anywhere in
// Initialise, so one extra row would run straight into the statics behind the array.
//
// The array is moved to the heap for that reason, the same way ModelLimits moves the model info
// stores: 34 code operands name it or one of its first five entries, every one of them is checked
// to still hold the value this table expects before anything is written, and on any mismatch the
// game is left exactly as it was.
constexpr uintptr_t kObjectInfo = 0xBB4A90;
constexpr unsigned kObjectInfoStride = 0x50;
constexpr int kVanillaObjectInfoCount = 160;
constexpr int kObjectInfoMargin = 32;
// Initialise's own count is a stack local, never a global, so the number of entries in use has to
// be recovered from the table: the first one that is still all zeroes. A parsed row always has a
// mass (the file's own range is 1..50000), and the block is allocated zeroed, so the scan is exact.
constexpr int kFirstParsedObjectInfo = 5;   // 0..4 are the hard-coded defaults
constexpr unsigned kObjectInfoIndexOffset = 0x10;   // int16 on CBaseModelInfo; -1 means "building"
// Read rather than written down, so this stays right if the values are ever patched: Initialise
// computes the buoyancy constant as (100.0 / percentSubmerged) * mass * 0.008 at 0x5B5536.
constexpr uintptr_t kBuoyancyNumerator = 0x858628;
constexpr uintptr_t kBuoyancyScale = 0x863984;

typedef void*(__cdecl* GetModelInfoByNameFn)(const char*, int*);
const GetModelInfoByNameFn CallGetModelInfoByName = reinterpret_cast<GetModelInfoByNameFn>(0x4C5940);

// Every instruction whose imm32 or disp32 names the array or one of its first five entries, found
// by disassembling the live sections and checking each operand; nothing in .rdata or .data points
// into it. `operandOffset` is where the four bytes sit inside the instruction.
struct ObjectInfoSite { uintptr_t address; unsigned char operandOffset; uint32_t oldValue; };
const ObjectInfoSite kObjectInfoSites[] = {
    { 0x59F857, 6, 0xBB4A90 },  // CObject::Init: m_pObjectInfo defaults to entry 0
    { 0x5A2D28, 2, 0xBB4A90 },  // CObjectData::SetObjectData: index * 0x50 + base
    { 0x5A2E44, 6, 0xBB4A90 },  // SetObjectData's "model has no object data" fallback
    { 0x5B5369, 2, 0xBB4A90 }, { 0x5B5373, 2, 0xBB4A94 }, { 0x5B537D, 2, 0xBB4A98 },
    { 0x5B5387, 2, 0xBB4A9C }, { 0x5B5391, 2, 0xBB4AA0 }, { 0x5B539B, 2, 0xBB4AA4 },
    { 0x5B53A5, 2, 0xBB4AA8 }, { 0x5B53AF, 2, 0xBB4AAC }, { 0x5B53B6, 2, 0xBB4AAD },
    { 0x5B53BD, 2, 0xBB4AAE },                            // entry 0, field by field
    { 0x5B53C9, 1, 0xBB4A90 }, { 0x5B53CE, 1, 0xBB4AE0 }, { 0x5B53D5, 2, 0xBB4AFE },  // entry 1
    { 0x5B53E1, 1, 0xBB4A90 }, { 0x5B53E6, 1, 0xBB4B30 }, { 0x5B53EF, 1, 0xBB4B4D },  // entry 2
    { 0x5B53F9, 1, 0xBB4A90 }, { 0x5B53FE, 1, 0xBB4B80 }, { 0x5B5405, 1, 0xBB4B9D },
    { 0x5B540A, 2, 0xBB4B9E },                                                        // entry 3
    { 0x5B5416, 1, 0xBB4A90 }, { 0x5B541B, 1, 0xBB4BD0 }, { 0x5B542C, 2, 0xBB4BED },  // entry 4
    { 0x5B5466, 1, 0xBB4C20 },  // where parsing starts: base + 5 * 0x50
    { 0x5B5587, 2, 0xBB4A90 }, { 0x5B5596, 2, 0xBB4AA8 }, { 0x5B55A6, 2, 0xBB4AAC },
    { 0x5B55B1, 2, 0xBB4AAD }, { 0x5B55BB, 2, 0xBB4B4D },  // the fast path's comparisons
    { 0x5B5608, 1, 0xBB4A94 },  // the de-duplication walk, biased by one field
    { 0x73860C, 6, 0xBB4BD0 },  // CProjectileInfo::AddProjectile: grenades take entry 4
};

// CIplStore keeps one entity-index array per IPL file - the table that turns an inst line's LOD
// column into the entity the streamer later loads for it - and CFileLoader::LoadScene takes a fresh
// one every time it runs. CIplStore::GetNewIplEntityIndexArray appends to that table with no bounds
// check at all, and the table is 40 pointers with the store's own globals directly behind it:
//
//     0x8E3F00 ms_numEntityIndexArrays   0x8E3F08 ms_pEntityIndexArrays[40] (ends at 0x8E3FA8)
//     0x8E3FA8 a flag   0x8E3FAC ms_pQuadTree   0x8E3FB0 ms_pPool
//
// San Andreas' own gta.dat uses about 29 of them, so the spare room is not enough for a region's
// IPLs: entry 41 lands on the quad tree and entry 42 on ms_pPool, replacing the pool with a
// freshly malloc'd int array. Nothing notices until CIplStore::SetupRelatedIpls walks the pool at
// the end of the next LoadScene and follows m_pObjects into unmapped memory. So move the table to
// the heap with room for the region's files. Only the five instructions that name it change; the
// count keeps its global, and CIplStore::Shutdown frees out of the new table the same way.
constexpr uintptr_t kIplEntityArrays = 0x8E3F08;
constexpr uintptr_t kIplEntityArrayCount = 0x8E3F00;
constexpr int kVanillaIplEntityArrays = 40;
constexpr int kIplEntityArrayMargin = 64;
constexpr uintptr_t kIplEntityArraySites[] = {
    0x405C3D,   // CIplStore::LoadIplBoundingBox   mov eax, [edx*4 + table]
    0x40619B,   // CIplStore::LoadIpl              mov edx, [ecx*4 + table]
    0x1561160,  // CIplStore::Shutdown             mov edx, [esi*4 + table]
    0x15649FA,  // GetNewIplEntityIndexArray       mov [ecx*4 + table], eax
    0x1569777,  // GetIplEntityIndexArray          mov eax, [eax*4 + table]
};

int gIplEntityArrayCapacity = kVanillaIplEntityArrays;

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
    // The same for the region's pedestrians, and deliberately not the same map. A donor's ped ids
    // start at 0 and so do GTA III's buildings - the imported Liberty City IDEs run from id 0 to
    // 3152 - so one map would hand a ped and a building the same San Andreas id and an `inst` line
    // would then place a pedestrian where a wall should be. Peds are also never named by an inst
    // line, never want the placeholder collision box, and never take part in LOD pairing, all of
    // which walk rs.ids.
    std::map<int, int> pedIds;
    // Ids and names already used by this region's own ped lines. NameAlreadyTaken only sees model
    // infos that exist, and a ped IDE is not handed to the game until the whole file is rewritten,
    // so two lines in one file could otherwise both pass it and the second would overwrite the
    // first's ms_modelInfoPtrs slot, leaking a ped store entry and leaving one model unreachable.
    std::set<int> pedSourceIds;
    std::set<std::string> pedNames;
    std::map<int, std::string> names;              // SA model id -> name as the IDE declares it
    std::map<std::string, std::string> originals;  // lowercase declared name -> name in the source game
    int added = 0;
    int droppedNoId = 0;
    int droppedNoStore = 0;

    int pedsAdded = 0;
    int pedsNoId = 0;
    int pedsNoStore = 0;
    int pedsShortLine = 0;      // fewer than the 14 columns LoadPedObject's sscanf reads
    int pedsNameTaken = 0;      // a model of that name already exists; loading it would shadow one
    int pedsDuplicate = 0;      // a second line of this region's with the same source id or name
    int pedsBadPedType = 0;     // a PED_TYPE CPedType::FindPedType answers 32 to
    int pedsBadAnimGroup = 0;   // an animation group San Andreas does not declare
    std::string pedsRefusedNames;
};

// Install() runs from Main.cpp's static constructor, and MinGW initialises this translation unit's
// globals after that one, so anything reached at install time has to be a function-local static.
std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }
std::string& CacheDir() { static std::string s; return s; }

int gNextFreeId = 0;
bool gAssetsRegistered = false;
bool gInstancesPlaced = false;
bool gPlacesOwnEntities = false;

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

// Settings resolves every path in the INI against the game directory; this puts one back the way
// it was written, for the places that pass a path on to the game rather than open it themselves.
std::string GameRelativePath(const std::string& path) {
    // GAME_PATH(x) is the game directory with x appended, so asking it for a one-character name and
    // dropping that character leaves the prefix Settings put in front of every configured path.
    std::string probe = std::string(GAME_PATH("?"));
    std::string root = probe.empty() ? probe : probe.substr(0, probe.size() - 1);
    if (!root.empty() && path.size() > root.size() &&
        _strnicmp(path.c_str(), root.c_str(), root.size()) == 0)
        return path.substr(root.size());
    return path;
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
//
// A pedestrian is given an id out of exactly the same range as a building, and the ranges San
// Andreas does reserve for peds by value are all far below it: CStreaming::RequestSpecialChar and
// its neighbours (0x40B450, 0x40B470, 0x40B490) hardcode `add reg, 0x122` so a special character is
// always ids 290 upwards, and CCutsceneMgr (0x5B02C0) gates on 300..319 and indexes a 20-entry
// table by (id - 300). FreeModelIdStart defaults to 18631, so nothing here can reach either.
//
// Otherwise nothing in San Andreas indexes anything by a ped's model id: CPopulation's ped groups
// hold ids as int16 read back with movzx (0xC0F358, 21 per group, with 2000 as the "empty slot"
// sentinel), CStreaming::ms_pedsLoaded holds eight full dwords, and CStreaming::RemoveModel decides
// a model is a ped from its vtable rather than its number. The one real width limit is
// CEntity::m_nModelIndex at +0x22, which is read sign-extended everywhere (0x5E48AE in
// CPed::SetModelIndex among them) - so 32767, and ModelIdLimit stops at 26400 anyway.
// ---------------------------------------------------------------- the gaps below the window
//
// FreeModelIdStart..FreeModelIdEnd is not the whole of the free id space, and once ModelIdLimit is
// at its ceiling it is not even the larger half. San Andreas' own IDE files declare 14832 models
// and the highest id any of them uses is 18630, so of the 20000 ids below the raised range 5168
// are declared by nothing at all - 1369 of them above 18631, where the window already is, and
// 3799 BELOW it, in runs as long as 1118 (11682..12799), 935 (15065..15999) and 492
// (13891..14382). Counted out of the game's own data/ tree, not recalled.
//
// Those 3799 are worth about half a city, and they cost nothing to take: no operand is patched and
// no array is moved, because an id with no CBaseModelInfo is an id the game has no way to name.
// They are a FALLBACK rather than the first choice, and deliberately so:
//
//   * the ids a region already had do not move when a later region is added, which is the property
//     docs/save-compatibility.md asks for. Two cities get exactly the ids they got before this;
//     only the third reaches down here;
//   * an id below the window is likelier to be one somebody else's mod also went looking for, and
//     ModelInfoAt only sees what already exists when we look. Taking them last means taking as few
//     as the regions actually need;
//   * FreeModelIdGapStart keeps the floor above everything San Andreas addresses by number rather
//     than by name: peds 0..312, weapon models 321..373, vehicles 400..611, and the 2000 that
//     CPopulation's ped groups use as their empty-slot sentinel. 3000 clears all of them, and
//     there are only 78 free ids below it in any case.
//
// Setting FreeModelIdGapStart to -1 turns the fallback off and leaves the window as the whole of it.
int gGapCursor = -1;
int gGapFirst = -1, gGapLast = -1, gGapTaken = 0;

int GapFloor() { return Settings::world.freeModelIdGapStart; }
int GapCeiling() { return Settings::world.freeModelIdStart; }   // the window starts where this stops

int AllocateModelId() {
    int end = FreeIdEnd();
    while (gNextFreeId <= end) {
        int id = gNextFreeId++;
        if (id >= 0 && !ModelInfoAt(id)) return id;
    }
    if (GapFloor() < 0) return -1;
    if (gGapCursor < GapFloor()) gGapCursor = GapFloor();
    while (gGapCursor < GapCeiling()) {
        int id = gGapCursor++;
        if (ModelInfoAt(id)) continue;
        if (gGapFirst < 0) gGapFirst = id;
        gGapLast = id;
        gGapTaken++;
        return id;
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

// The same for the gaps, so the log can say how much is behind the window as well as in it.
int FreeIdsInGaps() {
    if (GapFloor() < 0) return 0;
    int count = 0;
    for (int id = gGapCursor < GapFloor() ? GapFloor() : gGapCursor; id < GapCeiling(); id++)
        if (id >= 0 && !ModelInfoAt(id)) count++;
    return count;
}

// ---------------------------------------------------------------- IDE

enum class IdeSection { None, Objs, Tobj, Anim, Weap, Peds, Txdp, Skip };

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

// ---------------------------------------------------------------- what a `peds` line needs
//
// CFileLoader::LoadPedObject (0x5B7420) reads FOURTEEN columns with one sscanf:
//
//     %d %s %s %s %s %s %x %x %s %d %d %s %s %s
//      id model txd  pedType statType animGroup  carMask flags  animFile  radio1 radio2
//      audioPedType voice1 voice2
//
// and it does not look at sscanf's return value. A short line therefore leaves uninitialised stack
// in the buffers it did not fill, and those go straight into CKeyGen::GetUppercaseKey and
// CTxdStore::FindTxdSlot. So the column count is checked here rather than hoped for.
//
// Two of the columns are looked up in a table by name, and BOTH of those lookups answer an unknown
// name with a number that is one past the end of the array it indexes. Neither is checked by the
// game, and neither goes wrong until much later and somewhere else, so both are checked here.
//
//   Animation group. LoadPedObject seeds the index with CAnimManager::ms_numAnimAssocDefinitions
//   (0x5B7432, `mov ebx, [0xB4EA28]` - 118, set by Animation::Initialise at 0x5BF6C1), walks the
//   table comparing names, and writes whatever ebx still holds into CPedModelInfo + 0x24 (0x5B755F).
//   An unknown group therefore leaves 118 there, and CPed::SetModelIndex (0x5E498B) hands that
//   straight to CAnimManager::GetAnimAssocGroup, whose array is exactly 118 entries of 0x14 bytes
//   on the heap (0x4D3A40). Entry 118 is past the allocation.
//
//   Ped type. CPedType::FindPedType (0x608790) walks 32 names and, for anything it does not know
//   that is not "PLAYER_NETWORK" or "PLAYER_UNUSED", returns 32 (0x608825, `add eax, 0x20`) into
//   CPedModelInfo + 0x28. CPedType::ms_apPedType holds 32; GetPedFlag guards the index but the
//   acquaintance getters (0x6089B0 and its neighbours) do not.
//
// tools/convert_peds.py already maps a donor's group onto one animgrp.dat declares and whitelists
// the ped type against San Andreas' 26 names. These are the checks that say so out loud, and that
// hold for a peds.ide written by hand.
constexpr uintptr_t kAnimGroupCount = 0xB4EA28;     // CAnimManager::ms_numAnimAssocDefinitions
constexpr uintptr_t kGetAnimGroupName = 0x4D3A20;   // the name of definition n, as LoadPedObject reads it
constexpr uintptr_t kFindPedType = 0x608790;        // CPedType::FindPedType, __cdecl
constexpr int kPedTypeCount = 32;                   // CPedType::ms_apPedType
constexpr int kPedIdeColumns = 14;

bool AnimGroupDeclared(const std::string& name) {
    typedef const char*(__cdecl* GetNameFn)(int);
    int count = (int)patch::GetUInt(kAnimGroupCount);
    for (int i = 0; i < count; i++) {
        const char* have = reinterpret_cast<GetNameFn>(kGetAnimGroupName)(i);
        if (have && _stricmp(have, name.c_str()) == 0) return true;
    }
    return false;   // including a table that is not up yet, which is reported once by the caller
}

bool PedTypeKnown(const std::string& name) {
    typedef int(__cdecl* FindPedTypeFn)(const char*);
    return reinterpret_cast<FindPedTypeFn>(kFindPedType)(name.c_str()) < kPedTypeCount;
}

// CModelInfo::GetModelInfo (0x4C5940) hashes the name and scans ms_modelInfoPtrs for the key; its
// bounds are among the sites ModelLimits rewrites, so it sees the whole raised range. A `peds` line
// naming something that already exists must not be loaded: two model infos would share a key, and
// which one every later lookup by name answers with is then a matter of where the scan starts.
// This is what stops a donor's un-namespaced ped section - `wmyst`, `bfyst` - from shadowing San
// Andreas' own peds of the same name.
bool NameAlreadyTaken(const std::string& name) {
    int id = -1;
    return CallGetModelInfoByName(name.c_str(), &id) != nullptr;
}

struct StoreBudget {
    int atomic, damage, time, clump, weapon, ped;
};

StoreBudget CurrentBudget() {
    StoreBudget b;
    b.atomic = AtomicLimit() - (int)StoreCount(ModelLimits::AtomicStore());
    b.damage = kDamageStoreMax - (int)StoreCount(kDamageStore);
    b.time = ModelLimits::TimeCapacity() - (int)StoreCount(ModelLimits::TimeStore());
    b.clump = kClumpStoreMax - (int)StoreCount(kClumpStore);
    // Vanilla San Andreas' data/peds.ide declares 276 of the ped store's 278, so a region's peds
    // only fit when ModelLimits has moved that store to the heap; if it has not, this is 2.
    b.ped = ModelLimits::PedCapacity() - (int)StoreCount(ModelLimits::PedStore());
    // Vanilla San Andreas fills 50 of the weapon store's 51, so a region's weapon models only fit
    // when RegionWeapons has moved that store to the heap; if it has not, this is 1 and the weap
    // lines are dropped with the same message as any other full store.
    b.weapon = RegionWeapons::Capacity() - (int)StoreCount(RegionWeapons::Store());
    return b;
}

// The importer namespaces every model and dictionary name (docs/asset-namespacing.md), and writes
// names.txt beside the archive saying what each new name was in the source game. Nothing here
// depends on it; it exists so that a log line can say "v_veg_fern_balcn~D50 (veg_fern_balcny_kb2)"
// rather than a bare id, which is what made the docks freeze take so long to find.
void LoadNameMap(RegionState& rs) {
    if (rs.region->namesFile.empty()) return;
    for (const std::string& line : ReadLines(rs.region->namesFile)) {
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        size_t tab2 = line.find('\t', tab + 1);
        std::string fresh = line.substr(0, tab);
        std::string original = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
        // names.txt also records the region's renamed GXT keys, under a ".gxt" extension. Those
        // live in the text table's namespace and not the streamer's - a key and a model name that
        // read alike are unrelated - so they are no use here and would only pollute the map the
        // LOD pairing below looks originals up in.
        if (fresh.size() > 4 && Lower(fresh.substr(fresh.size() - 4)) == ".gxt") continue;
        size_t dot = fresh.find_last_of('.');
        if (dot != std::string::npos) fresh = fresh.substr(0, dot);
        dot = original.find_last_of('.');
        if (dot != std::string::npos) original = original.substr(0, dot);
        rs.originals[Lower(fresh)] = original;
    }
    if (!rs.originals.empty())
        Log::Write("[%s] names: %u renamed entr%s known from %s", rs.region->name.c_str(),
                   (unsigned)rs.originals.size(), rs.originals.size() == 1 ? "y" : "ies",
                   rs.region->namesFile.c_str());
}

// "18779 (v_veg_fern_balcn~D50 = veg_fern_balcny_kb2)", or as much of that as is known.
std::string DescribeModel(const RegionState& rs, int id) {
    std::string s = std::to_string(id);
    auto n = rs.names.find(id);
    if (n == rs.names.end()) return s;
    s += " (" + n->second;
    auto o = rs.originals.find(Lower(n->second));
    if (o != rs.originals.end() && Lower(o->second) != Lower(n->second)) s += " = " + o->second;
    return s + ")";
}

// `pedFile` is true only for a file the region named under PedIde - the output of
// tools/convert_peds.py. A `peds` section in a map IDE stays skipped: the map import does not
// namespace the ped columns (docs/asset-namespacing.md), so those lines name the donor's own
// `wmyst` and `bfyst` and would collide with San Andreas' peds of the same name.
std::string TranslateIde(RegionState& rs, const std::string& source, bool pedFile) {
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
            // weap lines only travel when the region asked for its guns: they cost a weapon model
            // info each, and a region that is not replacing any weapon has nothing to point at them.
            else if (head == "weap")
                section = RegionWeapons::Active() && !rs.region->weaponFile.empty()
                        ? IdeSection::Weap : IdeSection::Skip;
            else if (head == "peds")
                section = pedFile ? IdeSection::Peds : IdeSection::Skip;
            else if (head == "cars" || head == "hier" || head == "path" || head == "2dfx")
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

        // A ped is checked before it is offered, and a refusal is one ped rather than the region:
        // everything LoadPedObject would do with a line it cannot read is a silent wrong answer -
        // stack garbage hashed into a model key, or an animation group index one past the end of
        // the table - so the line never reaches it.
        if (section == IdeSection::Peds) {
            std::vector<std::string> col = Tokens(line);
            std::string name = col.size() > 1 ? col[1] : std::string("?");
            const char* refusal = nullptr;
            if ((int)col.size() < kPedIdeColumns) { rs.pedsShortLine++; refusal = "short line"; }
            else if (rs.pedSourceIds.count((int)oldId) || rs.pedNames.count(Lower(col[1]))) {
                rs.pedsDuplicate++;
                refusal = "a ped line of this region already has that id or name";
            }
            else if (NameAlreadyTaken(col[1])) { rs.pedsNameTaken++; refusal = "name already used"; }
            else if (!PedTypeKnown(col[3])) { rs.pedsBadPedType++; refusal = "no such ped type"; }
            else if (!AnimGroupDeclared(col[5])) { rs.pedsBadAnimGroup++; refusal = "no such animation group"; }
            if (refusal) {
                if (rs.pedsRefusedNames.size() < 400)
                    rs.pedsRefusedNames += (rs.pedsRefusedNames.empty() ? "" : ", ") +
                                           name + " (" + refusal + ")";
                continue;
            }
            if (budget.ped <= 0) { rs.pedsNoStore++; continue; }
            int newId = AllocateModelId();
            if (newId < 0) { rs.pedsNoId++; continue; }
            rs.pedIds[(int)oldId] = newId;
            rs.pedSourceIds.insert((int)oldId);
            rs.pedNames.insert(Lower(col[1]));
            rs.names[newId] = col[1];
            budget.ped--;
            out += std::to_string(newId) + line.substr(idEnd) + "\n";
            rs.pedsAdded++;
            continue;
        }

        int* pool = nullptr;
        if (section == IdeSection::Tobj) pool = &budget.time;
        else if (section == IdeSection::Anim) pool = &budget.clump;
        else if (section == IdeSection::Weap) pool = &budget.weapon;
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
        {
            size_t nameStart = line.find_first_not_of(" \t,", idEnd);
            size_t nameEnd = nameStart == std::string::npos ? nameStart : line.find_first_of(" \t,", nameStart);
            if (nameStart != std::string::npos)
                rs.names[newId] = line.substr(nameStart, nameEnd == std::string::npos ? std::string::npos : nameEnd - nameStart);
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
    bool hadLodColumn;   // an SA-format line, whose lod is the file's own and never guessed at
};

struct IplStats {
    int unmappedModel = 0;
    int noArchiveEntry = 0;   // defined by an IDE, but bound to nothing in any archive
    std::string noArchiveNames;   // the first few of those, for the log
    int malformed = 0;
    int lodLost = 0;
    int lodPaired = 0;    // HD lines given a LOD by the source game's naming rule
    int lodOrphans = 0;   // LOD lines no HD in the same file claimed
    int lodDropped = 0;   // orphan and whole-district LODs left out, having nothing to hide them
    int districtLods = 0; // of those, the island-sized ones
    int scaled = 0;
    int interior = 0;
    int badInterior = 0;
    std::map<unsigned, int> areas;   // area code -> how many lines carry it
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
    // The interior column is San Andreas' area code, and Vice City's is the same thing: III, VC and
    // SA all keep it in one byte on the entity (CFileLoader::LoadObjectInstance, 0x538353, stores
    // only the low byte at CEntity + 0x2F) and all three read 13 as "everywhere". So the number
    // carries across as it stands - VC's 4 really is the North Point Mall in San Andreas too.
    // What does not carry across is a value that will not fit: Vice City's club.ipl writes -1 for
    // club_exterior07, which through strtoul would be written back out as 4294967295 for SA's
    // "%d %s %d ..." to choke on. Anything outside a byte becomes 0, the outside world, which is
    // what a shell piece like that wants to be anyway.
    long interior = t.size() == 12 ? 0 : strtol(t[2].c_str(), nullptr, 10);
    if (interior < 0 || interior > 255) {
        stats.badInterior++;
        interior = 0;
    }
    inst.interior = (unsigned)interior;
    if (t.size() != 12) stats.areas[inst.interior]++;
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
    inst.hadLodColumn = t.size() == 11;
    inst.lod = inst.hadLodColumn ? atoi(t[10].c_str()) : -1;

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
        // A model defined by the IDEs but bound to nothing in any archive must not be placed. San
        // Andreas never checks: the first time the streamer is asked for it, it builds a read of
        // size zero from whatever the entry holds and retries it for ever, and the game freezes -
        // no crash, no exception, nothing in the log. Vice City's veg_fern_balcny_kb2 at the docks
        // did exactly that when the importer cut its name to the same 19 characters as _kb1's.
        if (!ModelHasArchiveEntry(mapped->second)) {
            if (stats.noArchiveEntry < 8)
                stats.noArchiveNames += (stats.noArchiveNames.empty() ? "" : ", ") + DescribeModel(rs, mapped->second);
            stats.noArchiveEntry++;
            newIndex.push_back(-1);
            continue;
        }
        inst.model = mapped->second;
        inst.x += rs.region->originX;
        inst.y += rs.region->originY;
        inst.z += rs.region->originZ;
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

    // III and VC carry no LOD column. There, a LOD is a separate model whose name matches its HD
    // model's from the fourth character on - LODhotel01 for dt_hotel01 - and the engine hid it
    // whenever the camera was inside the HD's draw distance (CSimpleModelInfo::FindRelatedModel).
    // San Andreas links by index and suppresses a LOD while any of its children draw; placed as
    // two unrelated objects, both draw at once and the LOD's textures show through the building.
    //
    // So rebuild the link the way SA wants it. Measured over Vice City's 9,114 placements: 1,099
    // of its 1,111 LODs have their HD in the same file, 1,066 of those at the identical position,
    // and one is in another file - the rule reconstructs nearly every link there is. The nearest
    // match by position settles a model placed more than once. The convention lives in the source
    // game's names; the region's are tagged, so names.txt supplies the originals.
    bool needsPairing = Settings::world.linkRegionLods;
    if (needsPairing) {
        needsPairing = false;
        for (const Inst& inst : kept)
            if (!inst.hadLodColumn) { needsPairing = true; break; }
    }
    if (needsPairing) {
        auto original = [&](const std::string& name) {
            auto o = rs.originals.find(Lower(name));
            return Lower(o != rs.originals.end() ? o->second : name);
        };
        std::map<std::string, std::vector<size_t>> lodsByKey;
        for (size_t i = 0; i < kept.size(); i++) {
            std::string o = original(kept[i].name);
            if (o.size() > 3 && o.compare(0, 3, "lod") == 0) lodsByKey[o.substr(3)].push_back(i);
        }
        std::vector<bool> claimed(kept.size(), false);
        for (size_t i = 0; i < kept.size() && !lodsByKey.empty(); i++) {
            Inst& inst = kept[i];
            if (inst.hadLodColumn || inst.lod >= 0) continue;
            std::string o = original(inst.name);
            if (o.size() <= 3 || o.compare(0, 3, "lod") == 0) continue;
            auto group = lodsByKey.find(o.substr(3));
            if (group == lodsByKey.end()) continue;
            size_t best = group->second.front();
            float bestDistance = 1e30f;
            for (size_t j : group->second) {
                float dx = kept[j].x - inst.x, dy = kept[j].y - inst.y;
                if (dx * dx + dy * dy < bestDistance) { bestDistance = dx * dx + dy * dy; best = j; }
            }
            inst.lod = (int)best;
            claimed[best] = true;
            stats.lodPaired++;
        }
        for (const auto& group : lodsByKey)
            for (size_t j : group.second)
                if (!claimed[j]) stats.lodOrphans++;

        // Leave out the LODs nothing will ever hide.
        //
        // III and Vice City load a district at a time - the loading screen between them is the swap
        // - and while you are in one district the others are not in memory. What stands in for them
        // is a LOD: a per-object one whose HD lives in a file that is not loaded, or one of the
        // island-sized meshes in islandsf.ipl and overview.ipl (IslandLODmainland, IslandLODcomIND
        // and their kin, every one of them drawn out to 3000 units) that stands in for a whole
        // district at once. San Andreas has no districts and streams the entire map, so the real
        // geometry those LODs deputise for is always there, and both draw together: the low-poly
        // stand-in sits inside and through the buildings it was meant to replace, which is the
        // overlapping, low-resolution mess this leaves behind.
        //
        // A LOD an HD claimed above is fine - SA hides it while the HD draws. The ones dropped here
        // are the rest: 14 orphans in Vice City and 213 in Liberty City, plus 4 and 5 island meshes.
        // Nothing is lost with them, because what they were standing in for is loaded.
        if (Settings::world.dropUnlinkedLods) {
            std::vector<bool> drop(kept.size(), false);
            for (size_t i = 0; i < kept.size(); i++) {
                const std::string o = original(kept[i].name);
                if (o.compare(0, 9, "islandlod") == 0) {
                    drop[i] = true;
                    stats.districtLods++;
                } else if (o.size() > 3 && o.compare(0, 3, "lod") == 0 && !claimed[i]) {
                    drop[i] = true;
                }
            }
            std::vector<int> moved(kept.size(), -1);
            std::vector<Inst> survivors;
            survivors.reserve(kept.size());
            for (size_t i = 0; i < kept.size(); i++) {
                if (drop[i]) { stats.lodDropped++; continue; }
                moved[i] = (int)survivors.size();
                survivors.push_back(kept[i]);
            }
            // Every LOD an HD points at survived, so no link is broken by this; the indices behind
            // the dropped lines still have to come down to match.
            for (Inst& inst : survivors)
                if (inst.lod >= 0) inst.lod = moved[inst.lod];
            kept.swap(survivors);
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

// PedImg is normally empty, and that used to be a hard constraint rather than a preference: San
// Andreas has room for EIGHT archives in all - CStreaming::ms_files is 8 x 0x30 at 0x8E48D8 - and
// it has spent five of them before this runs (gta3, gta_int, carrec, script, cutscene) with
// player.img still to come in CGame::Init2. Two imported cities took it to seven, there was no
// eighth for a ped archive, and a third city could not have registered a map archive either. So
// tools/convert_peds.py puts its models into the region's own archive.
//
// ArchiveSlots now moves the table to 32 entries, so there are 26 free rather than two, and PedImg
// is simply an option again. The converter still defaults to one archive a region because one
// archive is one directory scan and one open file handle, not because there is nowhere to put a
// second.
//
// Where it is used, the ped archive goes in BEFORE the map archive, and the order is load-bearing
// rather than tidy. The map import used to pack every .dff the donor shipped, pedestrians included,
// under the same namespaced name the converter gives its own - `v_wmyst.dff` in both - with the
// skin plugin's RenderWare version restamped but its 3.3 layout left alone. CStreaming::
// LoadCdDirectory binds an archive entry to a model info by name and, at 0x5B6449, asks whether
// that model already has a position; if it has, the later entry is dropped. FIRST archive wins.
// (Inside one archive it is the first directory entry, which is why convert_peds.py replaces by
// name rather than appending.) ReportPedAssets checks the outcome rather than assuming it.
void RegisterOneArchiveList(RegionState& rs, const std::vector<std::string>& paths,
                            const char* what, unsigned& used) {
    for (const std::string& path : paths) {
        if (!FileExists(path)) {
            Log::Write("[%s] %s: %s not found", rs.region->name.c_str(), what, path.c_str());
            continue;
        }
        // CClothes::Init registers player.img later, in CGame::Init2, so one slot has to be
        // left for it - it would otherwise get archive 0 back and stream clothes from gta3.img.
        const unsigned slots = (unsigned)ArchiveSlots::Count();
        if (used + 1 >= slots) {
            Log::Write("[%s] %s: %s NOT registered - CStreaming::ms_files is full (%u of %u "
                       "archives used, one reserved for player.img)",
                       rs.region->name.c_str(), what, path.c_str(), used, slots);
            continue;
        }
        std::string shortPath = GameRelativePath(path);
        if (shortPath.size() > kArchivePathMax) {
            Log::Write("[%s] %s: %s NOT registered - CStreaming::ms_files keeps only %u "
                       "characters of an archive path and this one needs %u; put the archive "
                       "closer to the game directory or give it a shorter name",
                       rs.region->name.c_str(), what, path.c_str(), (unsigned)kArchivePathMax,
                       (unsigned)shortPath.size());
            continue;
        }
        unsigned slot = CallAddImageToList(shortPath.c_str(), true);
        used++;
        Log::Write("[%s] %s: %s registered as archive %u (%u/%u slots used)",
                   rs.region->name.c_str(), what, shortPath.c_str(), slot, used, slots);
    }
}

void RegisterArchives() {
    unsigned used = (unsigned)ArchiveSlots::Used();

    for (RegionState& rs : Regions()) {
        RegisterOneArchiveList(rs, rs.region->pedImgFiles, "pedimg", used);
        RegisterOneArchiveList(rs, rs.region->imgFiles, "img", used);
    }
}

// Rewrite one IDE into the cache and hand the cached copy to the game's own parser.
void LoadOneIde(RegionState& rs, const std::string& source, bool pedFile) {
    std::string text = TranslateIde(rs, source, pedFile);
    if (text.empty()) return;

    std::string cached = CacheDir() + "\\" + SafeName(rs.region->name) + "_" +
                         SafeName(BaseName(source)) + ".ide";
    if (!WriteWholeFile(cached, text)) {
        Log::Write("[%s] ide: cannot write %s", rs.region->name.c_str(), cached.c_str());
        return;
    }
    CallLoadObjectTypes(cached.c_str());
}

void LoadModelDefinitions() {
    gNextFreeId = Settings::world.freeModelIdStart;
    int available = FreeIdsLeft();
    int gaps = FreeIdsInGaps();
    StoreBudget budget = CurrentBudget();
    Log::Write("models: %d free id(s) in %d..%d, and %d more in the gaps San Andreas' own IDEs "
               "leave in %d..%d - %d in all; store headroom atomic %d, damage %d, time %d, "
               "clump %d, weapon %d, ped %d",
               available, Settings::world.freeModelIdStart, FreeIdEnd(), gaps,
               GapFloor() < 0 ? 0 : GapFloor(), GapCeiling() - 1, available + gaps,
               budget.atomic, budget.damage, budget.time, budget.clump, budget.weapon, budget.ped);

    for (RegionState& rs : Regions()) {
        LoadNameMap(rs);
        for (const std::string& source : rs.region->ideFiles)
            LoadOneIde(rs, source, false);

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
            Log::Write("[%s] models: the free id range %d..%d ran out, and so did the %d..%d gaps "
                       "behind it - there is no more id space. ModelIdLimit is already at or near "
                       "%d, which is every id San Andreas' int16 streaming links can follow; see "
                       "docs/region-limits.md", rs.region->name.c_str(),
                       Settings::world.freeModelIdStart, FreeIdEnd(),
                       GapFloor() < 0 ? 0 : GapFloor(), GapCeiling() - 1, ModelLimits::ModelCount());
        if (rs.droppedNoStore)
            Log::Write("[%s] models: a model-info store ran out (atomic %d, damage %d, time %d, clump %d, "
                       "weapon %d in total; tobj lines need the time store, anim lines the clump store, "
                       "and weap lines the weapon store, which vanilla SA fills exactly) - this is "
                       "the hard wall, not the id range",
                       rs.region->name.c_str(), AtomicLimit(), kDamageStoreMax,
                       ModelLimits::TimeCapacity(), kClumpStoreMax, RegionWeapons::Capacity());
    }

    // The pedestrians come after every region's map models, and the order is a save-compatibility
    // decision rather than a convenience. AllocateModelId hands out ids in load order, so anything
    // registered BEFORE a region's buildings would renumber them - and a savegame stores model ids
    // inside its entities and a byte per model slot in its Streaming block (docs/save-compatibility.md).
    // Adding or removing a region's peds this way moves nothing that was already assigned; it only
    // appends. Removing a whole region still moves everything after it, which is the ledger's job
    // and not fixed here.
    // Both name checks below need the game's own tables. The animation definitions are the one that
    // could plausibly not be up - Animation::Initialise fills ms_numAnimAssocDefinitions - and if
    // they are not, every ped is refused for a reason that is not its own, so say which it is.
    bool anyPeds = false;
    for (RegionState& rs : Regions()) anyPeds = anyPeds || !rs.region->pedIdeFiles.empty();
    if (anyPeds && (int)patch::GetUInt(kAnimGroupCount) <= 0)
        Log::Write("peds: CAnimManager has no animation groups yet, so every ped line will be "
                   "refused for an unknown group - this is the loader running earlier than "
                   "Animation::Initialise, not a fault in the region's peds.ide");

    for (RegionState& rs : Regions()) {
        if (rs.region->pedIdeFiles.empty()) continue;
        for (const std::string& source : rs.region->pedIdeFiles)
            LoadOneIde(rs, source, true);

        // Same cleanup as the map models: a line the vanilla parser refused leaves nothing behind,
        // and a ped id with no model info would be handed to CPopulation::AddPed by whatever names
        // it in pedgrp.dat.
        int refusedByGame = 0;
        for (auto it = rs.pedIds.begin(); it != rs.pedIds.end();) {
            if (ModelInfoAt(it->second)) { ++it; continue; }
            it = rs.pedIds.erase(it);
            refusedByGame++;
            rs.pedsAdded--;
        }

        Log::Write("[%s] peds: %d model(s) registered, %d refused here (%d short line, %d duplicate, "
                   "%d name already used, %d unknown ped type, %d unknown animation group), %d "
                   "dropped (ped model store full), %d dropped (no free id), %d rejected by the "
                   "game's own IDE parser",
                   rs.region->name.c_str(), rs.pedsAdded,
                   rs.pedsShortLine + rs.pedsDuplicate + rs.pedsNameTaken + rs.pedsBadPedType +
                       rs.pedsBadAnimGroup,
                   rs.pedsShortLine, rs.pedsDuplicate, rs.pedsNameTaken, rs.pedsBadPedType,
                   rs.pedsBadAnimGroup, rs.pedsNoStore, rs.pedsNoId, refusedByGame);
        if (!rs.pedsRefusedNames.empty())
            Log::Write("[%s] peds: refused - %s", rs.region->name.c_str(), rs.pedsRefusedNames.c_str());
        if (rs.pedsNoStore)
            Log::Write("[%s] peds: the ped model info store is full at %d entries (San Andreas' own "
                       "data/peds.ide takes 276 of the vanilla 278) - raise ModelIdLimit above %d "
                       "so ModelLimits moves it to the heap",
                       rs.region->name.c_str(), ModelLimits::PedCapacity(), 20000);
    }

    if (gGapTaken)
        Log::Write("models: the free id range ran out, so %d id(s) came from the gaps San Andreas' "
                   "own IDEs leave below it, between %d and %d; %d such id(s) are still free. These "
                   "are ids no IDE line of San Andreas' declares, so nothing of its own names them - "
                   "but a mod that adds models at ids of its own choosing after this plugin loads "
                   "could collide with them, and FreeModelIdGapStart = -1 turns this off",
                   gGapTaken, gGapFirst, gGapLast, FreeIdsInGaps());
}

// A .col archive is a chain of chunks: 4cc, size, then 24 bytes of name and model id, with `size`
// counted from after the 4cc/size pair. Neither game loader checks the 4cc or the size, and the
// zero padding an IMG extractor leaves at the end of a file would send them off the rails - so walk
// the chunks here and hand the game only the bytes that hold sound chunks.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(total > 0 ? (size_t)total : 0);
    bool read = data.empty() || fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return read;
}

// The length of the leading run of well-formed chunks, with their number in `models` and whether
// any of them is a COL4.
size_t ColChunkRun(const std::vector<uint8_t>& data, int& models, bool& anyCol4) {
    models = 0;
    anyCol4 = false;
    size_t good = 0;
    while (good + 32 <= data.size()) {
        const uint8_t* header = data.data() + good;
        // Version 1 (III/VC and SA's own generic.col) is "COLL"; later ones "COL2".."COL4".
        if (memcmp(header, "COL", 3) != 0 || !(header[3] == 'L' || (header[3] >= '2' && header[3] <= '4'))) break;
        unsigned size = *reinterpret_cast<const unsigned*>(header + 4);
        if (size < 24 || good + 8 + size > data.size()) break;
        if (header[3] == '4') anyCol4 = true;
        good += 8 + size;
        models++;
    }
    return good;
}

// Returns the usable length (0 if nothing is), the file's bytes and how many models they hold.
size_t ReadColFile(const RegionState& rs, const std::string& path, std::vector<uint8_t>& data, int& models) {
    if (!ReadWholeFile(path, data)) return 0;

    bool anyCol4 = false;
    size_t good = ColChunkRun(data, models, anyCol4);
    if (anyCol4)
        Log::Write("[%s] col: %s contains COL4 data, which this loader path does not read",
                   rs.region->name.c_str(), path.c_str());

    if (models == 0) {
        Log::Write("[%s] col: %s holds no readable collision models", rs.region->name.c_str(), path.c_str());
        return 0;
    }
    if (good != data.size())
        Log::Write("[%s] col: %s has %u byte(s) of trailing data after its last collision model; ignored",
                   rs.region->name.c_str(), path.c_str(), (unsigned)(data.size() - good));
    return good;
}

// CFileLoader::LoadCollisionFileFirstTime(data, size, colSlot) is the loader SA itself uses for the
// col chunks of an archive: it looks each model up by the header's id and name, allocates a CColModel
// for one that has none yet, and records the model in the col slot. The by-path variant
// (LoadCollisionFile, 0x5B4E60) allocates that CColModel into a stack slot and then hands the
// register it never updated to LoadCollisionModel - fine for SA, whose models all have one by the
// time it runs, a wild write for ours.
typedef void(__cdecl* LoadCollisionDataFn)(const uint8_t*, unsigned, unsigned char);
const LoadCollisionDataFn CallLoadCollisionData = reinterpret_cast<LoadCollisionDataFn>(0x5B5000);

// Reads every region col file once, at install time, only to count its chunks: CPools::Initialise
// runs long before Init2, so the pool has to be sized before a single one of them is loaded.
int RegionColModelCount() {
    std::vector<uint8_t> data;
    int total = 0;
    for (RegionState& rs : Regions()) {
        for (const std::string& path : rs.region->colFiles) {
            if (!ReadWholeFile(path, data)) continue;
            int models = 0;
            bool anyCol4 = false;
            ColChunkRun(data, models, anyCol4);
            total += models;
        }
    }
    return total;
}

// Counts inst lines the same way CollectInsts walks them, without needing the model ids it has not
// assigned yet. An upper bound: lines whose model no IDE defines are counted here and dropped there.
int RegionInstLineCount() {
    int total = 0;
    for (RegionState& rs : Regions()) {
        for (const std::string& source : rs.region->iplFiles) {
            bool inInst = false;
            for (const std::string& raw : ReadLines(source)) {
                std::string line = Normalise(raw);
                if (line.empty() || line[0] == '#') continue;
                std::string head = SectionOf(line);
                if (!inInst) {
                    inInst = head == "inst";
                } else if (head == "end") {
                    inInst = false;
                } else {
                    total++;
                }
            }
        }
    }
    return total;
}

void RaiseEntityPools() {
    int building = (int)patch::GetUInt(kBuildingPoolSizeOperand);
    int dummy = (int)patch::GetUInt(kDummyPoolSizeOperand);
    int lines = RegionInstLineCount();
    if (!lines) return;
    if (building != kVanillaBuildingPool || dummy != kVanillaDummyPool)
        Log::Write("inst: the entity pools are already sized %d/%d rather than the vanilla %d/%d - "
                   "another limit adjuster got here first",
                   building, dummy, kVanillaBuildingPool, kVanillaDummyPool);

    int wantBuilding = Settings::world.buildingPoolLimit;
    int wantDummy = Settings::world.dummyPoolLimit;
    if (wantBuilding < 0) wantBuilding = building + lines + kEntityPoolMargin;
    if (wantDummy < 0) wantDummy = dummy + lines + kEntityPoolMargin;

    if (wantBuilding > building) patch::SetUInt(kBuildingPoolSizeOperand, (unsigned)wantBuilding);
    if (wantDummy > dummy) patch::SetUInt(kDummyPoolSizeOperand, (unsigned)wantDummy);
    Log::Write("inst: building pool %d -> %d, dummy pool %d -> %d entries (%d inst line(s) in the "
               "region IPLs)", building, wantBuilding > building ? wantBuilding : building,
               dummy, wantDummy > dummy ? wantDummy : dummy, lines);

    int nodes = (int)patch::GetUInt(kPtrNodePoolSizeOperand);
    int wantNodes = Settings::world.ptrNodePoolLimit;
    if (wantNodes < 0) wantNodes = nodes + lines * kPtrNodesPerInstLine + kPtrNodePoolMargin;
    if (wantNodes > nodes) {
        patch::SetUInt(kPtrNodePoolSizeOperand, (unsigned)wantNodes);
        Log::Write("inst: sector pointer-node pool %d -> %d entries (a building takes one per "
                   "50-unit sector it covers)", nodes, wantNodes);
    }

    // And the other pointer-node pool, which is a different pool entirely.
    //
    // A building is filed into its sectors through CPtrListSingleLink, out of the 70000-entry pool
    // raised above. Anything that can move - and a dummy object promoted to a real CObject can -
    // is filed through CPtrListDoubleLink instead, out of a pool of 3200. San Andreas sizes that
    // for its own handful of dynamic props, and until the regions had object data none of their
    // models qualified, so nothing here ever touched it.
    //
    // Importing object.dat changed that: it is what gives a model a row in ms_aObjectInfo, and a
    // model with a row is loaded as a dummy object rather than a building. 4672 of the two regions'
    // placements now qualify - 2336 in each, as it happens - against a pool of 3200 shared with the
    // whole of San Andreas. CPtrListDoubleLink::AddItem does not check whether the pool gave it
    // anything, so the first placement past the end wrote through a null node and took the game
    // down during the DAT load screen, before anything was on screen to see.
    //
    // Sized off the inst-line count like the pool above rather than off how many lines have object
    // data, which is not known this early - CPools::Initialise runs long before any region file is
    // read. That over-allocates, since only the dynamic ones draw on this pool, but a node is small
    // and running out is fatal where a spare megabyte is not.
    int dnodes = (int)patch::GetUInt(kPtrNodeDoubleLinkPoolSizeOperand);
    if (dnodes != kVanillaPtrNodeDoubleLinkPool) {
        // Not the push this expects, so the address is wrong or something else has already moved
        // it. Leave it alone rather than writing a size into whatever is actually there.
        Log::Write("inst: the double-link pointer-node pool size reads %d, not the expected %d; "
                   "leaving it alone (dynamic objects may run out of sector nodes)",
                   dnodes, kVanillaPtrNodeDoubleLinkPool);
        return;
    }
    int wantDnodes = Settings::world.ptrNodePoolLimit;
    if (wantDnodes < 0) wantDnodes = dnodes + lines * kPtrNodesPerInstLine + kPtrNodePoolMargin;
    if (wantDnodes > dnodes) {
        patch::SetUInt(kPtrNodeDoubleLinkPoolSizeOperand, (unsigned)wantDnodes);
        Log::Write("inst: double-link pointer-node pool %d -> %d entries (every object.dat model "
                   "is placed as a dynamic object and files itself through this one)",
                   dnodes, wantDnodes);
    }
}

// One entity-index array per CFileLoader::LoadScene call, so one per cached file we are about to
// write: a region's IPL list, plus a file for every further kMaxInstPerFile lines it splits into.
int RegionLoadSceneCount() {
    int lines = RegionInstLineCount();
    int files = 0;
    for (RegionState& rs : Regions()) files += (int)rs.region->iplFiles.size();
    return files + lines / (int)kMaxInstPerFile + 1;
}

void MoveIplEntityArrays() {
    int wanted = kVanillaIplEntityArrays + RegionLoadSceneCount() + kIplEntityArrayMargin;
    for (uintptr_t site : kIplEntityArraySites) {
        if (patch::GetUInt(site) == kIplEntityArrays) continue;
        Log::Write("ipl store: the entity-array table at %08X is not where it should be (site %08X "
                   "holds %08X); left alone, and a region's IPLs will overrun it",
                   kIplEntityArrays, (unsigned)site, patch::GetUInt(site));
        return;
    }

    void* table = calloc((size_t)wanted, sizeof(void*));
    if (!table) {
        Log::Write("ipl store: cannot allocate %d entity-array slots; left alone", wanted);
        return;
    }
    for (uintptr_t site : kIplEntityArraySites) patch::SetUInt(site, (unsigned)(uintptr_t)table);
    gIplEntityArrayCapacity = wanted;
    Log::Write("ipl store: entity-array table %d -> %d slots at %08X (%d of ours, %d for San "
               "Andreas' own IPLs, %d spare) - the vanilla table has CIplStore's pool right behind "
               "it and no bounds check in front of it",
               kVanillaIplEntityArrays, wanted, (unsigned)(uintptr_t)table, RegionLoadSceneCount(),
               kVanillaIplEntityArrays, kIplEntityArrayMargin);
}

// The count is still unchecked, so say so if a region ever gets near the new end.
void ReportIplEntityArrayUse() {
    int used = (int)*reinterpret_cast<unsigned*>(kIplEntityArrayCount);
    if (used > gIplEntityArrayCapacity - kIplEntityArrayMargin / 2)
        Log::Write("ipl store: %d of %d entity-array slots used - close enough to the end to be "
                   "worth raising", used, gIplEntityArrayCapacity);
}

void RaiseColModelPool() {
    int current = (int)patch::GetUInt(kColModelPoolSizeOperand);
    if (current != kVanillaColModelPool)
        Log::Write("col: the col model pool is already sized %d rather than the vanilla %d - "
                   "another limit adjuster got here first", current, kVanillaColModelPool);

    int wanted = Settings::world.colModelPoolLimit;
    int needed = RegionColModelCount();
    if (wanted < 0) wanted = current + needed + kColModelPoolMargin;
    if (wanted <= current) {
        if (needed)
            Log::Write("col: model pool left at %d entries; the region collision needs %d and the "
                       "configured limit is %d", current, needed, wanted);
        return;
    }

    patch::SetUInt(kColModelPoolSizeOperand, (unsigned)wanted);
    Log::Write("col: model pool %d -> %d entries (%d collision model(s) in the region col files, "
               "%d spare)", current, wanted, needed, wanted - current - needed);
}

// Distinct dictionary names the regions' IDEs bind models to - every one becomes a TxdDef.
int RegionTxdCount() {
    std::set<std::string> names;
    for (RegionState& rs : Regions()) {
        std::vector<std::string> sources = rs.region->ideFiles;
        // The ped dictionaries count too: LoadPedObject creates a TxdDef for every `peds` line's
        // txd column, and Vice City's 105 peds and Liberty City's 78 bring 184 more names.
        sources.insert(sources.end(), rs.region->pedIdeFiles.begin(), rs.region->pedIdeFiles.end());
        for (const std::string& source : sources) {
            IdeSection section = IdeSection::None;
            for (const std::string& raw : ReadLines(source)) {
                std::string line = Trim(raw);
                if (line.empty() || line[0] == '#') continue;
                std::string low = Lower(line);
                if (section == IdeSection::None) {
                    if (low == "objs") section = IdeSection::Objs;
                    else if (low == "tobj") section = IdeSection::Tobj;
                    else if (low == "anim") section = IdeSection::Anim;
                    else if (low == "weap" && RegionWeapons::Active() &&
                             !rs.region->weaponFile.empty()) section = IdeSection::Weap;
                    else if (low == "peds") section = IdeSection::Peds;
                    continue;
                }
                if (low == "end") { section = IdeSection::None; continue; }
                std::vector<std::string> t = Tokens(line);
                if (t.size() >= 3) names.insert(Lower(t[2]));
            }
        }
    }
    return (int)names.size();
}

void RaiseTxdPool() {
    int current = (int)patch::GetUInt(kTxdPoolSizeOperand);
    if (current != kVanillaTxdPool)
        Log::Write("txd: the dictionary pool is already sized %d rather than the vanilla %d - "
                   "another limit adjuster got here first", current, kVanillaTxdPool);

    int wanted = Settings::world.txdPoolLimit;
    int needed = RegionTxdCount();
    if (wanted < 0) wanted = current + needed + kTxdPoolMargin;
    if (wanted <= current) {
        if (needed)
            Log::Write("txd: dictionary pool left at %d entries; the regions need %d and the "
                       "configured limit is %d", current, needed, wanted);
        return;
    }

    patch::SetUInt(kTxdPoolSizeOperand, (unsigned)wanted);
    Log::Write("txd: dictionary pool %d -> %d entries (%d dictionary name(s) across the regions' "
               "IDEs, %d spare)", current, wanted, needed, wanted - current - needed);
}

void LoadCollisionArchives() {
    std::vector<uint8_t> data;
    for (RegionState& rs : Regions()) {
        for (const std::string& path : rs.region->colFiles) {
            if (!FileExists(path)) {
                Log::Write("[%s] col: %s not found", rs.region->name.c_str(), path.c_str());
                continue;
            }
            int models = 0;
            size_t bytes = ReadColFile(rs, path, data, models);
            if (!bytes) continue;
            CallLoadCollisionData(data.data(), (unsigned)bytes, kPermanentColSlot);
            Log::Write("[%s] col: %s loaded into col slot %u - %d collision model(s), permanently in "
                       "memory (the streamer never touches slot 0)",
                       rs.region->name.c_str(), path.c_str(), kPermanentColSlot, models);
        }
    }
}

void ReportMissingArchiveEntries() {
    for (RegionState& rs : Regions()) {
        int missing = 0;
        std::string which;
        for (const auto& pair : rs.ids) {
            if (ModelHasArchiveEntry(pair.second)) continue;
            if (missing < 20) which += (which.empty() ? "" : ", ") + DescribeModel(rs, pair.second);
            missing++;
        }
        if (missing)
            Log::Write("[%s] models: %d of %d have no .dff in any registered archive - their "
                       "placements are skipped, because a request for one hangs the streamer: %s%s",
                       rs.region->name.c_str(), missing, (int)rs.ids.size(), which.c_str(),
                       missing > 20 ? ", ..." : "");
    }
}

// A pedestrian needs two things out of an archive, not one: its .dff and the .txd its own IDE
// column names. CFileLoader::LoadPedObject calls CBaseModelInfo::SetTexDictionary (0x4C4B40), which
// creates a CTxdStore slot for that name whether or not anything ever fills it, and the slot's
// streaming id is the slot number plus the TXD base (LoadCdDirectory, 0x5B62CE `add eax, 0x4E20`).
// A ped whose dictionary never got an archive entry loads and walks with white skin, which reads as
// a conversion fault and is not one - so it is named here instead.
constexpr unsigned kTxdIndexOffset = 0xA;   // CBaseModelInfo::m_nTxdIndex, a u16

void ReportPedAssets() {
    for (RegionState& rs : Regions()) {
        if (rs.pedIds.empty()) continue;
        int noDff = 0, noTxd = 0;
        std::string which;
        for (const auto& pair : rs.pedIds) {
            uint8_t* mi = static_cast<uint8_t*>(ModelInfoAt(pair.second));
            bool dff = ModelHasArchiveEntry(pair.second);
            unsigned short txd = mi ? *reinterpret_cast<unsigned short*>(mi + kTxdIndexOffset) : 0xFFFF;
            bool tex = txd != 0xFFFF && ModelHasArchiveEntry(ModelLimits::TxdBase() + (int)txd);
            if (dff && tex) continue;
            if (!dff) noDff++;
            if (!tex) noTxd++;
            if (which.size() < 400)
                which += (which.empty() ? "" : ", ") + DescribeModel(rs, pair.second) +
                         (dff ? " (no .txd)" : tex ? " (no .dff)" : " (no .dff, no .txd)");
        }
        if (noDff || noTxd)
            Log::Write("[%s] peds: of %d registered, %d have no .dff and %d no .txd in any "
                       "archive - a request for one of those hangs the streamer, so check that the "
                       "region's PedImg is listed and was found: %s",
                       rs.region->name.c_str(), (int)rs.pedIds.size(), noDff, noTxd, which.c_str());
        else
            Log::Write("[%s] peds: all %d registered model(s) have both a .dff and a .txd bound",
                       rs.region->name.c_str(), (int)rs.pedIds.size());
    }
}

// CWorld::Add -> CEntity::GetBoundRect reads the model's CColModel without a null check. SA's
// buildings all have one by the time LoadScene runs: CFileLoader::LoadLevel calls
// CColStore::LoadAllCollision right after Init2, which loads every col file once (and unloads the
// data again, but the CColModel objects stay). A model in no col file at all - VC's LOD models,
// for instance - has none, so give those the placeholder box the game hands its peds
// (CTempColModels::ms_colModelBBox, from CFileLoader::LoadPedObject).
constexpr uintptr_t kColModelBBox = 0x968DF0;
constexpr uintptr_t kSetColModel = 0x4C4BC0;   // CBaseModelInfo::SetColModel(CColModel*, bool own), __thiscall

void GivePlaceholderCollision() {
    typedef void(__thiscall* SetColModelFn)(void*, void*, bool);
    for (RegionState& rs : Regions()) {
        int given = 0;
        for (const auto& pair : rs.ids) {
            uint8_t* mi = static_cast<uint8_t*>(ModelInfoAt(pair.second));
            if (!mi || *reinterpret_cast<void**>(mi + 0x14)) continue;
            reinterpret_cast<SetColModelFn>(kSetColModel)(mi, reinterpret_cast<void*>(kColModelBBox), false);
            given++;
        }
        if (given)
            Log::Write("[%s] col: %d model(s) appear in no collision file and got the placeholder box "
                       "(walk-through, like a LOD)", rs.region->name.c_str(), given);
    }
}

// ---------------------------------------------------------------- object data

uint8_t* gObjectInfo = nullptr;                          // the relocated array, or null
int gObjectInfoCapacity = kVanillaObjectInfoCount;

uintptr_t ObjectInfoBase() { return gObjectInfo ? (uintptr_t)gObjectInfo : kObjectInfo; }

// The rows a region's object.dat holds, without parsing them: enough to size the table before the
// game builds it. A row is anything that is not blank, a `;` comment or the `*` end marker, which
// is where CObjectData::Initialise stops reading (0x5B5486).
int RegionObjectDataRows() {
    int rows = 0;
    for (RegionState& rs : Regions()) {
        if (rs.region->objectDataFile.empty()) continue;
        for (const std::string& raw : ReadLines(rs.region->objectDataFile)) {
            std::string line = Normalise(raw);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '*') break;
            rows++;
        }
    }
    return rows;
}

// Move ms_aObjectInfo to a block big enough for San Andreas' own rows and the regions' on top.
// Nothing checks the table's length at run time - Initialise walks it with a stack local and
// appends without ever looking at a limit - so a bigger block is the whole of the change; what has
// to be right is that every operand naming the old one is rewritten, and that is verified first.
void RelocateObjectData() {
    int rows = RegionObjectDataRows();
    if (!rows) return;

    // De-duplication means the regions will need fewer entries than they have rows, but by how many
    // is not knowable until the names are matched, so budget one entry per row and a little over.
    int wanted = kVanillaObjectInfoCount + rows + kObjectInfoMargin;

    for (const ObjectInfoSite& s : kObjectInfoSites) {
        uint32_t have = (uint32_t)patch::GetUInt(s.address + s.operandOffset);
        if (have == s.oldValue) continue;
        Log::Write("ERROR: object data table not grown: 0x%X holds 0x%08X, expected 0x%08X "
                   "(another mod patched it); the regions' object.dat files are not loaded",
                   (unsigned)(s.address + s.operandOffset), have, s.oldValue);
        return;
    }

    uint8_t* block = (uint8_t*)calloc((size_t)wanted, kObjectInfoStride);
    if (!block) {
        Log::Write("ERROR: object data table not grown: cannot allocate %d entries", wanted);
        return;
    }
    for (const ObjectInfoSite& s : kObjectInfoSites)
        patch::SetUInt(s.address + s.operandOffset,
                       (unsigned)((uintptr_t)block + (s.oldValue - kObjectInfo)));

    gObjectInfo = block;
    gObjectInfoCapacity = wanted;
    Log::Write("object data: table %d -> %d entries at %p (%d row(s) across the regions' "
               "object.dat; vanilla San Andreas fills all 160 of its own)",
               kVanillaObjectInfoCount, wanted, (void*)block, rows);
}

// Append a region's object.dat to the table San Andreas has just built, and hang each row's index
// on its model. This has to be its own parser rather than a second call to CObjectData::Initialise:
// Initialise always starts writing at entry 5 with its count reset to 5, so calling it again would
// overwrite San Andreas' own rows underneath the models that already point at them. It has to run
// here, after CFileLoader::LoadLevel has finished, because Initialise is called from inside
// LoadLevel (0x5B925F) BEFORE CStreaming::Init2 (0x5B927D) - which is where a region's model infos
// are created - so at the moment San Andreas reads object.dat none of our models exist yet.
//
// The columns are San Andreas' own, and the importer has already mapped the source game's into
// them (tools/import_city.py). Unlike Initialise this appends every row instead of folding equal
// ones together: the table has room, and a shared entry silently loses the columns the fast path
// does not compare - an uproot limit, a break velocity - which is a thing to inherit, not to copy.
void LoadRegionObjectData() {
    bool any = false;
    for (RegionState& rs : Regions())
        if (!rs.region->objectDataFile.empty()) any = true;
    if (!any) return;

    uintptr_t base = ObjectInfoBase();
    int used = gObjectInfoCapacity;
    for (int i = kFirstParsedObjectInfo; i < gObjectInfoCapacity; i++) {
        const uint32_t* e = reinterpret_cast<const uint32_t*>(base + (unsigned)i * kObjectInfoStride);
        if (!e[0] && !e[1] && !e[2] && !e[3]) { used = i; break; }
    }
    Log::Write("object data: San Andreas' own DATA\\OBJECT.DAT filled %d of %d entries",
               used, gObjectInfoCapacity);

    const float buoyancyNumerator = *reinterpret_cast<const float*>(kBuoyancyNumerator);
    const float buoyancyScale = *reinterpret_cast<const float*>(kBuoyancyScale);

    for (RegionState& rs : Regions()) {
        if (rs.region->objectDataFile.empty()) continue;
        std::vector<std::string> lines = ReadLines(rs.region->objectDataFile);
        if (lines.empty()) {
            Log::Write("[%s] object data: %s is missing or empty - the region's props stay static "
                       "scenery", rs.region->name.c_str(), rs.region->objectDataFile.c_str());
            continue;
        }

        int added = 0, unknown = 0, malformed = 0, full = 0, effects = 0;
        std::string unknownNames;
        for (const std::string& raw : lines) {
            std::string line = Normalise(raw);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line[0] == '*') break;

            char name[64] = "", fx[64] = "";
            float mass = 0.0f, turnMass = 0.0f, airResistance = 0.0f, elasticity = 0.0f;
            float submerged = 0.0f, uprootLimit = 0.0f, damageMultiplier = 0.0f;
            int damageEffect = 0, colResponse = 0, cameraAvoid = 0, explodes = 0, fxType = 0;
            float fxX = 0.0f, fxY = 0.0f, fxZ = 0.0f;
            float smash = 0.0f, breakX = 0.0f, breakY = 0.0f, breakZ = 0.0f, breakRand = 0.0f;
            int gunBreak = 0, sparks = 0;
            // The same format string Initialise uses (0x868DC8). Like the game, a short line is
            // accepted and leaves the tail at zero; only the eleven columns every dialect has are
            // required, which is what the return value is checked against.
            int got = sscanf(line.c_str(),
                             "%63s %f %f %f %f %f %f %f %d %d %d %d %d %f %f %f %63s %f %f %f %f %f %d %d",
                             name, &mass, &turnMass, &airResistance, &elasticity, &submerged,
                             &uprootLimit, &damageMultiplier, &damageEffect, &colResponse,
                             &cameraAvoid, &explodes, &fxType, &fxX, &fxY, &fxZ, fx,
                             &smash, &breakX, &breakY, &breakZ, &breakRand, &gunBreak, &sparks);
            if (got < 11) { malformed++; continue; }

            void* mi = CallGetModelInfoByName(name, nullptr);
            if (!mi) {
                if (unknown < 8) unknownNames += (unknownNames.empty() ? "" : ", ") + std::string(name);
                unknown++;
                continue;
            }
            if (used >= gObjectInfoCapacity) { full++; continue; }
            if (fxType) { fxType = 0; effects++; }   // the FX system is looked up by name from a
                                                     // table that holds San Andreas' effects only

            uint8_t* e = reinterpret_cast<uint8_t*>(base + (unsigned)used * kObjectInfoStride);
            float* f = reinterpret_cast<float*>(e);
            f[0] = mass;
            f[1] = turnMass;
            f[2] = airResistance;
            f[3] = elasticity;
            // Percent submerged is not kept: the game stores only the buoyancy constant it becomes.
            f[4] = submerged != 0.0f ? (buoyancyNumerator / submerged) * mass * buoyancyScale : 0.0f;
            f[5] = uprootLimit;
            f[6] = damageMultiplier;
            e[0x1C] = (uint8_t)damageEffect;
            e[0x1D] = (uint8_t)colResponse;
            e[0x1E] = (uint8_t)cameraAvoid;
            e[0x1F] = (uint8_t)explodes;
            e[0x20] = (uint8_t)fxType;
            f[0x24 / 4] = fxX;
            f[0x28 / 4] = fxY;
            f[0x2C / 4] = fxZ;
            *reinterpret_cast<void**>(e + 0x30) = nullptr;   // FxSystemBP, only read when fxType > 0
            f[0x34 / 4] = smash;
            f[0x38 / 4] = breakX;
            f[0x3C / 4] = breakY;
            f[0x40 / 4] = breakZ;
            f[0x44 / 4] = breakRand;
            *reinterpret_cast<int*>(e + 0x48) = gunBreak;
            *reinterpret_cast<int*>(e + 0x4C) = sparks;

            *reinterpret_cast<int16_t*>(static_cast<uint8_t*>(mi) + kObjectInfoIndexOffset) = (int16_t)used;
            used++;
            added++;
        }

        Log::Write("[%s] object data: %d model(s) are dynamic objects now - breakable, pushable, "
                   "smashable - instead of buildings; table at %d of %d entries",
                   rs.region->name.c_str(), added, used, gObjectInfoCapacity);
        if (unknown)
            Log::Write("[%s] object data: %d line(s) name a model this region did not register: "
                       "%s%s", rs.region->name.c_str(), unknown, unknownNames.c_str(),
                       unknown > 8 ? ", ..." : "");
        if (malformed)
            Log::Write("[%s] object data: %d line(s) have fewer than the eleven columns every "
                       "dialect writes and were left out", rs.region->name.c_str(), malformed);
        if (effects)
            Log::Write("[%s] object data: %d line(s) asked for a particle effect by name; San "
                       "Andreas resolves those against its own effects only, so they were dropped "
                       "and the object still breaks, just without the puff",
                       rs.region->name.c_str(), effects);
        if (full)
            Log::Write("[%s] object data: %d line(s) dropped - the table is full at %d entries; "
                       "those models stay static scenery", rs.region->name.c_str(), full,
                       gObjectInfoCapacity);
    }
}

void PlaceInstances() {
    // Before the instances, not after: CFileLoader::LoadObjectInstance decides building or dynamic
    // object from the model's object-data index, and it reads it as it places each line.
    LoadRegionObjectData();
    GivePlaceholderCollision();
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
                {
                    int links = 0;
                    for (size_t i = first; i < first + count; i++)
                        if (insts[i].lod >= 0) links++;
                    Log::Write("[%s] ipl: loading %s - %u inst, %d LOD link(s)", rs.region->name.c_str(),
                               cached.c_str(), (unsigned)count, links);
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
        if (stats.lodPaired || stats.lodOrphans)
            Log::Write("[%s] inst: %d HD placement(s) linked to their LOD by the source game's naming "
                       "rule, so SA hides the LOD while the HD draws; %d LOD placement(s) had no HD to "
                       "claim them in their file",
                       rs.region->name.c_str(), stats.lodPaired, stats.lodOrphans);
        if (stats.lodDropped)
            Log::Write("[%s] inst: %d LOD placement(s) left out - %d of them island-sized stand-ins "
                       "for a whole district - having no HD to hide them and nothing left to stand "
                       "in for, since San Andreas keeps the real geometry loaded",
                       rs.region->name.c_str(), stats.lodDropped, stats.districtLods);
        if (stats.noArchiveEntry)
            Log::Write("[%s] inst: %d line(s) NOT placed - their model is defined by an IDE but has "
                       "no .dff in any registered archive, and asking the streamer for such a model "
                       "freezes the game: %s%s",
                       rs.region->name.c_str(), stats.noArchiveEntry, stats.noArchiveNames.c_str(),
                       stats.noArchiveEntry > 8 ? ", ..." : "");
        if (stats.scaled)
            Log::Write("[%s] inst: %d line(s) carried a non-unit scale, which San Andreas has no "
                       "field for - placed at scale 1", rs.region->name.c_str(), stats.scaled);
        if (refused)
            Log::Write("[%s] inst: %d line(s) dropped - the building pool has no room left; raise "
                       "BuildingPoolLimit", rs.region->name.c_str(), refused);
        // The interior column is San Andreas' own area code and needs no translation (see
        // ParseInst). Listing what came through is how you find out which areas a region has, and
        // therefore which Interior<N> boxes its ini wants - see Interiors.cpp.
        if (stats.interior) {
            std::string areas;
            char buffer[32];
            for (auto& a : stats.areas) {
                if (!a.first || a.first == 13u) continue;   // outside world, and "everywhere"
                snprintf(buffer, sizeof(buffer), "%s%u (%d)", areas.empty() ? "" : ", ", a.first, a.second);
                areas += buffer;
            }
            Log::Write("[%s] inst: %d line(s) carry an area code, which is San Andreas' own interior "
                       "mechanism and passes through unchanged: %s; %d line(s) are area 13 "
                       "(\"everywhere\", visible from every interior)",
                       rs.region->name.c_str(), stats.interior, areas.empty() ? "none" : areas.c_str(),
                       stats.areas.count(13u) ? stats.areas[13u] : 0);
        }
        if (stats.badInterior)
            Log::Write("[%s] inst: %d line(s) had an area code outside 0..255 (Vice City writes -1 "
                       "for one Malibu Club shell piece); placed in the outside world instead",
                       rs.region->name.c_str(), stats.badInterior);
    }
    ReportIplEntityArrayUse();
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
    ReportPedAssets();
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
            if (!r.imgFiles.empty() || !r.ideFiles.empty() || !r.colFiles.empty() ||
                !r.pedImgFiles.empty() || !r.pedIdeFiles.empty())
                Regions().push_back({ &r });
        if (Regions().empty()) return;

        // A server places its own objects and owns the model id space; the same INI serves both.
        if (Settings::RunningUnderSaMp()) {
            Log::Write("SA-MP: region Img/Ide/Col/PedImg/PedIde are left alone, the server places "
                       "the city and owns the ped model ids");
            Regions().clear();
            return;
        }

        CacheDir() = GAME_PATH("MapExpander\\cache");
        CreateDirectoryA(std::string(GAME_PATH("MapExpander")).c_str(), nullptr);
        if (!CreateDirectoryA(CacheDir().c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            Log::Write("city: cannot create %s - region assets not loaded", CacheDir().c_str());
            Regions().clear();
            return;
        }

        RaiseColModelPool();
        RaiseTxdPool();
        RelocateObjectData();
        RaiseEntityPools();
        MoveIplEntityArrays();

        Init2Original() = safetyhook::create_inline(reinterpret_cast<void*>(0x5B8AD0),
                                                    reinterpret_cast<void*>(&Hooked_StreamingInit2));
        LoadLevelOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(0x5B9030),
                                                       reinterpret_cast<void*>(&Hooked_LoadLevel));
        gPlacesOwnEntities = true;
        Log::Write("city: loading own assets for %u region(s), cache in %s",
                   (unsigned)Regions().size(), CacheDir().c_str());
    }

    bool PlacesOwnEntities() { return gPlacesOwnEntities; }
}
