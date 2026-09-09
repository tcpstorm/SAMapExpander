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
#include "Ground.h"
#include "VehicleSpawn.h"
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
// How much further than the spawn radius a car may get before it is taken away.
//
// This was 1.4, and 1.4 was not enough, because the spawn radius is not the distance a car is
// actually created at. NodesNear finds nodes within RegionTrafficRadius - 90 by default - and then
// LanePoint steps sideways off the node by (lane + 0.5) * 5.4, which is another 18.9 units in the
// outer lane of a four-lane road. So cars were being made as far out as 109 and taken away again
// at 126: a seventeen-unit gap, which an eighteen-unit cruise speed crosses in about a second.
// That is what "3 placed, 3 taken away, 0 driving" was, every second, for the whole session.
//
// San Andreas' own numbers are the shape to copy: it creates between about 42 and 50 and removes
// past 65, so its gap is a third of its spawn radius rather than a sixth. Two changes make ours
// the same shape - the lane offset now counts against the spawn radius (see SpawnSome) so nothing
// is created past 90, and the cull sits at 1.75 of it, 157, which is 67 units of room to drive in.
constexpr float kCullMargin = 1.75f;
constexpr int kSpawnsPerTick = 1;
constexpr unsigned kSpawnIntervalMs = 400;
// How near the destination counts as arrived, and therefore when the next leg is handed out. Wider
// than a lane so a car that clipped a kerb still counts as having got there.
constexpr float kArrivedDistance = 8.0f;
// A car this far from the road it is supposed to be on has lost its way - shunted by the player,
// most likely - and is given back to the game rather than dragged to a node it can no longer reach.
constexpr float kStrayDistance = 45.0f;

// How far a car may be from the point it was asked for and still count as having gone there.
// Generous - CreateCarForScript settles it onto the road and clears space around it, so a few
// units of movement is that routine doing its job. Anything past this is not settling.
constexpr float kMaxCreationSlip = 12.0f;

// Where a tick's spawn attempts went. "0 car(s) driving its carriageways" is a count, and a count
// cannot say whether the budget was full, no model was in memory, the pool was full, no node was
// in range, every node was in shot, or the car went somewhere else entirely. Each of those is a
// different bug and they were guessed at four times over. Summed and reported once a second under
// Debug, which is how the last one was finally caught.
struct Tally {
    int ticks, notReady, atBudget, waiting, poolFull, noModel, noNodes;
    int attempts, deadEnd, onSaGround, tooNear, tooFar, inShot, crowded, noGround, madeNothing, placed;
    int lost, gaveBack, tookAway, noPlayerGround, wentElsewhere, advanced;
    float tookAwayFurthest, tookAwayDeepestFall;
    unsigned nextReport;
};
Tally& Counts() { static Tally t{}; return t; }

// Where the player was this tick, for log lines deep in the creation path that would otherwise
// have to be handed it through calls that do not otherwise need it.
CVector& LastPlayer() { static CVector v; return v; }

// CAutoPilot::m_vecDestinationCoors, as the GAME reads it.
//
// plugin-sdk puts m_autoPilot at CVehicle+0x390 and m_vecDestinationCoors at CAutoPilot+0x5C, so
// +0x3EC - and the mission handler agrees: at 0x437420 and 0x437419 it loads [esi+0x3EC] and
// [esi+0x3F0] and pushes them into CCarCtrl::SteerAICarWithPhysicsHeadingForTarget. The
// diagnostic below reads the raw offset rather than the plugin-sdk field on purpose. Twice today a
// number was believed because the code printed what it MEANT to write; the only way to know what a
// car is steering for is to read it from where the steering reads it.
constexpr uintptr_t kAutoPilotDestination = 0x3EC;

// A few cars a second, no more - this is a positional fact per line, which is worth more than a
// rate, but not worth a thousand lines a second.
constexpr unsigned kDriverReportIntervalMs = 350;
unsigned gNextDriverReport = 0;

bool ReportOneDriver() {
    if (!Settings::world.debug) return false;
    if (CTimer::m_snTimeInMilliseconds < gNextDriverReport) return false;
    gNextDriverReport = CTimer::m_snTimeInMilliseconds + kDriverReportIntervalMs;
    return true;
}

