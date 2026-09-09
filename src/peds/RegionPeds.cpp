// Puts people on the pavements of an imported city, and keeps them walking.
//
// San Andreas will not do it. Its own pedestrian population is generated on the path graph, and a
// path node position is stored as an int16 times 0.125, so a node can only describe a point within
// +/-4095.875. Vice City sits near San Andreas x 6000 and Liberty City near y 6000; neither fits,
// and widening that encoding means several hundred hand-verified instruction sites that cannot be
// checked without running the game. docs/known-limitations.md has the whole finding. What is NOT
// the problem, and was checked: population density is live wherever the player is, and there is no
// map-extent gate anywhere in the spawning code. It really is only the nodes.
//
// So the nodes are kept here instead. tools/import_city.py reads the source game's own pedestrian
// path graph - Vice City's is in the `path` sections of its IPLs, GTA III attaches a group to a
// road model and means it once per placement of that road - and writes it to the region's
// pednodes.dat in the region's own coordinates. This file holds that graph in ordinary memory,
// where nothing is compressed and nothing has a range, and walks pedestrians along it directly.
// The game's own path graph is never touched, so San Andreas' own streets behave exactly as they
// did.
//
// Nothing about San Andreas' population is patched, replaced or hooked out of the way. The one
// hook is a post hook on CPopulation::Update, which runs after the game has done everything it
// normally does; over San Andreas' map this module does nothing at all, because there is no region
// under the player.
//
// WHO walks there comes from the source game as well, because a crowd that is right for the city
// and wrong for the street is only half the job. Both donor games decide it the same way - numbered
// groups of ped models in data/pedgrp.dat, zone boxes, and the link between them set from their
// mission script, one group for daytime and one for night - and the importer writes the result to
// the region's pedgrp.dat with every donor ped paired with the San Andreas model standing in for
// it. Each node in pednodes.dat carries the district it stands in, so choosing who to put there is
// a lookup and not a search. When the donor's own ped models are imported one day, only the right
// hand side of those pairs changes.
//
// A ped is created by the game's own CPopulation::AddPed with makeWander false, which is exactly
// what it does for its own peds minus the wander task - and the wander task is the one thing that
// must not be used here, because CTaskComplexWander walks the path graph. In its place each ped is
// given CTaskComplexGoToPointAndStandStill towards a neighbouring node, and a new one whenever it
// finishes; that task steers, avoids and animates by itself and never asks CPathFind anything,
// which was confirmed by disassembling it. Everything else about the ped is the game's: it is
// marked created by the game, so CPopulation::ManagePopulation culls it like any other, it reacts
// to events, it can be knocked down, robbed and run over.
//
// Nothing here runs under SA-MP. The server owns the population there and peds that existed only
// on this client would be worse than no peds at all.
#include <plugin.h>
#include <safetyhook.hpp>
#include <CPopulation.h>
#include <CPools.h>
#include <CPedModelInfo.h>
#include <CModelInfo.h>
#include <CStreaming.h>
#include <CClock.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CWorld.h>
#include <CPedPlacement.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "RegionPeds.h"
#include "PathDraw.h"
#include "Ground.h"
#include "ModelLimits.h"
#include "StreamedModels.h"
#include "RegionPolice.h"
#include "RegionTraffic.h"
#include "RegionParkedCars.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// CPopulation::Update(bool generatePeds), once a frame from CGame::Process while a game is running.
constexpr uintptr_t kPopulationUpdate = 0x616650;

// The task that walks a ped to a point and stops there.
//
//   CTaskComplexGoToPointAndStandStill::CTaskComplexGoToPointAndStandStill(
//       int moveState, CVector const& target, float radius, float slowDownDistance,
//       bool alwaysStop, bool useHeading)     __thiscall, ret 0x18
//
// The address is not in the name table under that name - the table calls it "Constructor" - so it
// was read out of the exe: 0x668120 is the function immediately before
// CTaskComplexGoToPointAndStandStill::SelectMoveState, its callers pass exactly those six
// arguments, and 0x490822 allocates 0x28 bytes for it before calling it. Getting an allocation
// size wrong corrupts the heap, so the prologue is checked byte for byte before the task is ever
// built, and when it does not match the peds are placed and left standing rather than walked.
constexpr uintptr_t kGoToPointCtor = 0x668120;
constexpr unsigned kGoToPointSize = 0x28;
constexpr uintptr_t kTaskNew = 0x61A5A0;   // CTask::operator new
const uint8_t kGoToPointPrologue[] = { 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1, 0xE8 };

// The game's own defaults for the two floats, read from where its callers push them from
// (0x86FC84 and 0x86FC88): stop within half a metre, start slowing two metres out.
constexpr float kArriveRadius = 0.5f;
constexpr float kSlowDownDistance = 2.0f;
constexpr int kMoveStateWalk = 4;   // PEDMOVE_WALK

typedef void*(__thiscall* GoToPointCtorFn)(void* self, int moveState, const CVector* target,
                                           float radius, float slowDown, bool alwaysStop,
                                           bool useHeading);
typedef void*(__cdecl* TaskNewFn)(unsigned size);

bool gCanWalk = false;   // the task above verified and safe to build

