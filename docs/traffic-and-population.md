# Traffic and population in an imported city

How an imported city gets pedestrians, police and moving vehicles when San Andreas' own spawners
cannot reach it, and the engine facts that make it work. `docs/known-limitations.md` covers why the
native route is closed; this covers what runs instead.

Everything here was established by disassembling `gta_sa.exe`. Where a fact was found the expensive
way — by shipping something wrong first — that is said, because the wrong version usually looks
reasonable.

---

## The shape of it

San Andreas populates a street from its vehicle path graph, and that graph cannot describe ground
outside ±4095.875 (see known-limitations). So the plugin populates imported streets itself, from the
donor game's own data, entirely in ordinary memory where nothing is compressed and nothing has a
range:

| | source | placed by | driven by |
|---|---|---|---|
| pedestrians | donor `pednodes.dat` | `CPopulation::AddPed`, `makeWander` false | `CTaskComplexGoToPointAndStandStill` |
| police on foot | same graph | as above | `CTaskComplexKillPedOnFoot` |
| vehicles | donor `carnodes.dat` | `CCarCtrl` creation path | `MISSION_GOTOCOORDS_STRAIGHT` |

The importer writes both graphs. Earlier figures here had Vice City at 7967 car nodes to 3789 ped, which was the two graphs swapped (see the note on path types in tools/import_city.py); Liberty
City 3233 / 3343 and 5198, with median carriageway width and lane counts each way.

Settings are `RegionTraffic` (default 1), `RegionTrafficDensity` (1.0, a multiplier on the car budget
`popcycle.dat` gives for the player's zone) and `RegionTrafficRadius` (90.0). All are skipped under
SA-MP, where the server owns the world.

---

## Why `MISSION_GOTOCOORDS_STRAIGHT`, and nothing else

It is the **only** car mission that drives toward an arbitrary coordinate without consulting
`CPathFind`, confirmed clean three call levels deep. Every other mission that goes somewhere either
routes through the path graph or is a manoeuvre relative to another entity.

That single constraint is why the imported cities can have traffic at all without touching San
Andreas' path arrays — and it is also why they cannot have traffic lights or lane geometry through
corners, both of which live in the link data the mission never reads.

---

## Six things the engine does that are not obvious

Each of these was wrong in a shipped version first. They are recorded because every one of them
looks fine until you know what it costs.

### 1. A `RANDOM` car of ours stops San Andreas generating its own

This is the important one. `GenerateOneRandomCar` budgets against
`NumRandomCars + NumLawEnforcerCars + NumMissionCars` plus the two emergency counters, capped at
`MaxNumberOfCars` = 12, and `GenerateRandomCars` **bails outright once `NumRandomCars` reaches 45**.
Thirty of our cars marked `RANDOM` would therefore stop the game generating any civilian traffic of
its own, anywhere in San Andreas, for as long as they existed.

Our cars are `PERMANENT`: a counter in neither sum, read by nothing that budgets. Set through
`CVehicle::SetVehicleCreatedBy` rather than by writing `m_nCreatedBy` directly, because the
constructor has already counted the car once and the setter is what un-counts it.

**If San Andreas' own traffic ever thins out or disappears while imported traffic works, this is the
first thing to check.**

### 2. Driving style 6 is the only one that gets both behaviours

Two separate routines give a car its road manners, and they disagree about which styles they serve:

- `FindAngleToWeaveThroughTraffic` — lateral avoidance — runs only for styles **{2, 5, 6}**.
- `FindMaximumSpeedForThisCarInTraffic` — longitudinal braking — early-outs for **{2, 3, 5}**.

So 6 is the only value that gets weaving *and* braking. Style 0 gets neither, which is a car that
drives through everything without slowing down.

### 3. The mission does not stay set unless you make it

`UpdateCarAI` clears `m_nCarMission` to 0 within three metres of the destination, braking the car to
a stop. Worse, for a destination further away than `m_nStraightLineDistance + 5` it runs a real
`DoPathSearch` one frame in eight and can flip the car to path-following mission 8.
`m_nStraightLineDistance` defaults to **20**, and node spacing regularly exceeds that, so this fires
constantly rather than rarely.

The fix is to set it to 255, re-stamp the mission every tick, and clear the re-join bit at `+0x3DC`
alongside it.

### 4. Only `STATUS_PHYSICS` is actually driven

`STATUS_PHYSICS` reaches `SteerAICarWithPhysics`. `STATUS_SIMPLE` runs `UpdateCarOnRails`, which is
entirely path-driven and therefore does nothing useful out here. It must be set explicitly.

### 5. The speed ceiling has to be called by hand

`FindMaximumSpeedForThisCarInTraffic` is `cdecl`, takes only the vehicle, and is free of
`CPathFind` — but it is simply *unreachable* from this mission. Calling it directly and using its
answer as a ceiling on cruise speed is what stops cars rear-ending each other, and it leaves the
plugin's own junction rule with nothing to do but junction priority.

### 6. There is no `HasModelLoaded` to call

In 1.0 US it is inlined at over a hundred sites. Getting the check wrong is not a graceful failure:
the constructor reaches `CVehicleModelInfo::CreateInstance`, which dereferences `m_pVehicleStruct`
unchecked and faults at **0x4C9691**. Read the flags straight out of `ms_aInfoForModel`.

### And one correction worth keeping

`ScanForPedDanger` makes **peds dodge cars**, not cars dodge peds. Cars avoiding people comes from
the weave in point 2, which is why style 6 matters twice.

---

## Police vehicles

Not built, but reachable on this route, which is worth knowing before anyone reaches for the limit
adjuster on their account.

Missions **3, 5, 6 and 18** — `RAMPLAYER_CLOSE`, `BLOCKPLAYER_CLOSE`, the handbrake stop, and
`BLOCKCAR_CLOSE` — are all path-free and steer at the player directly. The catch is that
`bIsLawEnforcer` must be left **clear**: `UpdateCarAI`'s police branch overwrites the mission with a
path-based one, so a car flagged as law enforcement will stop chasing the moment it is told to.

The wanted system itself needs nothing at all. Stars, decay, `CWanted`'s pursuit bookkeeping and
`CTaskComplexKillPedOnFoot` have no map or path dependency, which is why police on foot already work.

---

## What this route cannot give

- **Traffic lights.** They live in the link data the straight-line mission never reads.
- **Lane geometry through corners.** Cars hold a lane on a carriageway; they do not swing through a
  junction on the arc the link data would have described.
- **Path-based pursuit AI**, roadblocks, the police helicopter, and the SWAT/FBI/army responses at
  four stars and above — every one places a vehicle on a car path node.
- **The police radio's location dialogue.** `CAEPoliceScannerAudioEntity::AddAudioEvent` matches the
  zone against a hard-coded 194-entry table of San Andreas' own zones and says nothing when it does
  not find one. That is a zone dependency rather than a path one, and fixable.

The alternative that lifts all of these is fastman92's Limit Adjuster and its extended path format,
along with what that costs — a separate non-redistributable install, a global path grid that must be
widened to 32x32, and regenerating San Andreas' own 64 path files with a remapper that ships compiled
out. That comparison lives in `docs/known-limitations.md` so the trade-off is stated in one place.