// MISSION_GOTOCOORDS_STRAIGHT is the ONLY mission that drives a car towards an arbitrary
// coordinate without touching the path graph. Its handler at 0x4373FB calls
// CCarCtrl::SteerAICarWithPhysicsHeadingForTarget, which is clean of CPathFind three call levels
// deep. Missions 1 CRUISE, 8 GOTOCOORDS and 12 GOTOCOORDS_ACCURATE all route through
// SteerAICarWithPhysicsFollowPath and must never be set on one of these cars.
constexpr char kMissionGotoCoordsStraight = 9;

// Driving style 6, and it has to be exactly 6.
//
// CCarCtrl::FindAngleToWeaveThroughTraffic - the lateral avoidance that steers a car around
// another car, a ped or an object - is called from HeadingForTarget only for styles 2, 5 and 6.
// CCarCtrl::FindMaximumSpeedForThisCarInTraffic, the longitudinal one, early-outs for styles 2, 3
// and 5. Six is the only value that satisfies both, so it is the only one that gets a car both
// halves of the game's own avoidance. Style 0, which this used to set, gets neither.
constexpr char kDrivingStyleWeave = 6;

// Which of San Andreas' car counters ours sit on, why it is PERMANENT and not RANDOM, how a model
// is found to be in memory, and how a car of ours is taken back out of the world, all live in
// VehicleSpawn.cpp now - because the parked cars need every one of the same facts, and one of them
// (PARKED being a budget of five) was only found while writing them.
//
// CCarCtrl::FindMaximumSpeedForThisCarInTraffic. cdecl, one argument, verified free of CPathFind,
// and unreachable from our mission - so it is called directly to get back the one piece of
// avoidance the mission does not provide: not driving into the back of the car in front.
constexpr uintptr_t kFindMaxSpeedInTraffic = 0x434400;

// eEntityStatus STATUS_PHYSICS. CAutomobile::ProcessAI dispatches on this, and only 3 (and 12)
// reach AI::UpdateCarAI followed by CCarCtrl::SteerAICarWithPhysics. STATUS_SIMPLE runs
// UpdateCarOnRails instead, which is entirely path-driven and useless here.
constexpr unsigned char kStatusPhysics = 3;

// Two bytes in CAutoPilot that decide whether the game quietly puts the car back on the path graph.
//
// +0x3DD is m_nStraightLineDistance, and AI::UpdateCarAI checks the distance to the destination
// against it plus five: further than that and, one frame in eight, it runs a real
// CPathFind::DoPathSearch and may flip the mission to the path-following 8. The default is 20,
// which our node spacing regularly exceeds, so this would have happened constantly. 255 puts it
// past any leg we hand out.
//
// +0x3DC bit 0 arms a re-join branch at the top of SteerAICarWithPhysics that calls
// AreNodesLoadedForArea and then JoinCarWithRoadAccordingToMission. It is set by
// StopCarIfNodesAreInvalid, which only a path mission reaches - but it is one byte to be certain.
constexpr uintptr_t kAutoPilotStraightLineDistance = 0x3DD;
constexpr uintptr_t kAutoPilotRejoinFlags = 0x3DC;
constexpr unsigned char kStraightLineForever = 255;

// Ordinary town speed, and the crawl a car drops to while it is waiting its turn at a junction.
constexpr char kCruiseSpeed = 18;
constexpr char kYieldSpeed = 3;

struct Node {
    float x, y, z;
    int firstLink, linkCount;
    uint8_t lanes;      // per direction, at least 1
};

