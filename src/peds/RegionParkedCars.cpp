// The cars an imported city had parked at its kerbs, put back where its own script left them.
//
// An empty street is most of why an imported city still reads as a model rather than a place. Both
// donor games knew that and both filled their kerbs from their mission script rather than from
// their map: 148 cars in Vice City and 147 in Liberty City, each with a position, a heading, a
// model and a chance of being locked. tools/import_city.py mines those out of the source main.scm,
// pairs every vehicle with a San Andreas one and writes the region's parkedcars.dat; everything
// difficult happens there and this file reads a plain table and calls the game.
//
// WHY NOT CTheCarGenerators, WHICH IS EXACTLY THIS FEATURE ALREADY. Three reasons and any one of
// them would do.
//
//   It cannot hold a region. A car generator's position is three int16 multiplied by the float at
//   0x858C48, which is 0.125, so it reaches +/-4096. Vice City sits at x 3846..7282 and Liberty
//   City at x -6907..-3591 as this project places them; neither fits. Widening that scale is
//   possible - it is what CompressedPositionScale does for pickups - but it changes how San
//   Andreas reads generators saved before the change, which is coordinate drift by another name.
//
//   It is saved by array index. CTheCarGenerators::Save writes 500 fixed entries and Load reads
//   them back positionally, so which slot a region's car took depends on what else was loaded.
//   Add a city, remove a city, reorder two, and a save resolves someone else's parking space.
//   docs/save-compatibility.md calls that index drift and says the rule: never widen or re-shape
//   a block San Andreas saves, put ours above the window or outside it entirely.
//
//   Its budget is five. CCarGenerator::DoInternalProcessing reads NumParkedCars at 0x6F351B and
//   returns immediately at 5 by day or 10 by night - a GLOBAL count, so a region's cars would
//   stop San Andreas' own generators anywhere in the state. See VehicleSpawn.cpp; this is why
//   these are PERMANENT and not PARKED, and it is the regression this work was most at risk of.
//
// So these are plugin-owned spawns. San Andreas' own 500 generators, its own save block and its
// own parked-car budget are untouched, byte for byte.
//
// WHAT MAKES A CAR PARKED RATHER THAN STOPPED. San Andreas' own answer, read out of
// CCarGenerator::DoInternalProcessing and copied here rather than invented:
//
//   status STATUS_ABANDONED (4). At 0x6F3B5B it does `and al, 7 / or al, 0x20` on the type/status
//   bitfield at +0x36, and 0x20 >> 3 is 4. That is the whole trick: CAutomobile::ProcessAI only
//   dispatches to AI::UpdateCarAI for STATUS_PHYSICS, so an abandoned car runs no AI at all and
//   cannot drift, while still running physics - it settles on its wheels, it can be shunted,
//   damaged, entered, stolen and blown up exactly like any other car standing in the street.
//   CCarCtrl::CreateCarForScript already leaves a car in that state (0x43205C, the same two
//   instructions), so this is confirming it rather than changing it.
//
//   mission MISSION_NONE and cruise speed 0, and the three packed path link ids emptied. Nothing
//   on STATUS_ABANDONED reads any of them; they are cleared because CreateCarForScript calls the
//   path-based CCarCtrl::JoinCarWithRoadSystem on the way out and would otherwise leave a stale
//   San Andreas node address on a car standing in Vice City.
//
//   no driver. Nobody is put in it, which is what tells every scanner in the game it is empty.
//
// HEIGHT. A z of -100 in the data file means "on the ground here". Both donor games use that
// convention and - this is the part worth knowing - so does San Andreas: CreateCarForScript loads
// z at 0x431FDB, compares it against the float at 0x859014, which is -100.0 exactly, and resolves
// it through CWorld::FindGroundZForCoord itself. So the convention travels untranslated and the
// region origin is deliberately NOT added to such a z, because -100 is a sentinel and not a place.
//
// HEADING. The file is in degrees; both donor games and San Andreas measure a heading the same
// way, local +Y forward turning anticlockwise about Z. San Andreas' own generator multiplies its
// stored byte angle by the float at 0x859BBC, which is 2*pi/256, with no sign change, which is
// what settles that. Degrees times pi/180 is the whole conversion.
//
// Nothing here runs under SA-MP: the server owns the vehicles there.
#include <plugin.h>
#include <CCarCtrl.h>
#include <CPools.h>
#include <CVehicle.h>
#include <CStreaming.h>
#include <CCamera.h>
#include <CTimer.h>
#include <CModelInfo.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "RegionParkedCars.h"
#include "VehicleSpawn.h"
#include "Settings.h"
#include "Log.h"