// How the crowd is sized. San Andreas' popcycle gives a whole district somewhere between three and
// twenty-five people at once, so this is in the same country; the ini multiplier and the game's own
// population slider both scale it, so a player who has turned traffic down gets fewer here too.
constexpr int kBasePedCount = 16;
constexpr int kMaxPedCount = 48;
// Not right on top of the player, and never in shot: San Andreas puts its own peds in a ring
// roughly 42.5 to 50.5 units out and takes them away again past 65 off screen or 54.5 on it -
// those are the numbers its own CPopulation::Update and ManagePed use - so RegionPedRadius
// defaults to 55 to sit inside the same band. Past that the game would only remove them again the
// moment they were made, and the crowd would churn instead of walking.
constexpr float kMinSpawnDistance = 22.0f;
constexpr float kCullMargin = 1.3f;              // of the live radius, before we take one back
constexpr int kSpawnsPerFrame = 2;
// Two nodes closer together than this are the same doorway as far as a crowd is concerned; used to
// stop two people being put in the same place.
constexpr float kMinPedSeparation = 2.5f;

constexpr int kTaskPrimaryDefault = 4;   // ePrimaryTasks::TASK_PRIMARY_DEFAULT
constexpr unsigned char kPedCreatedByGame = 1;

// The pedestrian graph, as pednodes.dat writes it. Links are held in one flat array because a
// vector per node for several thousand nodes is a great deal of allocation for two or three
// numbers each.
struct Node {
    float x, y, z;      // the region's own coordinates; the origin is added when one is used
    int zone;           // line in the zone table below, or -1
    int firstLink, linkCount;
};

struct Group {
    std::string name;
    std::vector<std::string> models;   // San Andreas model names, the right of each DONOR/SA pair
    std::vector<int> ids;              // resolved on first use; -1 for a name San Andreas lacks
    bool resolved = false;
};

struct Zone {
    std::string name;
    int day, night;     // indices into the group table
};

// One ped this module is looking after.
struct Walker {
    int ref;            // CPools ped handle: slot and re-use counter, so a recycled slot reads dead
    int node;           // where in the graph they are heading
};

struct RegionState {
    const MapRegion* region = nullptr;
    std::vector<Node> nodes;
    std::vector<int> links;
    std::vector<Group> groups;
    std::vector<Zone> zones;
    std::unordered_map<int64_t, std::vector<int>> grid;   // node indices by cell
    std::vector<Walker> walkers;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;   // world extent of the graph
};

constexpr float kGridCell = 48.0f;

std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }

int64_t CellKey(float x, float y) {
    int cx = (int)floorf(x / kGridCell);
    int cy = (int)floorf(y / kGridCell);
    return ((int64_t)cx << 32) ^ (uint32_t)cy;
}

// ------------------------------------------------------------------ reading the region's files

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

bool ReadWholeFile(const std::string& path, std::string& text) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);
    return true;
}

// Every line is "x y z zone link link ...", numbered by the order it appears in.
bool ReadNodeFile(RegionState& rs) {
    std::string text;
    if (!ReadWholeFile(rs.region->pedNodeFile, text)) {
        Log::Write("[%s] peds: %s cannot be opened", rs.region->name.c_str(),
                   rs.region->pedNodeFile.c_str());
        return false;
    }
    int malformed = 0;
    size_t at = 0;
    std::vector<std::vector<int>> linksPerNode;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::vector<std::string> t = Tokens(line);
        if (t.empty()) continue;
        if (t.size() < 4) { malformed++; continue; }
        Node n;
        n.x = (float)atof(t[0].c_str());
        n.y = (float)atof(t[1].c_str());
        n.z = (float)atof(t[2].c_str());
        n.zone = atoi(t[3].c_str());
        n.firstLink = n.linkCount = 0;
        rs.nodes.push_back(n);
        std::vector<int> mine;
        for (size_t i = 4; i < t.size(); i++) mine.push_back(atoi(t[i].c_str()));
        linksPerNode.push_back(mine);
    }
    // Flatten, dropping any link that names a node the file does not have - a truncated or
    // hand-edited file must not walk somebody off the end of the array.
    int dropped = 0;
    for (size_t i = 0; i < rs.nodes.size(); i++) {
        rs.nodes[i].firstLink = (int)rs.links.size();
        for (int n : linksPerNode[i]) {
            if (n < 0 || n >= (int)rs.nodes.size() || n == (int)i) { dropped++; continue; }
            rs.links.push_back(n);
        }
        rs.nodes[i].linkCount = (int)rs.links.size() - rs.nodes[i].firstLink;
    }
    if (malformed || dropped)
        Log::Write("[%s] peds: %d malformed node line(s), %d link(s) naming a node that is not "
                   "there", rs.region->name.c_str(), malformed, dropped);
    return !rs.nodes.empty();
}