// One car this module is looking after. `from` and `to` are the road it is on; `lane` is which
// lane of it, and does not change for the life of the car.
struct Driver {
    int ref;            // CPools vehicle handle: slot plus re-use counter
    int model;          // what it was created as; half of the proof that it is still ours
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

// Is this vehicle model actually in memory? VehicleSpawn::ModelIsLoaded reads the streaming flag
// straight out of ms_aInfoForModel, because in 1.0 US there is no HasModelLoaded to call and
// getting the test wrong faults inside CVehicleModelInfo::CreateInstance at 0x4C9691.
int ChooseLoadedCar() {
    const int count = (int)(sizeof(kPlaceholderCars) / sizeof(int));
    int start = rand() % count;
    for (int i = 0; i < count; i++) {
        int model = kPlaceholderCars[(start + i) % count];
        if (VehicleSpawn::ModelIsLoaded(model)) return model;
    }
    return -1;   // nothing of ours is in memory; wait rather than fetch and stall the streamer
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
    // The three packed link ids, whose empty value is 0xFFFF. They are a bitfield pair in
    // plugin-sdk with no setter, so they are written as the words they are - which is what the
    // game's own JoinCarWithRoadSystem does to them too.
    *(uint16_t*)((char*)car + 0x3A4) = 0xFFFF;
    *(uint16_t*)((char*)car + 0x3A6) = 0xFFFF;
    *(uint16_t*)((char*)car + 0x3A8) = 0xFFFF;
    // Keep the game from quietly putting the car back on the path graph behind our backs. Both of
    // these are re-applied every tick, not just here, because both are things the game's own AI
    // will set again given the chance.
    *((unsigned char*)car + kAutoPilotStraightLineDistance) = kStraightLineForever;
    *((unsigned char*)car + kAutoPilotRejoinFlags) &= (unsigned char)~1u;
}

// CCarCtrl::CreateCarForScript(modelId, posn, missionCleanup) and CPopulation::AddPedInCar are
// called rather than patched - but checked the same way anything else here is checked, because
// calling the wrong address is worse than not calling it. VehicleSpawn::Ready() is that check,
// shared with the parked cars, which call the same two.
// The height a car is created at is NOT the height the graph claims - and it is not the -100
// sentinel either, which is what this tried first and why it is worth writing down.
//
// CCarCtrl::CreateCarForScript compares the z it is given against the float at 0x859014, which is
// -100.0 exactly (the test is at 0x432117 on the car path), and where they match it works the
// height out itself. That looks like the right thing to lean on until you see WHICH routine it
// calls: CWorld::FindGroundZForCoord, which returns the float at 0x858BA4 - **twenty** - when it
// finds nothing, and twenty is an ordinary street height in an imported city. Handing over the
// sentinel just moves the lie one level down.
//
// So the ground is found here, honestly, through Ground::Under, and a lane point with no floor
// under it is not used at all. See the top of src/region/Ground.cpp.
CVehicle* CreateCar(RegionState& rs, int from, int to, int lane, int model) {
    CVector at = LanePoint(rs, from, to, lane);
    float ground = 0.0f;
    if (!Ground::Under(at.x, at.y, at.z, ground)) { Counts().noGround++; return nullptr; }
    at.z = ground;
    // CreateCarForScript does the whole placement properly - ground height, clearing space, adding
    // to the world - and one thing that is wrong for us: at 0x43206C it calls
    // CCarCtrl::JoinCarWithRoadSystem, the PATH-BASED join, which will attach the car to whichever
    // San Andreas node the clamped area search happens to find. That is harmless and it is undone,
    // because SetDestination below overwrites the mission and clears all three node addresses a
    // moment later. It is written down because it looks like a loose end and is not one: doing the
    // placement by hand instead would mean reproducing four calls this already gets right.
    CVehicle* car = VehicleSpawn::Create(model, at, false);
    if (!car) return nullptr;
    // Did it go where we asked?
    //
    // This check exists because a car of ours turned up 6455 units from the player - the distance
    // between an imported city and San Andreas - sitting exactly where it had been put, and the
    // only reason anybody noticed was that the cull tally had been asked to record a distance. A
    // car dropped into a live Los Santos street wearing a donor-paired placeholder model is
    // indistinguishable from "the wrong vehicles are spawning for the zone", and it gets hit by
    // San Andreas' own traffic, which is indistinguishable from "cars arrive damaged". Both of
    // those were chased for a day as streaming faults.
    //
    // So the position is read back and compared with the one asked for, every time, and a car that
    // is not where it was put is removed at once and says so loudly. Whatever moves it - and this
    // is deliberately agnostic about that, because the answer has been guessed wrong twice - it
    // can no longer be left standing in San Andreas.
    CVector landed = car->GetPosition();
    float slipX = landed.x - at.x, slipY = landed.y - at.y;
    float slip = sqrtf(slipX * slipX + slipY * slipY);
    if (slip > kMaxCreationSlip) {
        Log::Write("traffic: asked for a car at (%.1f, %.1f, %.1f) and got one at (%.1f, %.1f, "
                   "%.1f) - %.0f units away, %.0f from the player. Refusing it: a car of ours "
                   "standing where it was not put is a car standing in somebody else's city",
                   at.x, at.y, at.z, landed.x, landed.y, landed.z, slip,
                   sqrtf((landed.x - LastPlayer().x) * (landed.x - LastPlayer().x) +
                         (landed.y - LastPlayer().y) * (landed.y - LastPlayer().y)));
        VehicleSpawn::Remove(car);
        Counts().wentElsewhere++;
        return nullptr;
    }
    // Through the game's own setter, which moves the car between the counters CCarCtrl budgets
    // against. The constructor has already counted it once as a mission car.
    VehicleSpawn::MakePermanent(car);
    car->m_nStatus = (eEntityStatus)kStatusPhysics;
    car->m_autoPilot.m_nCarDrivingStyle = (eCarDrivingStyle)kDrivingStyleWeave;
    car->m_autoPilot.m_nCruiseSpeed = kCruiseSpeed;
    // Face along the road rather than along whatever axis the constructor left it on.
    CVector b = NodeWorld(rs, to);
    car->SetHeading(atan2f(-(b.x - at.x), b.y - at.y));
    CPopulation::AddPedInCar(car, true, -1, 0, true, false);
    rs.drivers.push_back(Driver{ CPools::GetVehicleRef(car), model, from, to, lane });
    SetDestination(car, rs, rs.drivers.back());
    return car;
}

// Taking a car back out of the world is VehicleSpawn::Remove - never with C++ `delete`, for the
// reason written down there.

// ------------------------------------------------------------------ giving way at a junction
//
// What San Andreas does for us, checked rather than assumed:
//
//   CCarCtrl::ScanForPedDanger is called from CVehicle::ProcessControl itself - three call sites -
//   so a car brakes for pedestrians whatever mission it is on. That one is free.
//
//   CCarCtrl::FindMaximumSpeedForThisCarInTraffic, which is what slows a car for the car in front,
//   has exactly two callers: SteerAICarWithPhysicsFollowPath and SlowCarOnRailsDownForTrafficAndLights.
//   Both of those are the PATH-FOLLOWING steer. A car on a go-to-coordinates mission never reaches
//   it, so setting DRIVINGSTYLE_STOP_FOR_CARS - which is still done, because it costs nothing and
//   is right if the mission ever routes there - does not on its own make two of our cars give way
//   to each other. They would still collide rather than pass through one another, since physics is
//   physics, but arriving at a crossroads together they would simply crash.
//
// So the give-way is done here, and cheaply. A car approaching a node with three or more links is
// approaching a junction; if another of ours is nearer to that same node than it is, it waits. The
// rule is a total order - nearer wins, and the pool handle breaks a tie - so of any two cars
// exactly one yields and they cannot deadlock by both giving way.
constexpr int kJunctionLinks = 3;
constexpr float kJunctionWatch = 26.0f;

bool ShouldYield(const RegionState& rs, const Driver& me, const CVector& myPos) {
    const Node& node = rs.nodes[me.to];
    if (node.linkCount < kJunctionLinks) return false;
    CVector at = NodeWorld(rs, me.to);
    float mx = at.x - myPos.x, my = at.y - myPos.y;
    float mine = mx * mx + my * my;
    if (mine > kJunctionWatch * kJunctionWatch) return false;
    for (const Driver& other : rs.drivers) {
        if (other.ref == me.ref) continue;
        if (other.to != me.to) continue;          // heading somewhere else entirely
        CVehicle* v = VehicleSpawn::Resolve(other.ref, other.model);
        if (!v) continue;
        CVector p = v->GetPosition();
        float ox = at.x - p.x, oy = at.y - p.y;
        float theirs = ox * ox + oy * oy;
        if (theirs > kJunctionWatch * kJunctionWatch) continue;
        if (theirs < mine) return true;
        if (theirs == mine && other.ref < me.ref) return true;   // a tie, broken the same way twice
    }
    return false;
}

// ------------------------------------------------------------------ the frame

// Every way a car stops being ours goes through one of these two, and there is no third.
//
// GIVING ONE BACK is VehicleSpawn::HandBackToTheGame, and forgetting a car without calling it is
// the bug this module shipped: CCarCtrl::CreateCarForScript SETS bIsLocked at 0x43205F - "locked
// by the script (cannot be removed)" - and PERMANENT is a counter nothing budgets against, so a
// car we stop tracking and do not hand back is a car San Andreas can never take away. It holds a
// vehicle pool slot for the rest of the session, and - this is the part that reaches Los Santos -
// it holds a reference on its model, and CStreaming::RemoveLoadedVehicle refuses at 0x40C052 to
// unload a model whose CBaseModelInfo::m_nRefCount (+8) is not zero. Enough of those and
// CStreaming::LoadZoneVehicle cannot rotate the loaded set as the player crosses zones, so
// CCarCtrl::ChooseModel keeps picking from cars loaded for somewhere else entirely.
void LetGo(const Driver& d, const CVector& player) {
    CVehicle* car = VehicleSpawn::Resolve(d.ref, d.model);
    if (!car) return;
    CVector p = car->GetPosition();
    float dx = p.x - player.x, dy = p.y - player.y;
    const float cull = Settings::world.regionTrafficRadius * kCullMargin;
    // Out of sight and out of range: take it away rather than hand it back, so that leaving a
    // city does not put a dozen entries on NumRandomCars at once - the counter GenerateRandomCars
    // stops at 45. Anything the player might still be looking at is handed back instead and lives
    // out its life as an ordinary car.
    if (dx * dx + dy * dy > cull * cull && !TheCamera.IsSphereVisible(p, 4.0f))
        VehicleSpawn::Remove(car);
    else
        VehicleSpawn::HandBackToTheGame(car);
}

// TAKING ONE AWAY is VehicleSpawn::Remove, and only for a car far enough off that nobody is
// looking. That one is still ours at the moment it goes, so nothing has to be handed back.
void ManageDrivers(RegionState& rs, const CVector& player) {
    const float cull = Settings::world.regionTrafficRadius * kCullMargin;
    size_t write = 0;
    for (size_t i = 0; i < rs.drivers.size(); i++) {
        Driver& d = rs.drivers[i];
        // Resolve rather than CPools::GetVehicle: the handle alone would let a re-used pool slot
        // put one of San Andreas' own cars under everything below. See VehicleSpawn::Resolve.
        CVehicle* car = VehicleSpawn::Resolve(d.ref, d.model);
        // Gone, or the pool slot was re-used under the handle. Counted, because a car of ours that
        // stops resolving the tick after it was made is a very different bug from one that was
        // never made at all, and the two read the same in a population count.
        if (!car) { Counts().lost++; continue; }
        CVector p = car->GetPosition();
        float dx = p.x - player.x, dy = p.y - player.y;
        float away = sqrtf(dx * dx + dy * dy);
        if (away > cull && !TheCamera.IsSphereVisible(p, 4.0f)) {
            VehicleSpawn::Remove(car);
            Counts().tookAway++;
            // How far it had got, and how far it had fallen. A car that drove out of range and one
            // that was thrown out of range by physics both read as "taken away", and the two are
            // different bugs: 160 units from a 157-unit cull is a car that drove; 400 is a car
            // that was ejected, and a big drop in z says it was ejected out of the ground.
            if (away > Counts().tookAwayFurthest) Counts().tookAwayFurthest = away;
            float fell = NodeWorld(rs, d.from).z - p.z;
            if (fell > Counts().tookAwayDeepestFall) Counts().tookAwayDeepestFall = fell;
            continue;
        }
        // Once the player is driving it, it is the player's car and none of this module's
        // business - but it has to be GIVEN back, not just forgotten, or nothing in San Andreas
        // will ever be able to remove it again.
        if (car->m_pDriver && car->m_pDriver == (CPed*)FindPlayerPed()) {
            VehicleSpawn::HandBackToTheGame(car);
            Counts().gaveBack++;
            continue;
        }

        CVector goal = NodeWorld(rs, d.to);
        float gx = goal.x - p.x, gy = goal.y - p.y;
        float toGoal = gx * gx + gy * gy;
        if (toGoal > kStrayDistance * kStrayDistance) {
            // Shunted off its road, most likely by the player. Rather than drag it back across a
            // gap it cannot drive, hand it to the game - which means handing it back properly, so
            // that the game's own distance cleanup can have it.
            VehicleSpawn::HandBackToTheGame(car);
            Counts().gaveBack++;
            continue;
        }
        if (toGoal < kArrivedDistance * kArrivedDistance) {
            // Counted, not changed. The median link in Vice City is 10.6 units long and a quarter
            // of them are shorter than 6.2 - both inside kArrivedDistance, which is 8 - so a car
            // may be "arrived" at a node it has not reached and skip on to the next. If that is
            // happening this number is far larger than one advance per car per link.
            Counts().advanced++;
            int next = NextNode(rs, d.from, d.to);
            d.from = d.to;
            d.to = next;
            if (d.lane >= rs.nodes[d.to].lanes) d.lane = rs.nodes[d.to].lanes - 1;
            SetDestination(car, rs, d);
        }
        // Hold the mission every frame, because the game's own AI will take it off us otherwise.
        // AI::UpdateCarAI sets m_nCarMission to 0 the moment a car gets within three metres of its
        // destination - which brakes it to a stop - and, for a destination further away than
        // m_nStraightLineDistance + 5, runs a path search one frame in eight that can flip it to
        // the path-following mission 8. Re-stamping all three costs nothing and settles both.
        CAutoPilot& pilot = car->m_autoPilot;
        pilot.m_nCarMission = (eCarMission)kMissionGotoCoordsStraight;
        *((unsigned char*)car + kAutoPilotStraightLineDistance) = kStraightLineForever;
        *((unsigned char*)car + kAutoPilotRejoinFlags) &= (unsigned char)~1u;

        // The one piece of avoidance the mission does not give us. Lateral weaving comes free from
        // driving style 6, but nothing on this mission slows a car for the one in front:
        // FindMaximumSpeedForThisCarInTraffic is reached only from the path-following steer. It is
        // cdecl, takes only the vehicle and is free of CPathFind, so it is called here instead and
        // its answer used as a ceiling on the cruise speed.
        char speed = ShouldYield(rs, d, p) ? kYieldSpeed : kCruiseSpeed;
        float clear = reinterpret_cast<float(__cdecl*)(void*)>(kFindMaxSpeedInTraffic)(car);
        if (clear >= 0.0f && clear < (float)speed) speed = (char)clear;
        pilot.m_nCruiseSpeed = speed;

        // One car, a few times a second: where it is, what the GAME thinks it is steering for, and
        // what this module thinks the node it is steering for is. If the last two disagree, the
        // route is being computed in one space and driven in another, and the size of the
        // disagreement names which - an origin is thousands, a lane offset is single figures, a
        // scale is proportional to the distance from the region's own zero. If they agree, the
        // route is right and the fault is in how it is followed.
        if (ReportOneDriver()) {
            const float* dest = (const float*)((const char*)car + kAutoPilotDestination);
            CVector node = NodeWorld(rs, d.to);
            CVector lane = LanePoint(rs, d.to, NextNode(rs, d.from, d.to), d.lane);
            Log::Write("traffic: car at (%.1f, %.1f, %.1f) is steering for (%.1f, %.1f) [read from "
                       "the vehicle at +0x3EC]; it is aiming at node %d, which this module puts at "
                       "(%.1f, %.1f, %.1f) and whose lane point is (%.1f, %.1f). car->dest %.1f, "
                       "car->node %.1f, dest->node %.1f. mission %d, straight-line %d, lane %d of "
                       "%d, link %d->%d",
                       p.x, p.y, p.z, dest[0], dest[1], d.to, node.x, node.y, node.z,
                       lane.x, lane.y,
                       sqrtf((dest[0] - p.x) * (dest[0] - p.x) + (dest[1] - p.y) * (dest[1] - p.y)),
                       sqrtf((node.x - p.x) * (node.x - p.x) + (node.y - p.y) * (node.y - p.y)),
                       sqrtf((dest[0] - node.x) * (dest[0] - node.x) +
                             (dest[1] - node.y) * (dest[1] - node.y)),
                       (int)pilot.m_nCarMission,
                       (int)*((const unsigned char*)car + kAutoPilotStraightLineDistance),
                       d.lane, (int)rs.nodes[d.to].lanes, d.from, d.to);
        }
        rs.drivers[write++] = d;
    }
    rs.drivers.resize(write);
}

void ReportTally(const RegionState* here) {
    Tally& t = Counts();
    if (!Settings::world.debug) return;
    if (CTimer::m_snTimeInMilliseconds < t.nextReport) return;
    t.nextReport = CTimer::m_snTimeInMilliseconds + 1000;
    if (!t.ticks) return;
    Log::Write("traffic/sec: %d tick(s) -> %d not ready, %d at budget, %d waiting for the spawn "
               "interval, %d pool full, %d no model in memory, %d no node in range; %d attempt(s) "
               "-> %d dead end, %d on San Andreas' own ground, %d too near, %d past the radius, "
               "%d in shot, %d crowded, %d no ground under the lane, %d placed; %d driving, "
               "%d vanished, %d handed back, %d taken away (furthest %.0f of %.0f, deepest fall "
               "%.1f), %d refused for landing somewhere else, %d tick(s) with no ground under "
               "the player; %d node(s) advanced",
               t.ticks, t.notReady, t.atBudget, t.waiting, t.poolFull, t.noModel, t.noNodes,
               t.attempts, t.deadEnd, t.onSaGround, t.tooNear, t.tooFar, t.inShot, t.crowded,
               t.noGround, t.placed, here ? (int)here->drivers.size() : 0, t.lost, t.gaveBack,
               t.tookAway, t.tookAwayFurthest, Settings::world.regionTrafficRadius * kCullMargin,
               t.tookAwayDeepestFall, t.wentElsewhere, t.noPlayerGround, t.advanced);
    Tally fresh{};
    fresh.nextReport = t.nextReport;
    t = fresh;
}

void SpawnSome(RegionState& rs, const CVector& player, int wanted) {
    Tally& t = Counts();
    t.ticks++;
    if (!VehicleSpawn::Ready()) { t.notReady++; return; }
    if ((int)rs.drivers.size() >= wanted) { t.atBudget++; return; }
    if (CTimer::m_snTimeInMilliseconds < gNextSpawn) { t.waiting++; return; }
    gNextSpawn = CTimer::m_snTimeInMilliseconds + kSpawnIntervalMs;
    if (!VehicleSpawn::PoolHasRoom(kVehiclePoolReserve)) { t.poolFull++; return; }
    int model = ChooseLoadedCar();
    if (model < 0) { t.noModel++; return; }

    std::vector<int> candidates;
    NodesNear(rs, player, Settings::world.regionTrafficRadius, candidates);
    if (candidates.empty()) { t.noNodes++; return; }
    int placed = 0;
    for (int attempt = 0; attempt < 20 && placed < kSpawnsPerTick; attempt++) {
        t.attempts++;
        int from = candidates[rand() % candidates.size()];
        const Node& n = rs.nodes[from];
        // A node with no lanes is not a road to put a car on - and `rand() % 0` is a divide by
        // zero, which is a hardware fault and not a bad spawn.
        if (n.linkCount <= 0 || n.lanes <= 0) { t.deadEnd++; continue; }
        int to = rs.links[n.firstLink + (rand() % n.linkCount)];
        int lane = rand() % (int)n.lanes;
        CVector at = LanePoint(rs, from, to, lane);
        // The seam: inside San Andreas' own map its own traffic generator owns the road, and two
        // systems that cannot see each other's cars must not both be putting cars on it.
        if (SanAndreasOwnsThis(at)) { t.onSaGround++; continue; }
        float dx = at.x - player.x, dy = at.y - player.y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist < kMinSpawnDistance) { t.tooNear++; continue; }
        // And not past the radius either. The node was within it; the lane offset can put the car
        // outside it, and a car created outside the band it is culled against has a very short
        // life - see kCullMargin.
        if (dist > Settings::world.regionTrafficRadius) { t.tooFar++; continue; }
        // Never where the player is looking: a car appearing in shot is the one thing that reads
        // as a spawn rather than as traffic.
        if (TheCamera.IsSphereVisible(at, 4.0f)) { t.inShot++; continue; }
        // Not on top of another of ours.
        bool crowded = false;
        for (const Driver& other : rs.drivers) {
            CVehicle* v = VehicleSpawn::Resolve(other.ref, other.model);
            if (!v) continue;
            CVector q = v->GetPosition();
            float ox = q.x - at.x, oy = q.y - at.y;
            if (ox * ox + oy * oy < 100.0f) { crowded = true; break; }
        }
        if (crowded) { t.crowded++; continue; }
        // CreateCar counts its own failures - there are two of them now, and inferring which from
        // a bool got them added together: a run reported "32 no ground under the lane" and "32
        // refused for landing somewhere else" which were the same 32 events, and the first label
        // was simply wrong. A counter must be incremented where the decision is made.
        if (CreateCar(rs, from, to, lane, model)) { placed++; t.placed++; }
    }
}

}   // namespace