using namespace plugin;

namespace {

// The sentinel both donor games and San Andreas read as "work the ground height out yourself".
constexpr float kGroundZ = -100.0f;

// eEntityStatus STATUS_ABANDONED and eCarMission MISSION_NONE, spelled out because this file is
// about what a stationary car is.
constexpr unsigned char kStatusAbandoned = 4;
constexpr char kMissionNone = 0;
// eDoorLock DOORLOCK_LOCKED. The donor's lock column is a percentage chance, which is how both
// games wrote it; it is rolled once when the car is created, not once per bay, so the same kerb
// is not always locked.
constexpr unsigned kDoorLockLocked = 2;

// How far from the player a bay is worth filling, and how far past that its car is taken away
// again. The margin is what stops a car being made and unmade on the spot when the player stands
// on the boundary.
constexpr float kCullMargin = 1.35f;
// A car appearing in shot is the one thing that reads as a spawn rather than as parking, so a bay
// the camera can see is left empty - unless it is far enough away that a car appearing there is
// not something the eye catches, which is what keeps a street from staying visibly empty as you
// walk down it.
constexpr float kQuietDistance = 70.0f;
constexpr unsigned kSpawnIntervalMs = 250;

// The ceiling on how many of these exist at once, before RegionParkedCarsDensity scales it. It is
// a safety net rather than the thing that decides the density: the donor's own bays are what does
// that, and there are rarely more than a handful within the radius. 24 is the hard limit whatever
// the density says.
constexpr int kParkedCarBudget = 14;
constexpr int kParkedCarCeiling = 24;

// Slots kept free in the vehicle pool. This is deliberately larger than the 6 the moving traffic
// keeps, and that difference is the whole interaction between the two: they share one pool, and
// under pressure the parked cars stop being created first. A missing parked car is invisible; a
// missing car in the road is not. San Andreas' own defence, CCarCtrl::RemoveCarsIfThePoolGetsFull,
// starts removing at 8 free, so the order under pressure is: we stop parking, then we stop
// driving, then the game starts taking cars away.
constexpr int kVehiclePoolReserve = 14;

// A car this far from the bay it was made in is no longer parked - shunted out by the player,
// most likely - and its bay counts as emptied.
constexpr float kDisturbedDistance = 6.0f;
// Below this much health a car is a wreck, and a wreck's bay is not refilled the moment the player
// looks away. CVehicle starts at 1000.
constexpr float kWreckedHealth = 300.0f;
constexpr unsigned char kStatusWrecked = 5;

// How near the player has to be for a car of ours vanishing to count as the player having done it.
//
// This should not be able to happen at all: CCarCtrl::CreateCarForScript sets bIsLocked at
// 0x43205C - the flag whose own comment is "locked by the script (cannot be removed)" - so San
// Andreas' distance and pool sweeps leave ours alone, and a car of ours going away while it is
// still in range means it was destroyed. But "should not" rests on one bit read out of a header,
// and if it were wrong the failure would be a city that quietly empties over a session as space
// after space is remembered as taken. Three lines is a cheap price for not having that bug.
constexpr float kBlamePlayerWithin = 80.0f;

// One place the source game parked a car.
struct Bay {
    float x, y, z;          // the region's own coordinates; z of -100 means "on the ground"
    float heading;          // radians
    std::string modelName;  // the San Andreas model paired with it, or "*" for the fleet
    int model;              // resolved from modelName on first use; -1 until then, -2 if unknown
    int lock;               // percent chance the source game left it locked