// "zone <n> <NAME> <day> <night>" and "group <n> <name> <DONOR/SAMODEL> ..." in any order.
bool ReadGroupFile(RegionState& rs) {
    std::string text;
    if (!ReadWholeFile(rs.region->pedGroupFile, text)) {
        Log::Write("[%s] peds: %s cannot be opened; everybody will be a stand-in",
                   rs.region->name.c_str(), rs.region->pedGroupFile.c_str());
        return false;
    }
    int malformed = 0;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::vector<std::string> t = Tokens(line);
        if (t.empty()) continue;
        if (_stricmp(t[0].c_str(), "zone") == 0) {
            if (t.size() < 5) { malformed++; continue; }
            int index = atoi(t[1].c_str());
            if (index < 0 || index > 4096) { malformed++; continue; }
            if ((int)rs.zones.size() <= index) rs.zones.resize(index + 1);
            rs.zones[index] = Zone{ t[2], atoi(t[3].c_str()), atoi(t[4].c_str()) };
        } else if (_stricmp(t[0].c_str(), "group") == 0) {
            if (t.size() < 4) { malformed++; continue; }
            int index = atoi(t[1].c_str());
            if (index < 0 || index > 4096) { malformed++; continue; }
            if ((int)rs.groups.size() <= index) rs.groups.resize(index + 1);
            Group g;
            g.name = t[2];
            for (size_t i = 3; i < t.size(); i++) {
                // "DONORNAME/SAMODEL": the left half is what the source game asked for and is only
                // here so that importing its own peds later is a change to this file. The right
                // half is what is actually put in the street. A bare name is taken as the model.
                size_t slash = t[i].find('/');
                g.models.push_back(slash == std::string::npos ? t[i] : t[i].substr(slash + 1));
            }
            rs.groups[index] = g;
        } else {
            malformed++;
        }
    }
    if (malformed)
        Log::Write("[%s] peds: %d line(s) of %s neither a zone nor a group",
                   rs.region->name.c_str(), malformed, rs.region->pedGroupFile.c_str());
    return !rs.groups.empty();
}

void BuildGrid(RegionState& rs) {
    for (size_t i = 0; i < rs.nodes.size(); i++) {
        float wx = rs.nodes[i].x + rs.region->originX;
        float wy = rs.nodes[i].y + rs.region->originY;
        rs.grid[CellKey(wx, wy)].push_back((int)i);
        if (i == 0) {
            rs.minX = rs.maxX = wx;
            rs.minY = rs.maxY = wy;
        } else {
            if (wx < rs.minX) rs.minX = wx;
            if (wx > rs.maxX) rs.maxX = wx;
            if (wy < rs.minY) rs.minY = wy;
            if (wy > rs.maxY) rs.maxY = wy;
        }
    }
}

CVector NodeWorld(const RegionState& rs, int node) {
    const Node& n = rs.nodes[node];
    return CVector(n.x + rs.region->originX, n.y + rs.region->originY, n.z + rs.region->originZ);
}

// ------------------------------------------------------------------ choosing who to put there

// Which of a district's two groups is on the street. The donor games blend between the two either
// side of dawn and dusk; there is nothing to blend here - one person is one model - so it is a
// straight switch at seven in the morning and eight at night, which is inside both games' windows.
bool IsDaytime() {
    int hour = CClock::ms_nGameClockHours;
    return hour >= 7 && hour < 20;
}

void ResolveGroup(RegionState& rs, Group& g) {
    if (g.resolved) return;
    g.resolved = true;
    int missing = 0;
    for (const std::string& name : g.models) {
        int id = -1;
        CBaseModelInfo* info = CModelInfo::GetModelInfo((char*)name.c_str(), &id);
        if (!info || id < 0) { g.ids.push_back(-1); missing++; continue; }
        g.ids.push_back(id);
    }
    if (missing)
        Log::Write("[%s] peds: group %s names %d model(s) San Andreas does not have; they are "
                   "skipped", rs.region->name.c_str(), g.name.c_str(), missing);
}

// A model from the group this node's district asks for, streamed in and ready to use. Returns -1
// when nothing of the group is in memory yet, having asked the streamer for one; the next tick
// tries again, which is how the game itself waits for a ped model.
int ChooseModel(RegionState& rs, int node) {
    int groupIndex = 0;
    int zone = rs.nodes[node].zone;
    if (zone >= 0 && zone < (int)rs.zones.size()) {
        const Zone& z = rs.zones[zone];
        groupIndex = IsDaytime() ? z.day : z.night;
    }
    if (groupIndex < 0 || groupIndex >= (int)rs.groups.size()) return -1;
    Group& g = rs.groups[groupIndex];
    ResolveGroup(rs, g);
    if (g.ids.empty()) return -1;

    int start = rand() % (int)g.ids.size();
    int firstWanted = -1;
    for (size_t i = 0; i < g.ids.size(); i++) {
        int id = g.ids[(start + i) % g.ids.size()];
        if (id < 0) continue;
        if (firstWanted < 0) firstWanted = id;
        if (ModelLimits::ModelIsLoaded(id)) {
            Log::Debug("[%s] peds: %s in %s takes model %d from group %d (%s)",
                       rs.region->name.c_str(), IsDaytime() ? "daytime" : "night",
                       zone >= 0 && zone < (int)rs.zones.size() ? rs.zones[zone].name.c_str()
                                                               : "nowhere in particular",
                       id, groupIndex, g.name.c_str());
            return id;
        }
    }
    // Nobody from this district is in memory. Ask for one and come back later - through
    // StreamedModels, which hands it back to the streamer the moment it has loaded. Asking with
    // GAME_REQUIRED and leaving it at that is what this used to do, and it kept every model any
    // district ever wanted resident for the session: read the top of StreamedModels.cpp before
    // changing this back.
    StreamedModels::Want(firstWanted);
    return -1;
}

