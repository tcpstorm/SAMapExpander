// Cars driving in an imported city, along the roads the source game itself drew.
//
// WHY NOT THROUGH CPathFind, WHICH WOULD HAVE BEEN THE OBVIOUS WAY. Two encoding limits stand in
// the way and only the first is fixable. A path node's position is an int16 times 0.125, reaching
// +/-4096, and src/limits/PathScale.cpp can widen that. But a vehicle link is addressed by a packed
// uint16 - area = id >> 10, index = id & 0x3FF - so there are 1024 CCarPathLink per area and no
// more, and San Andreas already uses a good part of that. Vice City's ground clamps into five
// areas holding 2451 of the 5120 links those areas can carry between them; its roads need 8404,
// which is three times the whole capacity rather than three times what is free. There is no
// choosing different areas either: everything past x 2250 clamps into column 7 by construction.
// docs/known-limitations.md has the measurement.
//
// So the graph is kept here instead, the same way the pedestrian one is, and the cars are driven
// from it. That has a property worth more than fidelity: San Andreas' own path data is not touched
// at all, so its own traffic cannot regress. The worst this can do is drive badly in an imported
// city.
//
// WHAT DRIVES THEM. Not us - the game. A car is made with CCarCtrl::CreateCarForScript, given a
// driver, and put on MISSION_GOTOCOORDS_STRAIGHT with its destination set to a point in its lane at
// the far end of the road it is on. San Andreas' own vehicle AI does the steering, the throttle,
// the braking and the collision avoidance from there; when the car arrives, this hands it the next
// node and it carries on. Nothing here simulates driving, which is why it looks like driving.
//
// LANES. This is the difference between traffic and objects sliding along a graph, so it is done
// properly. Every node carries how many lanes run each way, out of the source game's own data, and
// a car is assigned a lane when it spawns and keeps it. The lane centre is offset to the RIGHT of
// the direction of travel by (lane + 0.5) * 5.4 units - 5.4 being San Andreas' own lane width, the
// float at 0x858C50 that its lane placement and steering read. Because the offset is always to the
// right of travel, two cars going opposite ways down the same link separate onto their own sides
// without anything having to know which side of the road the city drives on.
//
// The median column the source games also write is deliberately NOT used: GTA III writes the
// constant 160 for every car node and Vice City writes a continuous small value like 3.5, so the
// two disagree about what it means and there is no way to tell from the file which reading is
// right. Lane counts and San Andreas' own lane width cannot be wrong for either.
//
// Nothing here runs under SA-MP: the server owns traffic there.
#include <plugin.h>
#include <CCarCtrl.h>
#include <CPools.h>
#include <CVehicle.h>
#include <CPopulation.h>
#include <CStreaming.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CWorld.h>
#include <CModelInfo.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "RegionTraffic.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// San Andreas' own lane width: the float at 0x858C50, read by 26 instructions across CCarCtrl's
// lane placement and steering and CAutoPilot::RemoveOnePathNode. Using the game's own number means
// a lane here is the same width as a lane over the border.
constexpr float kLaneWidth = 5.4f;
constexpr int kMaxLanes = 4;

// Where cars live relative to the player. San Andreas creates its own between about 42 and 50 units
// out and removes them past 65 off screen; this sits inside the same band so the two behave alike
// either side of the border rather than one of them lingering.
constexpr float kMinSpawnDistance = 40.0f;
constexpr float kCullMargin = 1.4f;
constexpr int kSpawnsPerTick = 1;
constexpr unsigned kSpawnIntervalMs = 400;
// How near the destination counts as arrived, and therefore when the next leg is handed out. Wider
// than a lane so a car that clipped a kerb still counts as having got there.
constexpr float kArrivedDistance = 8.0f;
// A car this far from the road it is supposed to be on has lost its way - shunted by the player,
// most likely - and is given back to the game rather than dragged to a node it can no longer reach.
constexpr float kStrayDistance = 45.0f;