    int ref;                // CPools vehicle handle while a car of ours stands here, else -1
    int standing;           // the model the car in `ref` was actually made with - which is not
                            // always `model`, because a bay whose car has not streamed gets one
                            // out of the fleet. Half of the proof that the handle still names our
                            // car and not somebody else's; see VehicleSpawn::Resolve.
    bool taken;             // the player emptied this bay; see rearmAt
    unsigned rearmAt;       // CTimer ms at which it may be filled again
};

struct RegionState {
    const MapRegion* region = nullptr;
    std::vector<Bay> bays;
    // What the source game meant by "any ordinary car": its own traffic models, already paired.
    std::vector<std::string> fleetNames;
    std::vector<int> fleet;          // resolved ids, filled on first use
    bool fleetResolved = false;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
};

std::vector<RegionState>& Regions() { static std::vector<RegionState> v; return v; }
unsigned gNextSpawn = 0;

// ------------------------------------------------------------------ the file

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

// "donor/samodel" - the source game's vehicle and the San Andreas one standing in for it. Only the
// right-hand side is used; the left is there so that importing the donor's own cars later is a
// change to this file and to nothing else, exactly as pedgrp.dat pairs a person.
std::string PairedModel(const std::string& token) {
    size_t slash = token.find('/');
    return slash == std::string::npos ? token : token.substr(slash + 1);
}

bool ReadBayFile(RegionState& rs) {
    FILE* f = fopen(rs.region->parkedCarFile.c_str(), "rb");
    if (!f) {
        Log::Write("[%s] parked cars: %s cannot be opened", rs.region->name.c_str(),
                   rs.region->parkedCarFile.c_str());
        return false;
    }
    std::string text;
    char buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, got);
    fclose(f);

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
        if (_stricmp(t[0].c_str(), "fleet") == 0) {
            for (size_t i = 1; i < t.size(); i++) rs.fleetNames.push_back(PairedModel(t[i]));
            continue;
        }
        // car x y z heading model colour1 colour2 alarm lock
        if (_stricmp(t[0].c_str(), "car") != 0 || t.size() < 10) { malformed++; continue; }
        Bay b;
        b.x = (float)atof(t[1].c_str());
        b.y = (float)atof(t[2].c_str());
        b.z = (float)atof(t[3].c_str());
        b.heading = (float)atof(t[4].c_str()) * 0.01745329252f;
        b.modelName = PairedModel(t[5]);
        // t[6] and t[7] are the source game's own carcols.dat indices and t[8] its alarm chance.
        // Both are recorded in the file and deliberately not applied - a donor palette index is
        // not a San Andreas one, and San Andreas keeps an alarm state rather than a flag whose
        // "fitted but quiet" value is not established here. The file is where they go if they
        // ever are.
        b.lock = atoi(t[9].c_str());
        b.model = -1;
        b.ref = -1;
        b.standing = -1;
        b.taken = false;
        b.rearmAt = 0;
        rs.bays.push_back(b);
    }
    if (malformed)
        Log::Write("[%s] parked cars: %d malformed line(s)", rs.region->name.c_str(), malformed);
    return !rs.bays.empty();
}