// ------------------------------------------------------------------ the graph

// Nodes within `radius` of a world point, appended to `out`.
void NodesNear(const RegionState& rs, const CVector& at, float radius, std::vector<int>& out) {
    int cells = (int)ceilf(radius / kGridCell);
    int cx = (int)floorf(at.x / kGridCell);
    int cy = (int)floorf(at.y / kGridCell);
    float r2 = radius * radius;
    for (int dx = -cells; dx <= cells; dx++) {
        for (int dy = -cells; dy <= cells; dy++) {
            auto it = rs.grid.find(((int64_t)(cx + dx) << 32) ^ (uint32_t)(cy + dy));
            if (it == rs.grid.end()) continue;
            for (int n : it->second) {
                CVector p = NodeWorld(rs, n);
                float dxx = p.x - at.x, dyy = p.y - at.y;
                if (dxx * dxx + dyy * dyy <= r2) out.push_back(n);
            }
        }
    }
}

bool PlayerOver(const RegionState& rs, const CVector& at, float margin) {
    return at.x >= rs.minX - margin && at.x <= rs.maxX + margin &&
           at.y >= rs.minY - margin && at.y <= rs.maxY + margin;
}

// ------------------------------------------------------------------ making one walk

bool VerifyWalkTask() {
    uint8_t here[sizeof(kGoToPointPrologue)];
    memcpy(here, (const void*)kGoToPointCtor, sizeof(here));
    if (memcmp(here, kGoToPointPrologue, sizeof(here)) == 0) return true;
    char got[64] = "";
    for (size_t i = 0; i < sizeof(here); i++)
        snprintf(got + strlen(got), sizeof(got) - strlen(got), "%02X ", here[i]);
    Log::Write("peds: CTaskComplexGoToPointAndStandStill at 0x%X does not read as expected "
               "(%s); region peds will be placed but not walked", (unsigned)kGoToPointCtor, got);
    return false;
}

// Give the ped a leg of their walk. Set at TASK_PRIMARY_DEFAULT, which is the slot San Andreas
// puts a wander task in, so anything that happens to the ped - a fright, a fight, being pulled out
// of the way of a car - takes over above it and this comes back afterwards, exactly as wandering
// does for San Andreas' own peds.
bool GiveWalkTask(CPed* ped, const CVector& target) {
    if (!gCanWalk || !ped || !ped->m_pIntelligence) return false;
    void* memory = reinterpret_cast<TaskNewFn>(kTaskNew)(kGoToPointSize);
    if (!memory) return false;
    // The constructor compares the target it is given against the object's own target and radii
    // before deciding to copy them, and the memory it is handed comes out of a pool that has held
    // other tasks. Wiping it first makes that comparison meaningless rather than a coin toss.
    memset(memory, 0, kGoToPointSize);
    CVector goal = target;
    reinterpret_cast<GoToPointCtorFn>(kGoToPointCtor)(memory, kMoveStateWalk, &goal,
                                                      kArriveRadius, kSlowDownDistance,
                                                      false, false);
    CTaskManager& tasks = ped->m_pIntelligence->m_TaskMgr;
    tasks.SetTask(reinterpret_cast<CTask*>(memory), kTaskPrimaryDefault, false);
    // SetTask owns the task from that moment - it deletes whatever was in the slot, and it deletes
    // the new one too if the chain it builds bottoms out in a complex task with no subtask. So the
    // pointer is never touched again; the slot is read back instead to say whether it stuck.
    return tasks.m_aPrimaryTasks[kTaskPrimaryDefault] != nullptr;
}

int NextNode(const RegionState& rs, int node) {
    const Node& n = rs.nodes[node];
    if (n.linkCount <= 0) return node;
    return rs.links[n.firstLink + (rand() % n.linkCount)];
}

// The nearest node to a point, or -1 if there is none within `radius`.
int NearestNode(const RegionState& rs, const CVector& at, float radius) {
    std::vector<int> nearby;
    NodesNear(rs, at, radius, nearby);
    int best = -1;
    float bestd = 0.0f;
    for (int n : nearby) {
        CVector p = NodeWorld(rs, n);
        float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) +
                  (p.z - at.z) * (p.z - at.z);
        if (best < 0 || d < bestd) { best = n; bestd = d; }
    }
    return best;
}

// How far a ped may drift from the node it is walking to before it is taken to have lost its
// place. A ped knocked over by a car, pushed into a doorway or frightened up an alley comes back
// to a walk when the fright is over, and without this it would set off for wherever it had been
// heading - possibly a street away, possibly through a wall.
constexpr float kReanchorDistance = 30.0f;