// eCarMission and eCarDrivingStyle, from plugin-sdk. GOTOCOORDS_STRAIGHT steers for the
// destination on physics alone; STOP_FOR_CARS is what makes one car give way to another, which is
// all the give-way behaviour a junction gets without traffic lights.
constexpr char kMissionGotoCoordsStraight = 9;
constexpr char kDrivingStyleStopForCars = 0;
// eVehicleCreatedBy: 1 is a random traffic car, which is what makes the game cull it on its own
// terms. 2 is a mission car and would never be removed.
constexpr unsigned char kVehicleCreatedByRandom = 1;

struct Node {
    float x, y, z;
    int firstLink, linkCount;
    uint8_t lanes;      // per direction, at least 1
};

// One car this module is looking after. `from` and `to` are the road it is on; `lane` is which
// lane of it, and does not change for the life of the car.
struct Driver {
    int ref;            // CPools vehicle handle: slot plus re-use counter
    int from, to;
    int lane;
};

struct RegionState {
    const MapRegion* region = nullptr;
    std::vector<Node> nodes;
    std::vector<int> links;
    std::unordered_map<int64_t, std::vector<int>> grid;
    std::vector<Driver> drivers;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
};

constexpr float kGridCell = 64.0f;

std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }
unsigned gNextSpawn = 0;

int64_t CellKey(int cx, int cy) { return ((int64_t)cx << 32) ^ (uint32_t)cy; }

// ------------------------------------------------------------------ the road graph

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

// "x y z zone median left right crossing link link ..." - eight fixed columns then the links.
bool ReadNodeFile(RegionState& rs) {
    FILE* f = fopen(rs.region->carNodeFile.c_str(), "rb");
    if (!f) {
        Log::Write("[%s] traffic: %s cannot be opened", rs.region->name.c_str(),
                   rs.region->carNodeFile.c_str());
        return false;
    }
    std::string text;
    char buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);

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
        if (t.size() < 8) { malformed++; continue; }
        Node n;
        n.x = (float)atof(t[0].c_str());
        n.y = (float)atof(t[1].c_str());
        n.z = (float)atof(t[2].c_str());
        // t[3] is the district and t[4] the median, neither of which traffic uses.
        int left = atoi(t[5].c_str());
        int right = atoi(t[6].c_str());
        // Lanes per direction. A road with lanes both ways gives the narrower side, so nothing is
        // put in a lane the other carriageway is using; a one-way road gives all of them, since
        // there is no other carriageway to take.
        int lanes = (left > 0 && right > 0) ? (left < right ? left : right) : (left + right);
        if (lanes < 1) lanes = 1;
        if (lanes > kMaxLanes) lanes = kMaxLanes;
        n.lanes = (uint8_t)lanes;
        n.firstLink = n.linkCount = 0;
        rs.nodes.push_back(n);
        std::vector<int> mine;
        for (size_t i = 8; i < t.size(); i++) mine.push_back(atoi(t[i].c_str()));
        linksPerNode.push_back(mine);
    }
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
        Log::Write("[%s] traffic: %d malformed line(s), %d link(s) naming a node that is not there",
                   rs.region->name.c_str(), malformed, dropped);
    return !rs.nodes.empty();
}