namespace RegionTraffic {

void Tick(const CVector& playerPosition, bool playerHasGround) {
    if (!Settings::world.regionTraffic) return;
    LastPlayer() = playerPosition;
    // Every region every frame, and only then the one the player is standing over. The loop used
    // to stop at the first region it found the player over, which meant a region they had LEFT was
    // never visited again if another one came before it in the file - and F7 puts the player
    // straight from one city into another, with no frame in between where they are over neither.
    // Every car in the city they left would have stayed there for the session.
    RegionState* here = nullptr;
    const float margin = Settings::world.regionTrafficRadius * kCullMargin;
    for (RegionState& rs : Regions()) {
        if (!here && PlayerOver(rs, playerPosition, margin)) { here = &rs; continue; }
        if (rs.drivers.empty()) continue;
        // Out of this region: let go of everybody - by GIVING each one back or TAKING it away,
        // rather than dropping the list, which is what this used to do. Driving out of an imported
        // city is the commonest way a car of ours stops being tracked, so it was also the
        // commonest way one was left locked and permanent in a city the player is no longer in,
        // holding a pool slot and a model reference for the rest of the session.
        for (const Driver& d : rs.drivers) LetGo(d, playerPosition);
        rs.drivers.clear();
    }
    if (!here) { ReportTally(nullptr); return; }
    ManageDrivers(*here, playerPosition);
    // Cars still get tidied away when the player is under the world; they just do not get made.
    // Following the player down there is what a spawner that only knows about distance does, and
    // it is not what anybody wants to see - see Ground::BeneathEverything.
    if (playerHasGround) SpawnSome(*here, playerPosition, CarBudget());
    else Counts().noPlayerGround++;
    ReportTally(here);
}

int Count() {
    int n = 0;
    for (const RegionState& rs : Regions()) n += (int)rs.drivers.size();
    return n;
}

int Budget() { return CarBudget(); }

int LoadedModelCount(int* outOf) {
    const int count = (int)(sizeof(kPlaceholderCars) / sizeof(int));
    if (outOf) *outOf = count;
    int loaded = 0;
    for (int i = 0; i < count; i++) if (VehicleSpawn::ModelIsLoaded(kPlaceholderCars[i])) loaded++;
    return loaded;
}

// Only ever called when there is no player at all - a menu, a load, a new game. The vehicle pool
// and its re-use counters are about to be reset, so nothing here may touch a vehicle: the handles
// name memory the game is in the middle of throwing away. Dropping them is the whole job.
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

    if (!VehicleSpawn::Ready()) {
        Regions().clear();
        return;
    }
    // NOT "up to CarBudget() car(s)". The budget is San Andreas' own, out of the four floats
    // CPopCycle::UpdatePercentages fills from the popcycle row for the zone the player is standing
    // in, and at start-up the player is not standing anywhere yet: all four are zero and this line
    // used to say "up to 0 car(s)", which reads like a setting that has turned the traffic off and
    // is nothing of the kind. What is fixed at start-up is the multiplier and the radius, so those
    // are what it says; F8 reports the live budget.
    Log::Write("traffic: %u region(s) will be driven on their own carriageways within %.0f units, "
               "as many cars as San Andreas' own popcycle allows for the zone x%.2f (F8 says how "
               "many that is right now), at most %d",
               (unsigned)Regions().size(), Settings::world.regionTrafficRadius,
               Settings::world.regionTrafficDensity, kMaxCars);
}

}   // namespace RegionTraffic