// Is this point somewhere a person could be put without appearing out of nothing in front of the
// player? Close by, only out of shot; further away, anywhere.
bool GoodSpawnPoint(const RegionState& rs, const CVector& world, const CVector& player,
                    bool avoidCamera, int* why = nullptr) {
    float dx = world.x - player.x, dy = world.y - player.y, dz = world.z - player.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < kMinSpawnDistance) { if (why) *why = 1; return false; }
    // Never where the player is looking. This is why a crowd fills in behind you rather than
    // appearing in front, which is what San Andreas does and what makes it not look like a mod.
    if (avoidCamera && TheCamera.IsSphereVisible(world, 1.5f)) { if (why) *why = 2; return false; }
    for (const Walker& w : rs.walkers) {
        CPed* other = CPools::GetPed(w.ref);
        if (!other) continue;
        CVector p = other->GetPosition();
        float ox = p.x - world.x, oy = p.y - world.y, oz = p.z - world.z;
        if (ox * ox + oy * oy + oz * oz < kMinPedSeparation * kMinPedSeparation) {
            if (why) *why = 3;
            return false;
        }
    }
    return true;
}

// CPopulation::AddPed cannot fail politely, and it is worth knowing exactly how it fails so that
// neither way is ever reached.
//
// A ped type it has no case for - 21 is one, and so is anything outside 4..31 - falls through its
// jump table into a branch that reads a pointer out of an uninitialised local and dereferences it.
// The type comes from CPedModelInfo, which for San Andreas' own peds is always one of the good
// ones, but a region's ped table can name any model at all and a stray one must not take the game
// down. Anything unrecognised becomes an ordinary civilian.
int SafePedType(int pedType) {
    switch (pedType) {
        case 4: case 5: case 6:                                    // civilian male, female, cop
        case 7: case 8: case 9: case 10: case 11: case 12:         // the gangs
        case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20: case 22:               // dealer, medic, fireman,
            return pedType;                                        // criminal, prostitute
        default:
            return 4;                                              // PED_TYPE_CIVMALE
    }
}

// The other way AddPed fails: when CPed::operator new returns nothing it carries on with a null
// and dereferences it, so it cannot report a full pool - it faults. The pool is checked here
// instead, keeping back the same eight slots the game's own RemovePedsIfThePoolGetsFull works to.
constexpr int kPedPoolReserve = 8;

// Where a tick's spawn attempts went, summed over a second.
//
// This exists because "16 walking and none of them visible" and "nothing spawns at all" are the
// same line in the log - a count - and a count cannot say whether nobody was made, or everybody
// was made and taken away again, or the same two people were made and unmade sixty times a second.
// Three rounds of reasoning from counts got the wrong answer; this says which it is.
struct Tally {
    int ticks, attempts, tooNear, inShot, crowded, noModel, poolFull, madeNothing, placed;
    int noGround, offDatum, noPlayerGround;
    int lost, culled, kept;
    unsigned nextReport;
};
Tally& Counts() { static Tally t{}; return t; }

// The ground under a node, and WHICH question you ask matters.
//
// CPedPlacement::FindZCoorForPed (0x616920) is the ped-shaped one and it is what this used to
// call on its own. Read what it actually searches: it takes the z you hand it, adds the float at
// 0x858B8C - which is 0.5 - for the top of the line, subtracts the float at 0x858628 - which is
// 100 - for the bottom, and casts a vertical line between them. So it will find a surface a
// hundred units BELOW the height you gave it and half a metre above, and nothing else. Hand it a
// point that is under the road and it cannot find the road; it finds whatever is under that, or
// nothing.
//
// CWorld::FindGroundZForCoord (0x569660) asks the other question. It ignores z entirely and casts
// from 1000 straight down (the 0x447A0000 it plants at [esp+0x34] before calling
// CWorld::ProcessVerticalLine), so it answers "what is the highest ground at this x and y" and
// cannot be wrong about a datum it never reads. It is the same routine
// CCarCtrl::CreateCarForScript uses when it is handed the -100 sentinel.
//
// So: ask the whole-column question first, and use its answer to seed the ped-shaped one, which
// then places the ped properly relative to that surface. Where the graph is right the two agree
// and nothing changes. Where the graph is 20 units under the street - which is what an imported
// city measured on the ground, with the player standing on an ordinary road and the nearest node
// twenty units below their feet - this is the difference between a crowd and a rumour.
constexpr float kDatumSlack = 2.0f;

// False means there is no floor at this node and nobody may be put there.
//
// The first version of this called CWorld::FindGroundZForCoord, which looks like the right
// question and is not: where there is no collision it returns the float at 0x858BA4, which is
// **20.0**, and twenty is an ordinary street height in an imported city. Every failed probe
// therefore came back as a plausible answer a few units above the road, and the peds stood in the
// air and fell - which is what the player saw. Ground::Under asks
// CWorld::FindGroundZFor3DCoord instead, which reports failure instead of inventing a height.
// Read the top of src/region/Ground.cpp before changing this back.
bool SettleOnGround(const CVector& at, CVector& out, float* datumError) {
    out = at;
    float ground = 0.0f;
    if (!Ground::Under(out.x, out.y, out.z, ground)) return false;
    if (datumError) *datumError = ground - out.z;
    if (fabsf(ground - out.z) > kDatumSlack) out.z = ground;
    // Now the ped-shaped placement, against a height that is known to be real.
    float z = CPedPlacement::FindZCoorForPed(&out);
    if (z > -200.0f && z < 2000.0f) out.z = z;
    return true;
}

