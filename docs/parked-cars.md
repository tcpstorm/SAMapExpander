# Parked cars in an imported city

Where the cars at an imported city's kerbs come from, why they are not San Andreas car generators,
and the engine facts that decide what a parked car actually is. `docs/traffic-and-population.md`
covers the moving traffic on the same roads; everything there about how a vehicle is made and
counted applies here too, and the two share `src/peds/VehicleSpawn.cpp`.

Everything here was established by disassembling `gta_sa.exe` 1.0 US and by reading the two donor
games' compiled scripts. Nothing in it has been run in a game — see the checklist at the end.

---

## Where the data comes from

Neither donor game puts a parked car in a map file. Both create them from `data/main.scm`, with
literal coordinates, through one opcode:

```
0x014B CREATE_CAR_GENERATOR  x y z angle model colour1 colour2 force alarm lock minDelay maxDelay
                             -> handle
0x014C SET_CAR_GENERATOR     handle count            (0 = off, >100 = for ever)
```

The parameter order was read off the script stream rather than taken from an opcode listing,
because the listings reorder parameters for display. It is the same in both games. What is *not*
the same is the float encoding, which the pickup work on this project already found the hard way:
Vice City writes a 32-bit IEEE float behind the `0x06` type byte, GTA III writes an int16 divided
by sixteen.

`tools/import_city.py` scans for the two opcode bytes rather than walking the instruction set —
that would mean implementing two games' whole scripts — so every candidate has to earn its place.
Four filters, cheapest first:

| filter | what it demands | Vice City | GTA III |
|---|---|---|---|
| shape | twelve immediates, four floats then eight integers, then a variable to store the handle | −39 | −13 |
| chain | a neighbour: either the `SET_CAR_GENERATOR` addressed to its own handle, or another well-shaped generator starting exactly where it ends | −0 | −0 |
| sense | the model is −1 or a vehicle the donor's own `default.ide` declares; heading is a bearing; alarm and lock are percentages; the delays are ordered | −0 | −0 |
| ground | the position is inside one of the donor's own zone boxes — the same file `scm_zone_ped_groups` validates its zone labels against | −0 | −0 |
| — | switched **off** the moment it is created: a mission turns those on later, and a car belonging to a mission we are not running is not street furniture | −24 | −2 |
| **kept** | | **148** of 211 | **147** of 162 |

The chain filter is worth more than its zero says. Every one of Vice City's 172 well-shaped
candidates is followed immediately by a `SET_CAR_GENERATOR` on its own handle, and 146 of GTA III's
149 are followed immediately by another generator — they are one contiguous block ending in a
`RETURN`. That is a second, independent confirmation that the parameter layout is right: a wrong
layout would not leave the next opcode landing exactly on a byte boundary 149 times running.

**The independent check** is the one that was not designed in. The placements come out of a compiled
script; the region's carriageways come out of the map files; nothing connects the two except both
being the same city. Vice City's mined parked cars span x −1809..719, y −1736..1524 against
carriageways spanning x −1822..874, y −1773..1502; Liberty City's span x −1574..1577, y −1503..319
against x −1585..1533, y −1611..570. 105 of Vice City's 148 and 98 of Liberty City's 147 stand
within 40 units of an actual carriageway node; the rest are car parks, driveways, marina berths and
a rooftop helipad, which is what the remainder should be. That number is written into the head of
each `parkedcars.dat`.

---

## The model pairing

Exactly the story the pedestrians had, and for the same reason: San Andreas is the same series and
kept a great many of the names.

|  | paired by name alone | needing the hand table | asking for "any car" |
|---|---|---|---|
| Vice City | 43 models / 71 cars | 10 models / 16 cars | 61 cars |
| GTA III | 28 models / 91 cars | 13 models / 56 cars | 0 |

`landstal`, `peren`, `sentinel`, `infernus`, `cheetah`, `taxi`, `ambulan`, `rumpo`, `stallion`,
`patriot`, `bobcat`, `banshee`, `barracks`, `pizzaboy`, `pcj600`, `faggio`, `polmav` and the rest
are the same name in all three games. Twenty are not, and those are a hand table in
`tools/import_city.py` by class and era, each with what is being given up:

- **GTA III** — `idaho`→Glendale, `blista`→Blista Compact (San Andreas renamed it), `kuruma`→Previon,
  `stinger`→Comet, `police`→`copcarla`, `bellyup`/`mrwongs`→Boxville, `mafia`→Admiral,
  `yakuza`→Sultan, `columb`→Huntley, `panlant`→Yankee, `toyz`→Mr Whoopee.
- **Vice City** — `sentxs`→Sentinel, `gangbur`→Burrito, `pheonix`→`phoenix` (Vice City spells it
  wrong), `kaufman`→Cabbie, `vicechee`→Cheetah, `fbicar`→Washington, `angel`→Freeway, `rio`→Tropic,
  `police`→`copcarla`, `stinger`→Comet.