void MeasureRegion(RegionState& rs) {
    for (size_t i = 0; i < rs.bays.size(); i++) {
        float wx = rs.bays[i].x + rs.region->originX;
        float wy = rs.bays[i].y + rs.region->originY;
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

// ------------------------------------------------------------------ which model stands here

// Resolved on first use rather than at start-up, the same way a ped group is: a region's own
// models are not registered until its IDEs have been read, and this file is loaded before that.
int ResolveName(const std::string& name) {
    int id = -1;
    CBaseModelInfo* info = CModelInfo::GetModelInfo((char*)name.c_str(), &id);
    return (info && id >= 0) ? id : -2;
}

void ResolveFleet(RegionState& rs) {
    if (rs.fleetResolved) return;
    rs.fleetResolved = true;
    int missing = 0;
    for (const std::string& name : rs.fleetNames) {
        int id = ResolveName(name);
        if (id < 0) { missing++; continue; }
        rs.fleet.push_back(id);
    }
    if (missing)
        Log::Write("[%s] parked cars: the fleet names %d model(s) San Andreas does not have",
                   rs.region->name.c_str(), missing);
}

// A model in memory for this bay, or -1 to leave it empty for now.
//
// A bay that named a car gets that car when it is in memory. When it is not, it gets whatever of
// the fleet is - which is precisely what San Andreas does with its own generators: at 0x6F3540
// CCarGenerator::DoInternalProcessing reads ms_aInfoForModel for the model it wanted and, if it
// is not loaded, goes and picks one that is. Nothing is requested from the streamer, so this
// never competes with the traffic or the map for a slot; the cost is that a bay whose car is not
// in memory gets an ordinary one instead of the Cheetah the script asked for.
int ChooseModel(RegionState& rs, Bay& b) {
    if (b.model == -1 && b.modelName != "*") b.model = ResolveName(b.modelName);
    if (b.model >= 0 && VehicleSpawn::ModelIsLoaded(b.model)) return b.model;
    ResolveFleet(rs);
    if (rs.fleet.empty()) return -1;
    int start = rand() % (int)rs.fleet.size();
    for (size_t i = 0; i < rs.fleet.size(); i++) {
        int model = rs.fleet[(start + i) % rs.fleet.size()];
        if (VehicleSpawn::ModelIsLoaded(model)) return model;
    }
    return -1;
}

// ------------------------------------------------------------------ making one stand there

CVector BayWorld(const RegionState& rs, const Bay& b) {
    // A z of -100 is a sentinel, not a height, so the region's own vertical offset is not added
    // to it - CreateCarForScript resolves it against the ground that is actually there.
    float z = b.z <= kGroundZ ? kGroundZ : b.z + rs.region->originZ;
    return CVector(b.x + rs.region->originX, b.y + rs.region->originY, z);
}

// Where a car is actually MADE, which is not the same question as where the bay is.
//
// Every bay goes in on the sentinel, not on the height its line claims, because
// CCarCtrl::CreateCarForScript compares z against the float at 0x859014 (-100.0, tested at
// 0x432117) and where they match goes and asks CWorld::FindGroundZForCoord - which ignores z and
// casts from 1000 straight down, so it cannot be wrong about a datum it never reads. 317 of the
// 320 mined bays across both cities carry a real height rather than the sentinel, and a real
// height is only as good as the agreement between the donor's script and the collision that
// actually got imported. The ground that is there wins.
CVector BaySpawnPoint(const RegionState& rs, const Bay& b) {
    return CVector(b.x + rs.region->originX, b.y + rs.region->originY, kGroundZ);
}

bool CreateParkedCar(RegionState& rs, Bay& b, int model) {
    CVector at = BaySpawnPoint(rs, b);
    CVehicle* car = CCarCtrl::CreateCarForScript(model, at, false);
    if (!car) return false;
    // Through the game's own setter, which moves the car between the counters San Andreas budgets
    // against; the constructor has already counted it once as a mission car. PERMANENT is the one
    // counter nothing reads - see VehicleSpawn.cpp, and read it before changing this.
    VehicleSpawn::MakePermanent(car);
    car->m_nStatus = (eEntityStatus)kStatusAbandoned;
    car->SetHeading(b.heading);
    CAutoPilot& pilot = car->m_autoPilot;
    pilot.m_nCarMission = (eCarMission)kMissionNone;
    pilot.m_nCruiseSpeed = 0;
    pilot.m_nPathFindNodesCount = 0;
    pilot.m_currentAddress.Clear();
    pilot.m_startingRouteNode.Clear();
    pilot.field_8.Clear();
    // The three packed link ids CreateCarForScript's call to JoinCarWithRoadSystem will have
    // filled in with whichever San Andreas node the clamped area search happened to find. They
    // are a bitfield pair in plugin-sdk with no setter, so they are written as the words they are,
    // which is what the game's own code does to them too. 0xFFFF is its empty value.
    *(uint16_t*)((char*)car + 0x3A4) = 0xFFFF;
    *(uint16_t*)((char*)car + 0x3A6) = 0xFFFF;
    *(uint16_t*)((char*)car + 0x3A8) = 0xFFFF;
    if (b.lock > 0 && (rand() % 100) < b.lock) car->m_eDoorLock = (eDoorLock)kDoorLockLocked;
    b.ref = CPools::GetVehicleRef(car);
    b.standing = model;
    return true;
}

// ------------------------------------------------------------------ the bay's memory
//
// When the player takes a car or destroys it, the space it left should be a space. Refilling it
// the moment they look away is what San Andreas' own generators do - their minimum delay is zero
// - and it is the single most obviously wrong thing about them, so it is the one behaviour of the
// donor's system this deliberately does not reproduce.
//
// How long it lasts is RegionParkedCarsRespawn, and it is a real interval rather than "for ever"
// for one reason: a session long enough to empty every bay would leave a city permanently emptier
// than the game it came from, which is a worse failure than a car reappearing. The default, 720
// seconds, is half a San Andreas day at its normal clock rate - long enough that driving away and
// coming back finds the space still empty, short enough that a city does not drain.
//
// It is kept in memory and never written to a save. That is deliberate and it is the rule in
// docs/save-compatibility.md: our state does not go in San Andreas' blocks, and until there is a
// sidecar to put it in, a session's memory is what there is. A new game or a load starts every
// bay full, which is what a new game should look like.
// Where a tick's attempts to fill a bay went. Same reason as the traffic's: "0 parked at its
// kerbs" is a count, and a count cannot say whether every bay was out of range, or remembered as
// emptied, or in shot, or had no model in memory, or CreateCarForScript refused.
struct Tally {
    int ticks, notReady, waiting, atBudget, poolFull;
    int outOfRange, notArmed, taken, inShot, noModel, madeNothing, placed;
    int lost, released;
    unsigned nextReport;
};
Tally& Counts() { static Tally t{}; return t; }

void RememberEmptied(Bay& b) {
    b.taken = true;
    unsigned seconds = (unsigned)(Settings::world.regionParkedCarsRespawn > 0.0f
                                  ? Settings::world.regionParkedCarsRespawn : 0.0f);
    b.rearmAt = CTimer::m_snTimeInMilliseconds + seconds * 1000u;
}

bool BayIsArmed(const Bay& b) {
    if (!b.taken) return true;
    if (Settings::world.regionParkedCarsRespawn <= 0.0f) return false;   // 0 means never
    return (int)(CTimer::m_snTimeInMilliseconds - b.rearmAt) >= 0;
}

// ------------------------------------------------------------------ the frame

bool PlayerOver(const RegionState& rs, const CVector& at, float margin) {
    return at.x >= rs.minX - margin && at.x <= rs.maxX + margin &&
           at.y >= rs.minY - margin && at.y <= rs.maxY + margin;
}

int BayBudget() {
    float wanted = (float)kParkedCarBudget * Settings::world.regionParkedCarsDensity;
    int budget = (int)(wanted + 0.5f);
    if (budget < 0) budget = 0;
    if (budget > kParkedCarCeiling) budget = kParkedCarCeiling;
    return budget;
}

int LiveCount(const RegionState& rs) {
    int n = 0;
    for (const Bay& b : rs.bays) if (b.ref >= 0) n++;
    return n;
}

// Once the player gets into one of these it stops being ours.
//
// It is handed back to San Andreas as an ordinary random car, which is exactly what the game's own
// script does with a car it no longer wants to own - opcode 01C3 MARK_CAR_AS_NO_LONGER_NEEDED
// goes through the same setter to the same counter. From that moment every rule that applies to a
// car found in the street applies to this one: the game's own distance cleanup, the pool sweep,
// mission cleanup, a garage. The alternative was to keep owning it and delete it when it got far
// enough away, which would have taken a car the player had parked somewhere on purpose.
//
// What it costs is one entry on NumRandomCars while that car lives, against a generator that
// slows at 12 and stops at 45. San Andreas' own parked cars have the same property and a budget
// of five, so this is the game's own trade rather than a new one - but it is the thing to look at
// if San Andreas' traffic ever thins out after a long session of stealing parked cars.
void Release(Bay& b, CVehicle* car) {
    Counts().released++;
    VehicleSpawn::HandBackToTheGame(car);
    b.ref = -1;
    b.standing = -1;
    RememberEmptied(b);
}

void ManageBays(RegionState& rs, const CVector& player, bool over) {
    const float cull = Settings::world.regionParkedCarsRadius * kCullMargin;
    for (Bay& b : rs.bays) {
        if (b.ref < 0) continue;
        // Resolve rather than CPools::GetVehicle: a pool slot re-used 128 times answers to an old
        // handle again, and everything below this line writes to the car it hands back. See
        // VehicleSpawn::Resolve.
        CVehicle* car = VehicleSpawn::Resolve(b.ref, b.standing);
        if (!car) {
            Counts().lost++;
            // The car is gone and we did not take it - blown up and burnt out, most likely. The
            // space is empty because something happened to it, so it stays empty for a while, but
            // only if the player was near enough to be the thing that happened.
            b.ref = -1;
            b.standing = -1;
            CVector where = BayWorld(rs, b);
            float ax = where.x - player.x, ay = where.y - player.y;
            if (ax * ax + ay * ay < kBlamePlayerWithin * kBlamePlayerWithin) RememberEmptied(b);
            continue;
        }
        if (VehicleSpawn::PlayerIsIn(car)) { Release(b, car); continue; }

        CVector at = BayWorld(rs, b);
        CVector p = car->GetPosition();
        float bx = p.x - at.x, by = p.y - at.y;
        if (bx * bx + by * by > kDisturbedDistance * kDisturbedDistance) {
            // Pushed out of its space, or driven out and abandoned. Either way it is not parked
            // here any more, so it goes back to the game and the space counts as emptied.
            Release(b, car);
            continue;
        }
        if (car->m_nStatus == (eEntityStatus)kStatusWrecked || car->m_fHealth < kWreckedHealth) {
            // A wreck is left standing - a burnt-out car at the kerb is right - but its space is
            // remembered as emptied now, so that taking the wreck away when the player leaves
            // does not put a fresh car where they just destroyed one.
            RememberEmptied(b);
        }

        float dx = p.x - player.x, dy = p.y - player.y;
        float d2 = dx * dx + dy * dy;
        if (over && d2 <= cull * cull) continue;
        // Far enough to go, as long as it is not being watched - except when it is a very long
        // way off, where the camera test is meaningless and holding on to it would leave a car
        // standing in a city the player has left.
        if (d2 > (cull * 2.0f) * (cull * 2.0f) || !TheCamera.IsSphereVisible(p, 4.0f)) {
            VehicleSpawn::Remove(car);
            b.ref = -1;
            b.standing = -1;
        }
    }
}

void ReportTally() {
    Tally& t = Counts();
    if (!Settings::world.debug) return;
    if (CTimer::m_snTimeInMilliseconds < t.nextReport) return;
    t.nextReport = CTimer::m_snTimeInMilliseconds + 1000;
    if (!t.ticks) return;
    Log::Write("parked/sec: %d tick(s) -> %d not ready, %d waiting for the spawn interval, %d at "
               "budget, %d pool full; bays %d out of range, %d remembered as emptied, %d in shot, "
               "%d no model in memory, %d CreateCarForScript said no, %d placed; %d vanished, "
               "%d released to the player",
               t.ticks, t.notReady, t.waiting, t.atBudget, t.poolFull, t.outOfRange, t.notArmed,
               t.inShot, t.noModel, t.madeNothing, t.placed, t.lost, t.released);
    Tally fresh{};
    fresh.nextReport = t.nextReport;
    t = fresh;
}

void FillBays(RegionState& rs, const CVector& player) {
    Tally& t = Counts();
    t.ticks++;
    if (!VehicleSpawn::Ready()) { t.notReady++; return; }
    if (CTimer::m_snTimeInMilliseconds < gNextSpawn) { t.waiting++; return; }
    if (LiveCount(rs) >= BayBudget()) { t.atBudget++; return; }
    if (!VehicleSpawn::PoolHasRoom(kVehiclePoolReserve)) { t.poolFull++; return; }
    gNextSpawn = CTimer::m_snTimeInMilliseconds + kSpawnIntervalMs;

    const float radius = Settings::world.regionParkedCarsRadius;
    for (Bay& b : rs.bays) {
        if (b.ref >= 0) continue;
        if (!BayIsArmed(b)) { t.notArmed++; continue; }
        CVector at = BayWorld(rs, b);
        float dx = at.x - player.x, dy = at.y - player.y;
        float d2 = dx * dx + dy * dy;
        if (d2 > radius * radius) { t.outOfRange++; continue; }
        // Not in shot, unless it is far enough off that a car appearing there is not something
        // the eye catches. A space the player is standing and staring at simply stays empty.
        // A bay whose height is the -100 sentinel has no height to test yet, so the player's own
        // is used: the ground under a kerb is within a metre or two of the ground under them.
        CVector look(at.x, at.y, at.z <= kGroundZ ? player.z : at.z);
        if (d2 < kQuietDistance * kQuietDistance && TheCamera.IsSphereVisible(look, 4.0f)) {
            t.inShot++;
            continue;
        }
        int model = ChooseModel(rs, b);
        if (model < 0) { t.noModel++; return; }   // nothing of ours in memory yet; try next tick
        if (CreateParkedCar(rs, b, model)) { t.placed++; return; }   // one a tick, like the rest
        t.madeNothing++;
        return;
    }
}

}   // namespace

namespace RegionParkedCars {

void Tick(const CVector& playerPosition) {
    if (!Settings::world.regionParkedCars) return;
    const float margin = Settings::world.regionParkedCarsRadius * kCullMargin;
    for (RegionState& rs : Regions()) {
        bool over = PlayerOver(rs, playerPosition, margin);
        // Every region every frame, not only the one under the player. A car of ours has to be
        // tidied away when the player drives out of the city as much as when they walk out of the
        // street, and ManageBays is what does that - which is why this is not inside the "one
        // region under the player" loop the pedestrians and the traffic ride in.
        ManageBays(rs, playerPosition, over);
        if (over) FillBays(rs, playerPosition);
    }
    ReportTally();
}

int Count() {
    int n = 0;
    for (const RegionState& rs : Regions()) n += LiveCount(rs);
    return n;
}

int Budget() { return BayBudget(); }

int ArmedNear(const CVector& playerPosition) {
    const float radius = Settings::world.regionParkedCarsRadius;
    int n = 0;
    for (const RegionState& rs : Regions())
        for (const Bay& b : rs.bays) {
            if (b.ref >= 0 || !BayIsArmed(b)) continue;
            CVector at = BayWorld(rs, b);
            float dx = at.x - playerPosition.x, dy = at.y - playerPosition.y;
            if (dx * dx + dy * dy <= radius * radius) n++;
        }
    return n;
}

void Forget() {
    for (RegionState& rs : Regions()) {
        for (Bay& b : rs.bays) {
            b.ref = -1;
            b.standing = -1;
            b.taken = false;
            b.rearmAt = 0;
        }
    }
}

void Install() {
    bool any = false;
    for (const MapRegion& r : Settings::regions) any |= !r.parkedCarFile.empty();
    if (!any || !Settings::world.regionParkedCars) return;

    if (Settings::RunningUnderSaMp()) {
        Log::Write("SA-MP: the region's parked cars are off, the server owns the vehicles");
        return;
    }

    for (const MapRegion& r : Settings::regions) {
        if (r.parkedCarFile.empty()) continue;
        RegionState rs;
        rs.region = &r;
        if (!ReadBayFile(rs)) continue;
        MeasureRegion(rs);
        Log::Write("[%s] parked cars: %u space(s) over x %.0f..%.0f y %.0f..%.0f, %u model(s) of "
                   "ordinary traffic to fall back on", r.name.c_str(), (unsigned)rs.bays.size(),
                   rs.minX, rs.maxX, rs.minY, rs.maxY, (unsigned)rs.fleetNames.size());
        Regions().push_back(std::move(rs));
    }
    if (Regions().empty()) return;
    if (!VehicleSpawn::Ready()) {
        Regions().clear();
        return;
    }
    Log::Write("parked cars: %u region(s) will have their own cars at the kerb, up to %d at a "
               "time within %.0f units; a space the player empties stays empty for %.0fs",
               (unsigned)Regions().size(), BayBudget(), Settings::world.regionParkedCarsRadius,
               Settings::world.regionParkedCarsRespawn);
    // These are ticked from the pedestrian module's own once-a-frame hook on CPopulation::Update,
    // which is where the traffic rides too, so if that hook is never installed nothing here runs.
    // Say so at start-up rather than leaving an empty city and no explanation.
    bool anyGraph = false;
    for (const MapRegion& r : Settings::regions) anyGraph |= !r.pedNodeFile.empty();
    if (!Settings::world.regionPeds || !anyGraph)
        Log::Write("parked cars: ... but they are driven from the region pedestrian tick, and that "
                   "is not running (%s), so no car will actually be placed",
                   Settings::world.regionPeds ? "no region has a PedNodes graph" : "RegionPeds=0");
}

}   // namespace RegionParkedCars