CPed* CreateWalker(RegionState& rs, int node, int modelId, bool walk) {
    CBaseModelInfo* info = CModelInfo::GetModelInfo(modelId);
    if (!info) return nullptr;
    if (!RegionPeds::SafeToAddPed()) return nullptr;
    int pedType = SafePedType((int)reinterpret_cast<CPedModelInfo*>(info)->m_nPedType);
    CVector at;
    float datumError = 0.0f;
    // No floor under this node means nobody stands on it. Refusing is the whole point: the old
    // code could not tell "no ground" from "ground at twenty", so it put somebody in the air.
    if (!SettleOnGround(NodeWorld(rs, node), at, &datumError)) { Counts().noGround++; return nullptr; }
    if (fabsf(datumError) > kDatumSlack) Counts().offDatum++;
    CPed* ped = CPopulation::AddPed((ePedType)pedType, (unsigned)modelId, at, false);
    if (!ped) return nullptr;
    // Created by the game, not by a script: this is what makes CPopulation::ManagePopulation
    // responsible for taking them away again, so they are culled on exactly the same terms as San
    // Andreas' own and no separate lifetime is invented here.
    ped->SetCharCreatedBy(kPedCreatedByGame);
    rs.walkers.push_back(Walker{ CPools::GetPedRef(ped), node });
    if (walk) GiveWalkTask(ped, NodeWorld(rs, NextNode(rs, node)));
    return ped;
}

// ------------------------------------------------------------------ the frame

int LivePedBudget() {
    float density = Settings::world.regionPedDensity * CPopulation::PedDensityMultiplier;
    int want = (int)(kBasePedCount * density);
    if (want < 0) want = 0;
    if (want > kMaxPedCount) want = kMaxPedCount;
    return want;
}

// Forget the ones the game has taken away, keep the rest walking, and take back any that have
// strayed further than this module looks after.
void ManageWalkers(RegionState& rs, const CVector& player) {
    const float cull = Settings::world.regionPedRadius * kCullMargin;
    size_t write = 0;
    for (size_t i = 0; i < rs.walkers.size(); i++) {
        Walker& w = rs.walkers[i];
        CPed* ped = CPools::GetPed(w.ref);
        // The game removed them, or the pool slot was re-used under the handle. Which of the two
        // it is cannot be told from here, but how OFTEN it happens is the whole question when a
        // crowd is being made and unmade sixty times a second, so it is counted.
        if (!ped) { Counts().lost++; continue; }
        CVector p = ped->GetPosition();
        float dx = p.x - player.x, dy = p.y - player.y;
        if (dx * dx + dy * dy > cull * cull && !TheCamera.IsSphereVisible(p, 1.5f)) {
            CPopulation::RemovePed(ped);
            Counts().culled++;
            continue;
        }
        Counts().kept++;
        // A ped with nothing in the default slot has finished their leg - or has just come out of
        // an event that cleared it - so send them on to a neighbouring node.
        if (gCanWalk && ped->IsAlive() && ped->m_pIntelligence &&
            !ped->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[kTaskPrimaryDefault]) {
            CVector node = NodeWorld(rs, w.node);
            float nx = node.x - p.x, ny = node.y - p.y;
            if (nx * nx + ny * ny > kReanchorDistance * kReanchorDistance) {
                int here = NearestNode(rs, p, kReanchorDistance);
                if (here < 0) { rs.walkers[write++] = w; continue; }   // off the graph; let them be
                w.node = here;
            }
            w.node = NextNode(rs, w.node);
            GiveWalkTask(ped, NodeWorld(rs, w.node));
        }
        rs.walkers[write++] = w;
    }
    rs.walkers.resize(write);
}

void ReportTally() {
    Tally& t = Counts();
    if (!Settings::world.debug) return;
    if (CTimer::m_snTimeInMilliseconds < t.nextReport) return;
    t.nextReport = CTimer::m_snTimeInMilliseconds + 1000;
    if (!t.ticks) return;
    Log::Write("peds/sec: %d tick(s), %d attempt(s) -> %d too near, %d in shot, %d on top of "
               "somebody, %d no model in memory, %d pool full, %d no ground under the node, "
               "%d AddPed said no, %d placed (%d of them off the graph's own height); "
               "%d kept, %d vanished, %d taken back; %d tick(s) with no ground under the player; "
               "%d model(s) still asked for",
               t.ticks, t.attempts, t.tooNear, t.inShot, t.crowded, t.noModel, t.poolFull,
               t.noGround, t.madeNothing, t.placed, t.offDatum, t.kept, t.lost, t.culled,
               t.noPlayerGround, StreamedModels::Held());
    Tally fresh{};
    fresh.nextReport = t.nextReport;
    t = fresh;
}