Every pair is written into the region's `parkedcars.dat` as `DONOR/SAMODEL`, exactly as `pedgrp.dat`
pairs a person and `weapons.dat` pairs a gun, so importing the donor's own vehicles later is a
change to that file and to nothing else.

**Model −1** turns out to mean the same thing in all three games: "any ordinary car". San Andreas'
own generators say it the same way — `CCarGenerator::DoInternalProcessing` at 0x6F352E tests the
stored model for being negative and goes and picks a loaded one. So a `random/*` line draws from the
`fleet` line at the top of the file, which is the donor's own ordinary traffic (its `car` models in
the `poorfamily`/`richfamily`/`executive`/`worker`/`normal`/`taxi` classes — everything it marks
`ignore` is a mission or emergency vehicle and is left out), each paired the same way. 40 models for
Vice City, 22 for GTA III.

**What was carried and what was dropped.**

| | |
|---|---|
| position | carried. A z of −100 means "on the ground here" in both donor games — **and in San Andreas**: `CCarCtrl::CreateCarForScript` loads z at 0x431FDB, compares it against the float at 0x859014, which is −100.0 exactly, and resolves it through `CWorld::FindGroundZForCoord` itself. The convention travels untranslated, and the region's vertical origin is deliberately not added to such a z, because −100 is a sentinel and not a place. |
| heading | carried. All three games measure a heading the same way — local +Y forward, turning anticlockwise about Z — confirmed by San Andreas' own generator multiplying its stored byte angle by the float at 0x859BBC, which is 2π/256, with no sign change. Degrees × π/180 is the whole conversion. |
| door lock | carried. The donor's column is a percentage chance and it is rolled per car, not per space, so the same kerb is not always locked. |
| alarm | **dropped**, recorded in the file. San Andreas keeps an alarm *state* rather than a flag and the value meaning "fitted but quiet" was not established here; a car that howls when it is brushed is worse than a car with no alarm. |
| colour | **dropped**, recorded in the file. A donor `carcols.dat` index is not a San Andreas one — different tables, different lengths. 317 of the 320 say −1 anyway, which all three games read as "any colour this model comes in", so this costs three cars. |
| force-spawn | **dropped**. It exists to override San Andreas' parked-car budget, and these are not in it. |
| min/max delay | **dropped**, deliberately, and see *Persistence* below. |

---

## Not `CTheCarGenerators`, and three reasons

**It cannot hold a region.** A car generator's position is three int16 multiplied by the float at
0x858C48, which is 0.125, so it reaches ±4096. Vice City sits at x 3846..7282 as this project places
it and Liberty City at x −6907..−3591; neither fits. Widening that scale is possible — it is what
`CompressedPositionScale` does for pickups — but it changes how San Andreas reads generators saved
before the change, which is coordinate drift by another name.

**It is saved by array index.** `CTheCarGenerators::Save` writes 500 fixed entries and `Load` reads
them back positionally, so which slot a region's car took would depend on which regions loaded
before it. Add a city, remove one, reorder two, and a save resolves someone else's parking space.
`docs/save-compatibility.md` calls that index drift and states the rule: never widen or re-shape a
block San Andreas saves.

**Its budget is five.** See below — and this is the one that would have bitten hardest.

So these are plugin-owned spawns. San Andreas' own 500 generators, its own save block and its own
parked-car budget are untouched, byte for byte.

---

## The counter, and the regression this was most at risk of

`docs/traffic-and-population.md` records that a car of ours marked `RANDOM` stops San Andreas
generating its own. There is a second counter with the same shape, one along, and it is the one a
parked car would naturally reach for.

`CCarCtrl::UpdateCarCount` (0x424000) is the only thing that moves a vehicle between the five
counters, and its jump tables at 0x4240CC/0x4240DC say exactly which:

| `eVehicleCreatedBy` | counter | read by |
|---|---|---|
| 1 `RANDOM` | 0x969094 `NumRandomCars` (+ 0x969098 for a law enforcer) | `GenerateOneRandomCar`, `GenerateRandomCars` (bails at 45) |
| 2 `MISSION` | 0x96909C `NumMissionCars` | the same budgets |
| 3 `PARKED` | 0x9690A0 `NumParkedCars` | **three budgets, below** |
| 4 `PERMANENT` | 0x9690A4 `NumPermanentCars` | **nothing at all** |

Every reader of `NumParkedCars` in the whole `.text` is one of three, and all three are budgets:
`CCarGenerator::DoInternalProcessing` (0x6F351B), `CCarCtrl::GenerateEmergencyServicesCar`
(0x42FA27) and `CPopulation::AddToPopulation` (0x614860). The first is the one that hurts. It reads
`NumParkedCars` and returns immediately if it is at or above **5 by day or 10 by night** — a global
count, checked before anything else, skipped only for a generator with the force bit set.