void BuildGrid(RegionState& rs) {
    for (size_t i = 0; i < rs.nodes.size(); i++) {
        float wx = rs.nodes[i].x + rs.region->originX;
        float wy = rs.nodes[i].y + rs.region->originY;
        rs.grid[CellKey((int)floorf(wx / kGridCell), (int)floorf(wy / kGridCell))].push_back((int)i);
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

// The centre of one lane at `at`, on a road running towards `towards`.
//
// The offset is always to the right of the direction of travel, which is what separates the two
// directions onto their own sides without this having to know which side of the road the city
// drives on: a car coming the other way offsets to the right of ITS travel, which is the other
// side of the same centre line.
CVector LanePoint(const RegionState& rs, int at, int towards, int lane) {
    CVector a = NodeWorld(rs, at);
    CVector b = NodeWorld(rs, towards);
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return a;
    dx /= len;
    dy /= len;
    float offset = ((float)lane + 0.5f) * kLaneWidth;
    return CVector(a.x + dy * offset, a.y - dx * offset, a.z);
}

void NodesNear(const RegionState& rs, const CVector& at, float radius, std::vector<int>& out) {
    int cells = (int)ceilf(radius / kGridCell);
    int cx = (int)floorf(at.x / kGridCell);
    int cy = (int)floorf(at.y / kGridCell);
    float r2 = radius * radius;
    for (int dx = -cells; dx <= cells; dx++) {
        for (int dy = -cells; dy <= cells; dy++) {
            auto it = rs.grid.find(CellKey(cx + dx, cy + dy));
            if (it == rs.grid.end()) continue;
            for (int n : it->second) {
                CVector p = NodeWorld(rs, n);
                float ax = p.x - at.x, ay = p.y - at.y;
                if (ax * ax + ay * ay <= r2) out.push_back(n);
            }
        }
    }
}

bool PlayerOver(const RegionState& rs, const CVector& at, float margin) {
    return at.x >= rs.minX - margin && at.x <= rs.maxX + margin &&
           at.y >= rs.minY - margin && at.y <= rs.maxY + margin;
}

// San Andreas' own map, which owns its own traffic and must not have ours put on top of it.
//
// This is the seam rule, and it is written down rather than left to happen. San Andreas generates
// traffic from its path nodes, which exist only inside +/-3000; this module generates traffic from
// a region's carriageways, which begin at x 4176 for Vice City and y 4389 for Liberty City. The
// two sets do not overlap, so in practice neither ever spawns on the other's ground - but "in
// practice" depends on where a region's ini puts it, and a region placed over San Andreas would
// otherwise give one stretch of road two lots of traffic from two systems that cannot see each
// other's cars. So the boundary is checked rather than assumed.
//
// What this does NOT do is fill the gap between the maps. There is no road there - about 1170
// units of open ground unpacked, 500 with PackRegions - so traffic thins out at San Andreas' edge
// exactly as it does today, and picks up again when the imported city's roads begin. A road
// between the two would be content, not code.
constexpr float kVanillaMapEdge = 3000.0f;

bool SanAndreasOwnsThis(const CVector& at) {
    return at.x > -kVanillaMapEdge && at.x < kVanillaMapEdge &&
           at.y > -kVanillaMapEdge && at.y < kVanillaMapEdge;
}

// The next node to head for, preferring to carry straight on rather than double back. A dead end
// turns the car round, which is what the game's own traffic does when a road runs out.
int NextNode(const RegionState& rs, int from, int to) {
    const Node& n = rs.nodes[to];
    if (n.linkCount <= 0) return from;
    int choices[16];
    int count = 0;
    for (int i = 0; i < n.linkCount && count < 16; i++) {
        int candidate = rs.links[n.firstLink + i];
        if (candidate != from) choices[count++] = candidate;
    }
    if (count == 0) return from;   // a dead end: turn round
    return choices[rand() % count];
}

// ------------------------------------------------------------------ making one drive

// San Andreas' own idea of how many cars belong here, rather than a number invented in this file.
//
// CCarCtrl::GenerateOneRandomCar gates on the sum of four floats at 0xC0BC30..0xC0BC3C, which
// CPopCycle::UpdatePercentages fills from popcycle.dat for the zone the player is standing in. Out
// here that resolves to info zone 0 - the whole-map SAN_AND - because CTheZones' lookup falls back
// to it, so the row is San Andreas' own and the number means the same thing it means at home.
constexpr uintptr_t kCarGroupBudget = 0xC0BC30;   // four floats
constexpr uintptr_t kCarDensityMultiplier = 0x8A5B20;
constexpr int kMaxCars = 24;
constexpr int kVehiclePoolReserve = 6;

int CarBudget() {
    float wanted = 0.0f;
    for (int i = 0; i < 4; i++) wanted += *(float*)(kCarGroupBudget + (uintptr_t)i * 4);
    wanted *= *(float*)kCarDensityMultiplier;
    wanted *= Settings::world.regionTrafficDensity;
    int budget = (int)wanted;
    if (budget < 0) budget = 0;
    if (budget > kMaxCars) budget = kMaxCars;
    return budget;
}

// The placeholder fleet. San Andreas' own models stand in until the source game's cars are
// imported, exactly as its peds stand in for the source game's people - so this is a list of
// ordinary traffic that any San Andreas install has, and nothing is requested that the streamer
// has not already brought in for its own traffic. Picking from what is loaded rather than asking
// for something keeps this out of the streamer's way entirely.
const int kPlaceholderCars[] = {
    400,  // Landstalker
    401,  // Bravura
    404,  // Perennial
    410,  // Manana
    412,  // Voodoo
    418,  // Moonbeam
    419,  // Esperanto
    421,  // Washington
    426,  // Premier
    436,  // Previon
    445,  // Admiral
    466,  // Glendale
    467,  // Oceanic
    474,  // Hermes
    475,  // Sabre
    492,  // Greenwood
    516,  // Nebula
    517,  // Majestic
    518,  // Buccaneer
    527,  // Cadrona
};

int ChooseLoadedCar() {
    const int count = (int)(sizeof(kPlaceholderCars) / sizeof(int));
    int start = rand() % count;
    for (int i = 0; i < count; i++) {
        int model = kPlaceholderCars[(start + i) % count];
        if (CStreaming::HasModelLoaded(model)) return model;
    }
    return -1;   // nothing of ours is in memory; wait rather than fetch and stall the streamer
}

bool RoomForACar() {
    auto* pool = CPools::ms_pVehiclePool;
    return pool && (int)pool->GetNoOfFreeSpaces() > kVehiclePoolReserve;
}

// Point the car at the far end of the road it is on, in its own lane, and let the game drive.
//
// The obvious thing here would be CCarCtrl::JoinCarWithRoadSystemGotoCoors, which is what San
// Andreas itself calls when it puts a car on a goto-coordinates mission. It must not be used.
// Reading its body (0x156A4A0, behind the HOODLUM jump at 0x42F870) it copies the destination into
// the autopilot and then goes on to fill m_aPathFindNodesInfo - it builds a PATH NODE ROUTE to the
// destination, and it does so with 0x497423FE, about a million, as the search radius. Out here
// that would find San Andreas' nearest node thousands of units away and hand every car a route
// leading off the edge of the city.
//
// So the destination is written directly and the route is emptied instead. MISSION_GOTOCOORDS_-
// STRAIGHT is the variant that steers for the destination rather than following a route, which is
// exactly what a car with no route needs; the node addresses are set to the game's own empty value
// so that anything which does look at them sees "none" rather than a stale area index.
void SetDestination(CVehicle* car, const RegionState& rs, const Driver& d) {
    CVector target = LanePoint(rs, d.to, NextNode(rs, d.from, d.to), d.lane);
    CAutoPilot& pilot = car->m_autoPilot;
    pilot.m_vecDestinationCoors = target;
    pilot.m_nCarMission = (eCarMission)kMissionGotoCoordsStraight;
    pilot.m_nPathFindNodesCount = 0;
    pilot.m_currentAddress.Clear();
    pilot.m_startingRouteNode.Clear();
    pilot.field_8.Clear();
}

// CCarCtrl::CreateCarForScript(modelId, posn, missionCleanup) and CPopulation::AddPedInCar, which
// are called rather than patched - but checked the same way anything else here is checked, because
// calling the wrong address is worse than not calling it.
constexpr uintptr_t kCreateCarForScript = 0x431F80;
constexpr uintptr_t kAddPedInCar = 0x613A00;
const uint8_t kCreateCarPrologue[] = { 0x6A, 0xFF, 0x68, 0xBB, 0x74, 0x83, 0x00 };
const uint8_t kAddPedInCarPrologue[] = { 0x83, 0xEC, 0x18, 0x53, 0x56, 0x57 };
bool gCanDrive = false;

bool VerifyCall(uintptr_t address, const uint8_t* expected, size_t length, const char* what) {
    uint8_t here[8] = {};
    memcpy(here, (const void*)address, length);
    if (memcmp(here, expected, length) == 0) return true;
    Log::Write("traffic: %s at 0x%X does not read as expected (%02X %02X %02X %02X...); no cars "
               "will be put on the region's roads", what, (unsigned)address,
               here[0], here[1], here[2], here[3]);
    return false;
}

CVehicle* CreateCar(RegionState& rs, int from, int to, int lane, int model) {
    CVector at = LanePoint(rs, from, to, lane);
    // CreateCarForScript does the whole placement properly - ground height, clearing space, adding
    // to the world - and one thing that is wrong for us: at 0x43206C it calls
    // CCarCtrl::JoinCarWithRoadSystem, the PATH-BASED join, which will attach the car to whichever
    // San Andreas node the clamped area search happens to find. That is harmless and it is undone,
    // because SetDestination below overwrites the mission and clears all three node addresses a
    // moment later. It is written down because it looks like a loose end and is not one: doing the
    // placement by hand instead would mean reproducing four calls this already gets right.
    CVehicle* car = CCarCtrl::CreateCarForScript(model, at, false);
    if (!car) return nullptr;
    // A random traffic car, not a mission one: this is what makes San Andreas remove it on its own
    // terms rather than keeping it for ever.
    car->m_nCreatedBy = kVehicleCreatedByRandom;
    // Stop for other cars. Without traffic lights this is the whole of the give-way behaviour at a
    // junction, and it is the game's own - the same field its own traffic uses.
    car->m_autoPilot.m_nCarDrivingStyle = (eCarDrivingStyle)kDrivingStyleStopForCars;
    car->m_autoPilot.m_nCruiseSpeed = 18;
    // Face along the road rather than along whatever axis the constructor left it on.
    CVector b = NodeWorld(rs, to);
    car->SetHeading(atan2f(-(b.x - at.x), b.y - at.y));
    CPopulation::AddPedInCar(car, true, -1, 0, true, false);
    rs.drivers.push_back(Driver{ CPools::GetVehicleRef(car), from, to, lane });
    SetDestination(car, rs, rs.drivers.back());
    return car;
}

// ------------------------------------------------------------------ the frame

void ManageDrivers(RegionState& rs, const CVector& player) {
    const float cull = Settings::world.regionTrafficRadius * kCullMargin;
    size_t write = 0;
    for (size_t i = 0; i < rs.drivers.size(); i++) {
        Driver& d = rs.drivers[i];
        CVehicle* car = CPools::GetVehicle(d.ref);
        if (!car) continue;                      // the game removed it; the slot may be reused
        CVector p = car->GetPosition();
        float dx = p.x - player.x, dy = p.y - player.y;
        if (dx * dx + dy * dy > cull * cull && !TheCamera.IsSphereVisible(p, 4.0f)) {
            CWorld::Remove(car);
            delete car;
            continue;
        }
        // Once the player is driving it, it is the player's car and none of this module's business.
        if (car->m_pDriver && car->m_pDriver == (CPed*)FindPlayerPed()) continue;

        CVector goal = NodeWorld(rs, d.to);
        float gx = goal.x - p.x, gy = goal.y - p.y;
        float toGoal = gx * gx + gy * gy;
        if (toGoal > kStrayDistance * kStrayDistance) {
            // Shunted off its road, most likely by the player. Rather than drag it back across a
            // gap it cannot drive, hand it to the game: it keeps whatever mission it has and gets
            // culled with everything else.
            continue;
        }
        if (toGoal < kArrivedDistance * kArrivedDistance) {
            int next = NextNode(rs, d.from, d.to);
            d.from = d.to;
            d.to = next;
            if (d.lane >= rs.nodes[d.to].lanes) d.lane = rs.nodes[d.to].lanes - 1;
            SetDestination(car, rs, d);
        }
        rs.drivers[write++] = d;
    }
    rs.drivers.resize(write);
}

void SpawnSome(RegionState& rs, const CVector& player, int wanted) {
    if (!gCanDrive) return;
    if ((int)rs.drivers.size() >= wanted) return;
    if (CTimer::m_snTimeInMilliseconds < gNextSpawn) return;
    gNextSpawn = CTimer::m_snTimeInMilliseconds + kSpawnIntervalMs;
    if (!RoomForACar()) return;
    int model = ChooseLoadedCar();
    if (model < 0) return;

    std::vector<int> candidates;
    NodesNear(rs, player, Settings::world.regionTrafficRadius, candidates);
    if (candidates.empty()) return;
    int placed = 0;
    for (int attempt = 0; attempt < 20 && placed < kSpawnsPerTick; attempt++) {
        int from = candidates[rand() % candidates.size()];
        const Node& n = rs.nodes[from];
        if (n.linkCount <= 0) continue;
        int to = rs.links[n.firstLink + (rand() % n.linkCount)];
        int lane = rand() % (int)n.lanes;
        CVector at = LanePoint(rs, from, to, lane);
        // The seam: inside San Andreas' own map its own traffic generator owns the road, and two
        // systems that cannot see each other's cars must not both be putting cars on it.
        if (SanAndreasOwnsThis(at)) continue;
        float dx = at.x - player.x, dy = at.y - player.y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < kMinSpawnDistance) continue;
        // Never where the player is looking: a car appearing in shot is the one thing that reads
        // as a spawn rather than as traffic.
        if (TheCamera.IsSphereVisible(at, 4.0f)) continue;
        // Not on top of another of ours.
        bool crowded = false;
        for (const Driver& other : rs.drivers) {
            CVehicle* v = CPools::GetVehicle(other.ref);
            if (!v) continue;
            CVector q = v->GetPosition();
            float ox = q.x - at.x, oy = q.y - at.y;
            if (ox * ox + oy * oy < 100.0f) { crowded = true; break; }
        }
        if (crowded) continue;
        if (CreateCar(rs, from, to, lane, model)) placed++;
    }
}

}   // namespace