void SpawnSome(RegionState& rs, const CVector& player, int wanted) {
    Tally& t = Counts();
    t.ticks++;
    std::vector<int> candidates;
    NodesNear(rs, player, Settings::world.regionPedRadius, candidates);
    if (candidates.empty()) return;
    int placed = 0;
    for (int attempt = 0; attempt < 24 && placed < kSpawnsPerFrame &&
                          (int)rs.walkers.size() < wanted; attempt++) {
        t.attempts++;
        int node = candidates[rand() % candidates.size()];
        CVector world = NodeWorld(rs, node);
        int why = 0;
        if (!GoodSpawnPoint(rs, world, player, true, &why)) {
            if (why == 1) t.tooNear++; else if (why == 2) t.inShot++; else t.crowded++;
            continue;
        }
        if (!RegionPeds::SafeToAddPed()) { t.poolFull++; return; }
        int model = ChooseModel(rs, node);
        if (model < 0) { t.noModel++; return; }   // nothing of this district in memory yet
        if (CreateWalker(rs, node, model, true)) { placed++; t.placed++; }
        else t.madeNothing++;
    }
}

SafetyHookInline& UpdateOriginal() { static SafetyHookInline h; return h; }

void __cdecl UpdateHook(bool generatePeds) {
    UpdateOriginal().ccall<void>(generatePeds);

    CPed* player = FindPlayerPed();
    if (!player) {
        // A menu, a loading screen, or a game that has just ended. Let go of every ped this was
        // looking after: a new game empties the ped pool and resets its re-use counters, so a
        // handle kept across one could name somebody else's ped entirely.
        for (RegionState& rs : Regions()) rs.walkers.clear();
        RegionPolice::Forget();
        RegionTraffic::Forget();
        RegionParkedCars::Forget();
        StreamedModels::ReleaseAll();
        return;
    }
    // Hand back anything the streamer has finished fetching for us, every frame. This is what
    // stops a district's ped models being held out of San Andreas' own rotation for the rest of
    // the session, which is what was starving its vehicle streaming hundreds of units away - the
    // whole story is at the top of StreamedModels.cpp.
    StreamedModels::Tick();
    CVector at = player->GetPosition();
    // The parked cars are driven from outside the loop below, and deliberately. That loop stops at
    // the first region the player is standing over and does nothing at all when they are over
    // none - which is exactly when a car standing at a Vice City kerb needs taking away, because
    // the player has just driven out of Vice City.
    // Is there a floor under the player at all? Asked once a frame and handed to everything that
    // creates something, because a crowd that follows the player is right and a crowd that follows
    // one flying a jetpack beneath the map is not - and the second is what the first does unless
    // somebody decides otherwise. Standing in a tunnel, under a bridge or inside a building all
    // still count as having ground; see Ground::BeneathEverything.
    const bool playerHasGround = !Ground::BeneathEverything(at.x, at.y, at.z);

    RegionParkedCars::Tick(at, playerHasGround);
    // The traffic is ticked from out here too, and for the same reason the parked cars are: it
    // owns vehicles that have to be handed back when the player leaves the city, and this loop
    // does nothing at all when they are over no region. It used to be called from inside the
    // loop, which meant a car of ours was only ever tidied away while the player was still
    // standing over a PEDESTRIAN graph - two graphs that do not cover the same ground.
    RegionTraffic::Tick(at, playerHasGround);

    RegionState* here = nullptr;
    for (RegionState& rs : Regions()) {
        if (!here && PlayerOver(rs, at, Settings::world.regionPedRadius * kCullMargin)) {
            here = &rs;
            continue;
        }
        // Out of this region altogether: let go of everybody, the game will remove them the
        // moment they are far enough away and it is not this module's business any more.
        rs.walkers.clear();
    }
    if (!here) {
        // Over no region at all. Nothing of ours is being made, so nothing of ours should be
        // held: give the streamer back anything we have asked it for.
        StreamedModels::ReleaseAll();
        return;
    }
    ManageWalkers(*here, at);
    if (generatePeds && playerHasGround) SpawnSome(*here, at, LivePedBudget());
    else if (generatePeds) Counts().noPlayerGround++;
    ReportTally();
    // The police ride on this graph too, and on nothing else, so they are driven from here rather
    // than from a hook of their own: one region under the player, one tick.
    RegionPolice::Tick(at);
}

RegionState* RegionUnder(const CVector& at) {
    for (RegionState& rs : Regions())
        if (PlayerOver(rs, at, Settings::world.regionPedRadius)) return &rs;
    return nullptr;
}

}   // namespace

namespace RegionPeds {

bool PlayerOverGraph() {
    CPed* player = FindPlayerPed();
    return player && RegionUnder(player->GetPosition()) != nullptr;
}

bool FindSpawnPoint(const CVector& at, float minDist, float maxDist, bool avoidCamera,
                    CVector& out) {
    RegionState* rs = RegionUnder(at);
    if (!rs) return false;
    std::vector<int> candidates;
    NodesNear(*rs, at, maxDist, candidates);
    for (int attempt = 0; attempt < 32 && !candidates.empty(); attempt++) {
        int node = candidates[rand() % candidates.size()];
        CVector world = NodeWorld(*rs, node);
        float dx = world.x - at.x, dy = world.y - at.y;
        if (dx * dx + dy * dy < minDist * minDist) continue;
        if (avoidCamera && TheCamera.IsSphereVisible(world, 1.5f)) continue;
        if (!SettleOnGround(world, out, nullptr)) continue;
        return true;
    }
    return false;
}

CPed* Spawn(int modelId, const CVector& at, bool wander) {
    RegionState* rs = RegionUnder(at);
    if (!rs || !ModelLimits::ModelIsLoaded(modelId)) return nullptr;
    std::vector<int> candidates;
    NodesNear(*rs, at, kGridCell * 2.0f, candidates);
    int node = -1;
    float best = 0.0f;
    for (int n : candidates) {
        CVector p = NodeWorld(*rs, n);
        float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y);
        if (node < 0 || d < best) { node = n; best = d; }
    }
    if (node < 0) return nullptr;
    return CreateWalker(*rs, node, modelId, wander);
}