**So five cars of ours marked `PARKED` would have stopped every one of San Andreas' own 500 car
generators producing anything, anywhere in the state, for as long as ours existed.** Every kerb in
Los Santos would have emptied because a player walked down a street in Vice City. That is the same
shape as the traffic regression and worse in degree, and it is why these are `PERMANENT`, set
through `CVehicle::SetVehicleCreatedBy` (0x6D5D70) so the constructor's count is undone rather than
doubled.

`NumPermanentCars` is read by nothing: an xref over the whole `.text` finds only `CCarCtrl::Init`,
`CCarCtrl::ReInit` and `UpdateCarCount` itself.

---

## What makes a car parked rather than stopped

San Andreas' own answer, read out of `CCarGenerator::DoInternalProcessing` and copied rather than
invented:

- **`STATUS_ABANDONED` (4).** At 0x6F3B5B it does `and al, 7 / or al, 0x20` on the type/status
  bitfield at `CEntity+0x36`, and `0x20 >> 3` is 4. That is the whole trick.
  `CAutomobile::ProcessAI` only dispatches to `AI::UpdateCarAI` for `STATUS_PHYSICS`, so an
  abandoned car runs no AI at all and cannot drift — while still running physics, so it settles on
  its wheels and can be shunted, damaged, entered, stolen and blown up exactly like any other car
  standing in the street. `CCarCtrl::CreateCarForScript` already leaves a car in that state (the
  same two instructions at 0x43205C), so this is confirming it rather than changing it.
- **`MISSION_NONE`, cruise speed 0, and the three packed path link ids emptied.** Nothing on
  `STATUS_ABANDONED` reads any of them; they are cleared because `CreateCarForScript` calls the
  path-based `CCarCtrl::JoinCarWithRoadSystem` on its way out and would otherwise leave a stale San
  Andreas node address on a car standing in Vice City.
- **No driver.** Nobody is put in it, which is what tells every scanner in the game it is empty.

---

## Lifecycle, and what was reused

The traffic had already solved a version of every part of this, so the shape is the same and three
pieces are now literally shared, in `src/peds/VehicleSpawn.cpp`: which counter a car is on and how
to move it, the `ms_aInfoForModel` read that stands in for the `HasModelLoaded` that does not exist
in 1.0 US, the `CWorld::Remove` + scalar-deleting-destructor disposal that must never be a C++
`delete`, and the byte check on `CreateCarForScript` and `AddPedInCar` before either is called.
`RegionTraffic.cpp` now calls those instead of its own copies.

What is deliberately *not* the same:

- **It ticks for every region, every frame, including when the player is over none.** The
  pedestrians and the traffic run inside a loop that stops at the first region the player is
  standing over and does nothing at all when they are over none — which is exactly the moment a car
  standing at a Vice City kerb needs taking away, because the player has just driven out of Vice
  City. So `RegionParkedCars::Tick` is called from outside that loop.
- **A space, not a spawn point.** Every car belongs to a numbered space; a space holds at most one
  car and remembers what happened to it.

The rules, in order:

| what happened | what is done |
|---|---|
| the space is within `RegionParkedCarsRadius` and empty | fill it, one a tick, but never in shot unless it is more than 70 units off, where a car appearing is not something the eye catches |
| the car is more than radius × 1.35 away and off camera | take it away; the space stays armed and refills when the player comes back |
| the car is more than radius × 2.7 away | take it away whether or not the camera can see it — at that distance the camera test is meaningless and holding on would leave a car standing in a city the player has left |
| the player gets in | it stops being ours: hand it back as an ordinary random car and forget it |
| it is pushed or driven more than 6 units out of its space | the same |
| it is wrecked or below 300 health | leave the wreck standing, but remember the space as emptied now |
| `CPools::GetVehicle` no longer resolves it | the game took it; remember the space as emptied |
| no player at all (a menu, a load, a new game) | forget every space and every handle — the pool's re-use counters are about to be reset |

**Why hand a stolen one back rather than keep owning it.** The alternative was to keep it and delete
it once it got far enough away, which would take a car the player had parked somewhere on purpose.
Handing it back is what the game's own script does — opcode 01C3 `MARK_CAR_AS_NO_LONGER_NEEDED`
reaches `CTheScripts::CleanUpThisVehicle` — and from that moment every rule that applies to a car
found in the street applies to this one, including garages and the game's own cleanup. What it
costs is one entry on `NumRandomCars` while that car lives, against a generator that slows at 12 and
stops at 45; San Andreas' own parked cars have exactly the same property with a budget of five, so
this is the game's own trade and not a new one. It is still the thing to look at if San Andreas'
traffic thins after a long session of stealing parked cars in an imported city.

**And the half of that idiom it is easy to miss.** `CleanUpThisVehicle` (0x486670, behind the
HOODLUM jump at 0x1565CA0) does not only move the counter. It first does
`and byte ptr [esi+0x428], 0xF7`, clearing **`bIsLocked`** — the flag whose own name is "locked by
the script (cannot be removed)". `CCarCtrl::CreateCarForScript` *sets* that bit at 0x43205F, which
is what keeps San Andreas' distance and pool sweeps off any vehicle of ours while it is ours (and
therefore what makes a car of ours vanishing mean it was destroyed rather than culled). Handing one
back without clearing it would put the car on the `RANDOM` counter and still leave nothing able to
remove it — the leak the hand-back exists to avoid, wearing the costume of the fix.