namespace RegionTraffic {

void Tick(const CVector& playerPosition) {
    if (!Settings::world.regionTraffic) return;
    for (RegionState& rs : Regions()) {
        if (!PlayerOver(rs, playerPosition, Settings::world.regionTrafficRadius * kCullMargin)) {
            // Out of this region: let go of everybody. The game removes them on its own terms once
            // they are far enough away, and they are not this module's business any more.
            rs.drivers.clear();
            continue;
        }
        ManageDrivers(rs, playerPosition);
        SpawnSome(rs, playerPosition, CarBudget());
        break;
    }
}

int Count() {
    int n = 0;
    for (const RegionState& rs : Regions()) n += (int)rs.drivers.size();
    return n;
}

void Forget() {
    for (RegionState& rs : Regions()) rs.drivers.clear();
}

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.carNodeFile.empty();
    if (!any || !Settings::world.regionTraffic) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region traffic is off, the server owns the vehicles");
        return;
    }

    for (const MapRegion& r : Settings::regions) {
        if (r.carNodeFile.empty()) continue;
        RegionState rs;
        rs.region = &r;
        if (!ReadNodeFile(rs)) continue;
        BuildGrid(rs);
        Log::Write("[%s] traffic: %u node(s), %u link(s) of carriageway over x %.0f..%.0f "
                   "y %.0f..%.0f", r.name.c_str(), (unsigned)rs.nodes.size(),
                   (unsigned)rs.links.size(), rs.minX, rs.maxX, rs.minY, rs.maxY);
        Regions().push_back(std::move(rs));
    }
    if (Regions().empty()) return;

    gCanDrive = VerifyCall(kCreateCarForScript, kCreateCarPrologue, sizeof(kCreateCarPrologue),
                           "CCarCtrl::CreateCarForScript") &&
                VerifyCall(kAddPedInCar, kAddPedInCarPrologue, sizeof(kAddPedInCarPrologue),
                           "CPopulation::AddPedInCar");
    if (!gCanDrive) {
        Regions().clear();
        return;
    }
    Log::Write("traffic: %u region(s) will be driven on their own carriageways, up to %d car(s) "
               "within %.0f units", (unsigned)Regions().size(), CarBudget(),
               Settings::world.regionTrafficRadius);
}

}   // namespace RegionTraffic