int Count() {
    int n = 0;
    for (const RegionState& rs : Regions()) n += (int)rs.walkers.size();
    return n;
}

bool SafeToAddPed() {
    auto* pool = CPools::ms_pPedPool;
    return pool && (int)pool->GetNoOfFreeSpaces() > kPedPoolReserve;
}

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.pedNodeFile.empty();
    if (!any || !Settings::world.regionPeds) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region pedestrians are off, the server owns the population");
        return;
    }

    for (const MapRegion& r : Settings::regions) {
        if (r.pedNodeFile.empty()) continue;
        RegionState rs;
        rs.region = &r;
        if (!ReadNodeFile(rs)) continue;
        if (!r.pedGroupFile.empty()) ReadGroupFile(rs);
        BuildGrid(rs);
        Log::Write("[%s] peds: %u node(s), %u link(s), %u district(s), %u group(s) over "
                   "x %.0f..%.0f y %.0f..%.0f",
                   r.name.c_str(), (unsigned)rs.nodes.size(), (unsigned)rs.links.size(),
                   (unsigned)rs.zones.size(), (unsigned)rs.groups.size(),
                   rs.minX, rs.maxX, rs.minY, rs.maxY);
        Regions().push_back(std::move(rs));
    }
    if (Regions().empty()) return;

    gCanWalk = VerifyWalkTask();

    UpdateOriginal() = safetyhook::create_inline(reinterpret_cast<void*>(kPopulationUpdate),
                                                reinterpret_cast<void*>(&UpdateHook));
    if (!UpdateOriginal()) {
        Log::Write("peds: CPopulation::Update could not be hooked; the region gets no pedestrians");
        Regions().clear();
        return;
    }
    Log::Write("peds: %u region(s) will be populated from their own pedestrian graph, up to %d at "
               "a time within %.0f units%s",
               (unsigned)Regions().size(), LivePedBudget(), Settings::world.regionPedRadius,
               gCanWalk ? "" : " - standing, not walking");
}

// ------------------------------------------------------------------ the debug overlay
//
// The pavement graph, drawn from this module's own NodeWorld() - see RegionPeds.h. It is on its
// own key because it is a separate answer: pednodes.dat and carnodes.dat are two files, read by
// two parsers, offset by the same origin. If the pavement lands on the pavements and the
// carriageways do not land on the roads, the fault is in the carriageway derivation and not in
// where the city was put; if neither lands anywhere sensible, the region's origin is wrong and
// every other question is downstream of that.
constexpr unsigned kPedLink = 0xC060FF90u;   // violet
constexpr unsigned kPedNode = 0xFFC0FFC0u;
constexpr unsigned kWalker  = 0xFFFFFF80u;

void DrawGraph(const CVector& eye, float radius) {
    const float r2 = radius * radius;
    for (const RegionState& rs : Regions()) {
        if (!PlayerOver(rs, eye, radius)) continue;
        std::vector<int> nearby;
        NodesNear(rs, eye, radius, nearby);
        for (int i : nearby) {
            if (!PathDraw::Budget()) return;
            const CVector a = NodeWorld(rs, i);
            const Node& n = rs.nodes[i];
            PathDraw::Cross(a, 0.4f, kPedNode);
            for (int k = 0; k < n.linkCount; k++) {
                if (!PathDraw::Budget()) return;
                const int j = rs.links[n.firstLink + k];
                const CVector b = NodeWorld(rs, j);
                const float bx = b.x - eye.x, by = b.y - eye.y;
                const bool bothIn = (bx * bx + by * by) <= r2;
                if (bothIn && j < i) continue;   // drawn once, from the lower index
                PathDraw::Line(a, b, kPedLink);
            }
        }
        // The node each walker is heading for. A ped walking away from its own line is the same
        // class of fault as a car steering away from its link, and it is worth being able to see
        // both at once.
        for (const Walker& w : rs.walkers) {
            if (!PathDraw::Budget()) return;
            if (w.node < 0 || w.node >= (int)rs.nodes.size()) continue;
            CPed* ped = CPools::GetPed(w.ref);
            if (!ped) continue;
            const CVector p = ped->GetPosition();
            const float dx = p.x - eye.x, dy = p.y - eye.y;
            if (dx * dx + dy * dy > r2) continue;
            PathDraw::Line(p, NodeWorld(rs, w.node), kWalker);
        }
    }
}

}   // namespace RegionPeds