---

## Persistence within a session

A space the player empties stays empty for `RegionParkedCarsRespawn` seconds, default 720.

**Why not instantly.** The donor games' generators have a minimum delay of zero and put another car
in the space the moment you look away. That is the single most obviously wrong thing about them and
it is the one behaviour of their system this deliberately does not reproduce — which is also why
the mined `minDelay`/`maxDelay` columns are dropped rather than carried.

**Why not for ever.** A session long enough to empty every space would leave the city permanently
emptier than the game it came from, which is a worse failure than a car reappearing. 720 seconds is
half a San Andreas day at its normal clock rate: long enough that driving off and coming back finds
the space still empty, short enough that a city does not drain. `0` means "for the rest of the
session" for anyone who wants it.

**Why not saved.** `docs/save-compatibility.md` says our state does not go in San Andreas' blocks,
and there is no sidecar to put it in yet. So it is a session's memory, held in the plugin, and a new
game or a load starts with every space full — which is what a new game should look like.

---

## Density and the vehicle pool

The donor's own spaces are what decides the density: 148 and 147 over a whole city, sparse enough
that there are rarely more than a handful within the radius. `RegionParkedCarsDensity` is therefore a
ceiling on how many exist at once rather than a thinning — 1.0 is 14, and 24 is the hard limit
whatever the setting says.

The interaction with the moving traffic is one number. They share the vehicle pool, and the parked
cars keep **14** free slots where the traffic keeps **6**, so under pressure the order is: parked
cars stop being created, then traffic stops being created, then San Andreas' own
`CCarCtrl::RemoveCarsIfThePoolGetsFull` starts removing cars at 8 free. A missing car at a kerb is
invisible; a missing car in the road is not. Nothing here ever removes a car that already exists to
make room — existing cars are the ones the player can see.

They do not share a *budget*: the traffic's comes from `popcycle.dat` and the parked cars' is the
flat ceiling above, because a parking space is not traffic and San Andreas' own population model has
nothing to say about how many cars are parked in Vice City.

**Streaming.** No model is ever requested. A space that named a car gets that car when it is in
memory and an ordinary one from the fleet when it is not — which is precisely what San Andreas does
with its own generators (0x6F3540 reads `ms_aInfoForModel` for the model it wanted and goes and
picks a loaded one if it is not there). The cost is that a space whose car has not been streamed
gets an ordinary car instead of the Cheetah the script asked for; the benefit is that this never
competes with the traffic, the peds or the map for a streaming slot.

---

## What this deliberately does not do

- **No boats, aircraft or bikes are excluded** — the donor placed a Maverick on a helipad and a
  Tropic in a marina and those come across — but nothing checks that the water or the roof is
  actually there. If a region is placed with a vertical offset, or its water map is off, a boat
  space is a boat on the sea bed.
- **No blockage check.** San Andreas' own generator calls `CCarGenerator::CheckForBlockage` before
  spawning. That is a `__thiscall` on a `CCarGenerator` we do not have, so instead a space is only
  filled when it is out of shot or far away, which covers the case that matters (a car appearing on
  top of the player) and not the case that does not (two cars overlapping in a space the player
  cannot see, where physics separates them before they look).
- **No alarms and no colours**, for the reasons in the table above. Both are in the data file.
- **No save.** Nothing is written to a savegame, by design.
- **No sidecar.** When one exists, the emptied-space memory and the region's own vehicle imports are
  the two things that belong in it.

---

## Two more the engine does that are not obvious

Both were found the expensive way, in a shipped build. `docs/traffic-and-population.md` numbers six
of these; these are the seventh and the eighth, and they belong with them.

### 7. There are two `ms_aInfoForModel` arrays once the model id limit is raised

`[World] ModelIdLimit` moves `CStreaming::ms_aInfoForModel` to the heap (`src/limits/ModelLimits.cpp`)
and rewrites the 363 instructions that name it. What it cannot rewrite is a *plugin* that reads the
vanilla address, and every "is this model in memory" test in this project did:
`cmp byte ptr [m*0x14 + 0x8E4CD0], 1` is how San Andreas asks, because 1.0 US has no
`CStreaming::HasModelLoaded` to call, and 0x8E4CD0 is a literal. After the move that array is
abandoned, still holding the zeros `CStreaming::Init` left in it, so **the answer is "nothing in
the game is loaded", for ever**.

That is not a crash and not a log line. Every subsystem installs, reports its node counts and its
budgets, and then never creates anything: the traffic's `ChooseLoadedCar`, the parked cars'
`ChooseModel` and the pedestrians' district model pick all give up on the first test. An imported
city with a correct log and nothing alive in it is this.

`ModelLimits::ModelIsLoaded` is now the only place that reads the flag, and it reads whichever
array is the live one. F8 reports how many of the placeholder vehicle models are in memory, which
is the number that tells this apart from "no room in the pool" and from "no bay in range".

The same trap catches plugin-sdk, which caches both arrays at static-initialisation time by reading
the disp32 out of an instruction — `ms_aInfoForModel` from 0x408B25 in `CStreaming::RemoveModel`,
`ms_modelInfoPtrs` from 0x40CD67 in `RequestModelStream`. Both operands are in the site table, so
the SDK gets the right answer **if and only if its initialiser runs after ours**, and the two are
ordinary C++ static initialisers in one DLL whose relative order nothing fixes (mingw's `.ctors`
walk runs backwards through the link order, which puts a static library's initialisers first).
`ModelLimits::Install` now assigns both pointers itself and the log says which way round it fell.

### 7b. `GAME_REQUIRED` is not "ask nicely" — it is "keep this for ever"

`CStreaming::RequestModel(id, GAME_REQUIRED)` is what San Andreas asks its own zone peds with, and
it reads like a polite request that the streamer may refuse or undo. It is neither.

- `CStreaming::RemoveLeastUsedModel` (0x40CFD0) walks the loaded list and at **0x40D029** does
  `test byte ptr [edi+6], bl` against the caller's don't-remove mask, skipping any entry whose
  flags match. `GAME_REQUIRED` (0x02) is in that mask.
- Worse than skipped: such a model is **not on that list at all**. Look at what
  `CStreaming::SetModelIsDeletable` (0x409C10) does — it clears bit 0x02 at +6 and then, at
  **0x409C41**, calls `CStreamingInfo::AddToList` to put the entry *back* on the least-recently-used
  list. Clearing the flag is what makes a model evictable again, and nothing else does.
- San Andreas clears its own. `CStreaming::RemoveCurrentZonesModels` (0x40B080) is the call it
  makes when the popcycle zone changes, and it is why walking from one district to another does not
  slowly fill the streamer with everybody you have ever walked past.

The pedestrians never made that call, so every ped model any district ever wanted stayed resident
for the session — Vice City alone names 67 groups over 43 districts. **The cost is not paid by the
pedestrians.** It is paid by whatever the streamer evicts instead, and what San Andreas evicts and
reloads most is vehicles: `CStreaming::LoadZoneVehicle` brings in the cars for the player's zone
and `CStreaming::RemoveLoadedVehicle` takes the old ones out. Starve that rotation and
`CCarCtrl::ChooseModel` → `CLoadedCarGroup::PickLeastUsedModel` hands the zone whatever survived.

**That is a Feltzer in Jefferson, caused by a pedestrian in Vice City, with not one vehicle of ours
ever created.** It was confirmed the blunt way: with `RegionPeds`, `RegionTraffic`,
`RegionParkedCars` and `RegionPolice` all set to 0 — same build, same game folder, same other mods
— Los Santos is clean.

`src/peds/StreamedModels.cpp` is the rule now, and it is one sentence: **borrow, do not keep.** Ask
with `GAME_REQUIRED` because that is what gets a model loaded promptly, then hand it straight back
with `SetModelIsDeletable` the moment it has loaded. From that point it is protected exactly the way
San Andreas' own are — by the peds standing in it, since `RemoveLeastUsedModel` checks
`CBaseModelInfo::m_nRefCount` at +8 (0x40D064) and will not touch a model with an instance in the
world. Anything still outstanding goes back when the player leaves every region.

**Nothing else in this project may call `CStreaming::RequestModel` and walk away.** F8 reports how
many models are outstanding, and the answer should be a small number that keeps returning to zero.

### 8. A car of ours that is forgotten rather than handed back reaches Los Santos

`docs/traffic-and-population.md` §1 explains why our vehicles are `PERMANENT`, and the section
above explains why they are not `PARKED`. Neither says what happens to one that stops being ours
without going through `VehicleSpawn::HandBackToTheGame` — and the traffic had four such exits: the
player getting in, the car being shunted off its road, the whole list being dropped when the player
leaves the region, and the list being dropped between games.

`CCarCtrl::CreateCarForScript` **sets `bIsLocked`** at 0x43205F — the flag whose own name is
"locked by the script (cannot be removed)" — and `PERMANENT` is a counter nothing budgets against.
So a forgotten car is a car San Andreas can never take away. Three things follow, and the third is
the one that shows up hundreds of units away in a game the player thinks is vanilla:

1. It holds a **vehicle pool slot** for the rest of the session. Drive in and out of Vice City a few
   times and the pool is full of cars standing in an empty city, at which point
   `CCarCtrl::RemoveCarsIfThePoolGetsFull` starts removing cars at 8 free slots — and the only ones
   it *can* remove are San Andreas' own.
2. It keeps running its mission and its physics, in a city nobody is in.
3. It holds a reference on its **model**. `CStreaming::RemoveLoadedVehicle` reads
   `CBaseModelInfo::m_nRefCount` at +8 (the test is at 0x40C052) and refuses to unload a model that
   still has an instance in the world. Enough pinned models and `CStreaming::LoadZoneVehicle` can
   no longer rotate the loaded set as the player crosses zones, so `CCarCtrl::ChooseModel` →
   `CLoadedCarGroup::PickLeastUsedModel` keeps drawing from cars that were loaded for somewhere
   else. **That is what "the wrong cars are spawning for this zone in Los Santos" looks like from
   the inside**, and nothing about it points at an imported city.

Every exit now goes through one of two calls and there is no third: hand it back, or take it away.
Taking it away is preferred when the player cannot see it, because handing a dozen cars back at once
puts a dozen entries on `NumRandomCars`, which `GenerateRandomCars` stops at 45.

### 9. There are three "find the ground" routines and only one of them can say "there is none"

This decides whether an imported city has anybody standing in it, so it is worth stating flatly.

| | searches | reports failure? |
|---|---|---|
| `CPedPlacement::FindZCoorForPed` (0x616920) | `z + 0.5` down to `z − 100` — the floats at 0x858B8C and 0x858628 | no, and it is a band around the height you already believe, so it cannot find a surface above you |
| `CWorld::FindGroundZForCoord` (0x569660) | from **z = 1000** down (`0x447A0000` planted at `[esp+0x34]`) | **no — and this is the trap** |
| `CWorld::FindGroundZFor3DCoord` (0x5696C0) | from the z you give it down to **−1000** (`0xC47A0000` pushed at 0x5696F7) | **yes.** It takes a `bool*` and writes 0 through it at 0x569711 |

The middle one looks like exactly the right question — x and y only, no z to get wrong. Read its
failure path at 0x5696A5:

```
test al, al                  ; did ProcessVerticalLine hit anything?
jne  0x5696B3                ; yes - return the hit
fld  dword ptr [0x858BA4]    ; no  - return the float at 0x858BA4
ret
```

and the float at **0x858BA4 is 20.0**. When there is no collision under a point it returns twenty,
and twenty is a perfectly ordinary height: Vice City's streets sit around 12 to 20 in the region's
own coordinates. **A failed probe is indistinguishable from a good answer**, so a caller that trusts
it puts a pedestrian a few units above the road and watches them fall. That is what the player saw,
and it is why `src/region/Ground.cpp` exists rather than a one-line call. `FallRecovery.cpp` already
knew this and used the honest variant; the population code did not, and the two had grown separate
ground-finding code — the user asked whether they should share one, and they were right.

**What the graph itself is worth.** Sampling every ped node and every car node in Vice City against
the IPL props standing within fifteen units of it: median disagreement **1.3 units**, quartiles 0.2
and 3.4, tail about 3.5. The graph and the map agree. An earlier reading of "twenty units below the
player's feet" was the player being somewhere higher than the street, not a datum fault — the
in-game correction lines from a real session say 3.3 and 3.4 units, which is the tail and no more.
`Ground::Under` therefore lifts the start of its search 12 units — roughly three times the worst
disagreement, and still well under one storey, so a node beneath a bridge stays beneath the bridge.

All three spawners now use it, and **all three refuse to place anything when it says there is no
ground**, which is the part that matters: a bay or a node over a hole should be skipped, not filled
at a fabricated height. Each reports the refusals in its per-second line.

### 10. A spawn band that nearly touches its own cull band empties itself

The traffic reported, every second for a whole session, `3 placed … 0 driving … 3 taken away`. Cars
were being made and removed at the same rate, and "taken away" is our own distance cull. The
arithmetic, which nobody had done:

- `NodesNear` finds nodes within `RegionTrafficRadius`, **90** by default.
- `LanePoint` then steps sideways off the node by `(lane + 0.5) × 5.4` — up to **18.9** more in the
  outer lane of a four-lane road.
- So a car could be created **109** units out, and the cull sat at `90 × 1.4` = **126**.

Seventeen units of room, at a cruise speed of 18. A car crossed it in about a second and was culled
before it had driven anywhere. San Andreas' own numbers are the shape to copy — it creates between
about 42 and 50 and removes past 65, a gap of a *third* of its spawn radius rather than a sixth. Two
changes make ours the same shape: the lane offset now counts against the spawn radius, so nothing is
created past 90, and the cull is at `× 1.75` = 157, leaving 67 units to drive in.

**The general rule, and it applies to the pedestrians too:** the distance a thing is *created* at is
not the radius you searched with, and the cull has to be outside whatever the creation can actually
reach. Any spawner here should be able to state both numbers.

### And the handle is not the whole record

A `CPools` handle is `(slot << 8) | the slot's re-use byte`, and `CPools::GetVehicle` (0x54FFF0)
compares that byte before handing a vehicle back — which is why a handle to a destroyed car reads
as nothing. But `CPool`'s re-use counter is **seven bits**, so the 128th re-use of a slot answers to
the handle stored for the first. San Andreas has the same property and lives with it because it
re-checks nothing; we would be writing status, mission, cruise speed and `createdBy` into whatever
car had landed in that slot — one of the game's own.

So `VehicleSpawn::Resolve` takes the handle *and* the model the car was created with, and requires
`m_nCreatedBy` (+0x4A4) still to read `PERMANENT`. San Andreas puts nothing on that counter by
itself — its traffic is `RANDOM`, its generators `PARKED`, its script cars `MISSION` — so the two
together are as good as a name. Nothing in either module writes to a vehicle it did not resolve
that way.

---

## For `docs/testing-checklist.md`

The section below is written to drop into that file as **5**, in its style. It is here because that
file lives on `docs/traffic-and-testing` and copying it wholesale onto this branch would make an
add/add conflict out of a doc that should merge cleanly.

---

### 5. Parked cars — and the second budget

Do the San Andreas half **first**, and do it before section 4's traffic run rather than after, so
that if San Andreas' own kerbs empty you know which of the two branches did it.

- **Before starting the game, read the log line.** Start-up should say
  `[vicecity] parked cars: 148 space(s) over x ... y ...` and
  `[libertycity] parked cars: 147 space(s) ...`, then
  `parked cars: 2 region(s) will have their own cars at the kerb, up to 14 at a time within 110
  units; a space the player empties stays empty for 720s`.
- **Walk San Andreas first and look at its parked cars.** Grove Street, the Idlewood side streets,
  the Vinewood hills, the LV Strip car parks. Count them, roughly. This is the control.
- **Then walk a Vice City street.** Cars should be standing at the kerbs, facing along the road
  rather than across it, on the ground rather than in it. **F8** reports how many.
- **Then go straight back to San Andreas and look at its parked cars again.** This is the check that
  matters most — see below.
- **Stand still and stare at an empty space.** Nothing should appear while you are looking at it.
  Turn around, count three, turn back: now there should be a car there.
- **Steal one, drive two streets away, come back.** The space should be **empty**. Wait twelve
  minutes of play and come back again: now it may be full.
- **Blow one up and walk away.** Come back: the burnt wreck may have gone, but the space should
  still be empty, not holding a fresh car.
- **Drive a stolen one out of Vice City and into San Andreas.** It should stay with you the whole
  way and behave like any other car — this is the hand-back working.
- **Try the doors on several.** Some should be locked; most should not.
- **Drive fast down a street with the traffic on as well**, and watch for a frame-rate step or a
  stutter as spaces fill.

**The failure to watch for above all others:** *San Andreas' own parked cars disappear.* Empty kerbs
in Los Santos, no cars outside the houses in Grove Street, nothing in the car parks — while the
imported city has cars at its kerbs. That is our cars being counted on `NumParkedCars`, whose budget
`CCarGenerator::DoInternalProcessing` checks at **five**, globally. It is the same class of
regression as the traffic one and it is the reason this work exists in the shape it does; see
`docs/parked-cars.md`. Check `RegionParkedCars = 0` restores them before reporting anything else.

**Other failures and their signatures.**

- *A car sinks into the road, or floats above it* — the −100 "find the ground" sentinel is not
  reaching `CreateCarForScript`, most likely because the region has a `WorldOriginZ` and it is being
  added to the sentinel.
- *Cars face across the road, or all face the same way* — the heading is not being converted from
  degrees, or its sign is wrong.
- *A car creeps forward, or drives off on its own* — the status is not `STATUS_ABANDONED`. Anything
  that reaches `AI::UpdateCarAI` is being driven.
- *Cars appear in front of you as you walk* — the camera test; a space closer than 70 units should
  never fill in shot.
- *Spaces refill the moment you look away* — `RegionParkedCarsRespawn` is not being applied, or the
  space is not being remembered as emptied when the player takes the car.
- *The same space is always the same car and always locked* — the lock chance is being rolled per
  space rather than per car.
- *A crash on the first parked car* — a vehicle model was used before it finished streaming; the
  fault is inside `CVehicleModelInfo::CreateInstance` at 0x4C9691, the same one traffic can cause.
- *A crash after a long drive between the two cities, or vehicles stop appearing anywhere* — the
  vehicle pool. F8's counts are the evidence; parked cars should stop being created before traffic
  does.
- *No parked cars at all and a log line about a byte check* — another mod owns
  `CCarCtrl::CreateCarForScript` and nothing was patched, deliberately. The traffic will be off too.
- *No parked cars and no log line at all* — `RegionPeds` is off. The parked cars ride on the same
  once-a-frame hook as the pedestrians and the traffic.

**Expected and not a bug:** the same street does not have the same cars twice, because 61 of Vice
City's 148 spaces asked the source game for "any ordinary car" rather than for something in
particular; a police car outside the Vice City station that is a Los Santos police car; and a space
whose car has not streamed in holding an ordinary saloon instead.

---

### 6. Nothing spawns anywhere — the one that reports itself working

This is section 5's and section 4's failure with no signature of their own, so it gets a check of
its own. The symptom is: the log is perfect — node counts, bay counts, budgets, "will be
populated", "will have their own cars at the kerb" — and an imported city has no people, no
traffic and no parked cars in it at all.

- **Read the two new start-up lines.** `model id limit ...: plugin-sdk's cached array pointers ...`
  says which of two static initialisers ran first, and either answer is fine — it saying nothing at
  all means the model id limit was not raised, in which case nothing here applies.
- **Stand in a Vice City street and press F8.** The line to read is
  `population budgets: traffic N car(s) from the zone's popcycle row, M of 20 placeholder model(s)
  in memory; parked cars K at once, J bay(s) armed and in range`.
  - **M is 0** — no vehicle model in the whole game reads as loaded. That is the streaming array
    being read in the wrong place; see *Two more the engine does that are not obvious*, §7. It is
    the only one of these that also takes the pedestrians down.
  - **N is 0** with M nonzero — San Andreas' own popcycle says no cars belong here. Out beyond the
    map every zone resolves to info zone 0, so this is the whole-map `SAN_AND` row; `RegionTrafficDensity`
    scales it and cannot rescue a zero.
  - **J is 0** while standing on a street the source game parked cars in — the bays are all
    remembered as emptied, or the radius is too small, or the region origin does not match the one
    the bays were written in.
- **The start-up line no longer prints a traffic car count**, and that is deliberate: at start-up
  the player is not standing in any zone, all four popcycle floats are zero, and the old line said
  "up to 0 car(s)" in a game whose traffic was about to work perfectly well. F8 is where the live
  number is.
- **Then read the three per-second lines**, which is what actually settles it. With `Debug = 1` the
  pedestrians, the traffic and the parked cars each write one line a second saying where every
  attempt went — `peds/sec:`, `traffic/sec:`, `parked/sec:`. A population count cannot tell
  "nobody was made" from "everybody was made and taken away again"; these can, and three rounds of
  reasoning from counts alone got the wrong answer before they existed. Read them in this order:
  - **`0 attempt(s)`** — the tick never reached the spawn loop. The same line says which gate
    stopped it: at budget, waiting for the spawn interval, pool full, no model in memory, no node
    in range.
  - **attempts but `0 placed`** — the loop ran and every candidate was rejected. The line breaks
    that down: too near, in shot, on San Andreas' own ground, crowded, or the game refused to make
    the vehicle. `CCarCtrl::CreateCarForScript` has no path that returns nothing for a car model,
    so "said no" there means the model was not what it was taken for.
  - **`placed` high and `vanished` just as high** — things are being made and lost every tick. That
    is the worst of the three, because for the traffic it means cars are being left in the world
    that nothing can remove: see §8.

### 7. San Andreas' own cars are healthy — the check that matters most

Do this **before and after** a trip to an imported city, in the same session, without restarting.
It is the whole of what "we did not break San Andreas" means, and it is cheap.

- **Stand in Idlewood, in Los Santos, before going anywhere near a region.** Watch the traffic for
  a minute. Write down what goes past: it should be Tahomas, Greenwoods, Blades, Remingtons,
  Voodoos, Majestics — poor-area cars — and they should arrive **undamaged**. Note the parked cars
  in the side streets too.
- **Now drive to Vice City, drive around it for five minutes, and come back to the same corner of
  Idlewood.** Watch for another minute.
- **It must look the same.** The same kinds of car, the same number of them, and no visible body
  damage on cars that have just appeared.
- **A sports car in Idlewood is the signature.** A ZR-350, a Turismo, a Banshee, an Infernus
  arriving in a poor neighbourhood — repeatedly, not once — means San Andreas' loaded vehicle set
  has stopped rotating with the zone. That is cars of ours pinned in the world holding references
  on their models; see the section above. **F8 in Los Santos will show a nonzero
  `car(s) driving its carriageways` or `parked at its kerbs` while you are nowhere near a region**,
  which is the proof, because both counts should be zero the moment you leave one.
- **Cars arriving already dented is the same fault with a different face** — a full vehicle pool
  makes San Andreas remove and re-create cars in tight spaces. Check the pool the same way.
- **The control.** `RegionTraffic = 0`, `RegionParkedCars = 0` and `RegionPeds = 0` in
  `MapExpander.ini`, then repeat the Idlewood run. If Los Santos is still wrong with all three off,
  it is not the population code — and if it is still wrong with `MapExpander.SA.asi` moved out of
  the folder altogether, it is not this plugin at all. Do both before reporting it: `modloader`,
  CLEO scripts and a vehicle mod can all change which cars a zone spawns and what state they arrive
  in, and none of them announce it.
- **Nothing survives a restart.** Every change this plugin makes to San Andreas — the moved arrays,
  the rewritten instructions, the extra zones, the position scale — is made to a running process and
  is gone when the game exits. The one exception is a **savegame written while the plugin was
  loaded**, which carries the region's zones; loading it without MapExpander would overrun the
  game's 380-zone array, and the log warns about that at start-up. So if Los Santos looked wrong,
  restarting the game clears it; it does not clear a save made in that session.
