# Testing checklist

Five bodies of work are finished and none has been run in a game. Nothing in this project is tested
by the people who write it — the plugin cannot be exercised without launching San Andreas, and that
is done by hand. This is the list, in the order that finds the most with the least driving.

Set `[World] Debug = 1` first. Debug keys: **F6** drops you under the world on purpose, **F7** cycles
to the next region, **F8** prints position, zone, subsystem counts and where a death or an arrest would put you, **F9** gives the imported
weapons. `MapExpander.log` is the other half of every check below — most failures announce themselves
there rather than on screen.

The branches, and what each needs:

| branch | what it changes | risk |
|---|---|---|
| `feature/sector-grid-fps` | grid fitted to real building extents | low — nothing moves |
| `feature/fall-recovery` | recovery when you fall through the world | low — inert outside a region |
| `feature/region-restart-points` | death and arrest wake you in the imported city | low — a plain forward outside a region |
| `feature/peds-and-police` | pedestrians, police, and now vehicles | medium — touches population |
| (same branch) `PathScale` | widened path encoding | **off by default; leave it off for now** |

---

## 1. The sector grid — start here

This is the one already installed, and the one whose failure would be least confusing. Only
`FitSectorsToContent` is on by default; `PackRegions` and `MapWindow` are both off.

- **Start a game and read the log.** It should report the measured content box for each region and
  the sector size that follows. Vice City declares 4000x4000 and should measure about 3436x3681.
- **Drive around San Andreas normally.** Nothing should have moved, because this setting changes the
  grid's *extent*, not any entity's position.
- **Frame rate in a busy area** — Los Santos downtown, then the same spot in an imported city.

**Failures.** If anything has moved position, the wrong box was measured. If the log reports a region
whose IPLs it could not read, it fell back to the declared tile grid and the saving was not taken.

---

## 2. Falling through the world

- **Stand in a Vice City street and press F6.** Expected: a brief drop, then standing again within a
  few units of where you were, inside a second. The log should say roughly
  `fall recovery: the player went under the world at (...) and is back on ground at (...), 4 units away`.
- **Repeat in a car, on a rooftop, on a bridge, and at the water's edge.**
- **Press F6 in San Andreas.** This is the control: you should get the *old* behaviour — thrown to
  the nearest road node — and a log line saying the fall was outside every region.

**Failures.**

- *Thrown to San Andreas from inside a region, no `fall recovery:` line at start-up* — it never
  installed. The log says which of the three reasons: setting off, no regions, or SA-MP.
- *Start-up says the prologues do not match San Andreas 1.0 US* — another mod owns those functions
  and nothing was patched, deliberately.
- *`nothing to stand on within 160 units`* — the hole is bigger than the search or the collision is
  not in memory. The fall is handed back to the game.
- *Repeated `(repeat fall, searching wider)` then a give-up line* — recovery is landing you back in
  the same hole, and those coordinates locate a real collision gap worth reporting.
- *You come back on a rooftop* — the second pass searched from above and hit a roof. Reachable, but
  worth reporting.

---

## 3. Pedestrians and police

- **Walk a Vice City street.** People should appear and walk, not stand. The log names the district,
  group and model for each spawn when `Debug = 1`.
- **Cross between districts** — Ocean Beach into Little Haiti, say — and check the crowd changes.
  Each district draws from the donor's own ped groups, so it should not be the same faces everywhere.
- **F8** reports how many are walking and how many police are out.
- **Raise a wanted level** and confirm police arrive on foot and engage.
- **Watch streaming as you cross a district boundary** — ped model churn is the thing most likely to
  stutter.

**Failures.** Peds standing still rather than walking means the task was built but not driven. Peds
walking toward the world origin means something reached `CTaskComplexWander`, which must never be
used out here. No peds at all with a start-up log line about byte checks means a prologue did not
match and nothing was patched.

---

## 4. Traffic — and the regression that matters

Do this last, and do the San Andreas half **first**.

- **Drive San Andreas before going anywhere near an imported city.** Los Santos downtown, the
  Mulholland intersection, the LV Strip. Traffic should be exactly as you remember: same density,
  same behaviour at junctions.
- **Then drive an imported city.** Cars should appear on the donor's own carriageways, hold a lane,
  slow for the car in front, and give way at junctions.
- **Then come back to San Andreas and check its traffic again.**

**The failure to watch for above all others:** *traffic works in the imported city but San Andreas'
own traffic thins out or vanishes.* That is our cars being counted against San Andreas' own budget —
`GenerateRandomCars` stops entirely at 45 random cars — and it is the regression this work was most
at risk of causing. See `docs/traffic-and-population.md`.

**Other failures and their signatures.**

- *Cars drive through each other and never brake* — the driving style is not 6, which is the only
  value that gets both weaving and braking.
- *Cars set off then stop after a few seconds* — the mission was cleared by `UpdateCarAI`, meaning
  `m_nStraightLineDistance` is not being held high.
- *Cars slide rather than drive* — status is not `STATUS_PHYSICS`.
- *A crash on the first car* — a vehicle model was used before it finished streaming; the fault is
  inside `CVehicleModelInfo::CreateInstance` at 0x4C9691.
- *Cars in the wrong place entirely, or a hard crash on entering a city* — that would be the path
  encoding, which is **off by default**. If `PathScale` is on, turn it off.

**Expected and not a bug:** no traffic lights, and cars cutting corners rather than swinging through
junctions on an arc. Neither is reachable without the link data.

---

## 5. Dying and being arrested in an imported city

This one is cheap to check and the failure is unmistakable, so do it early. It needs a re-import (or
the `restarts.dat` that shipped with this branch already in place) — the region folder has to have a
`restarts.dat` and its `region.ini` a `Restarts =` line, or nothing at all changes.

- **Read the log at start-up.** It should say, once per region,
  `[vicecity] restarts: 4 hospital and 4 police point(s) of its own; San Andreas' own arrays are not
  touched and the savegame is unchanged`, and 3 and 3 for Liberty City.
- **F7 to Vice City, then F8.** Before dying at all, the read-out says
  `restart points: [vicecity] die here -> (x, y, z) heading h; arrested here -> (x, y, z) heading h`.
  Both should be Vice City coordinates — x between 3998 and 7998 with the shipped origin — not
  San Andreas ones. This is the whole check without the driving.
- **Die.** Walk into the sea, or shoot the ground with a rocket. You should wake outside a Vice City
  hospital, facing the way that game faced you, with the usual hospital fee and weapon loss. The log
  writes one line: `[vicecity] restarts: death at (...) answered with the region's own hospital at
  (...) heading ...  # BEACH3 @0x328a9`.
- **Get arrested.** Raise a wanted level in Vice City and stand still. You should come out of a Vice
  City police station, not a Los Santos one.
- **Do both again in Liberty City.** Its Portland police station is the one whose height comes from
  the ground rather than from the script, so it is the one worth watching: you should be standing on
  the street outside, not sunk into it and not on a roof.
- **Now the control: die and get arrested in San Andreas.** Los Santos, then somewhere in the
  countryside. You must get exactly the vanilla behaviour — All Saints, the LS police station, the
  nearest hospital to wherever you were — and **no** `restarts:` line in the log at all. This is the
  case that must not have changed, and silence in the log is the proof.
- **F8 in San Andreas** should say `restart points: outside every region - San Andreas answers death
  and arrest here, as it always did`.
- **Save the game in Vice City, then load it.** The save must load normally, and a save made before
  this branch must still load after it. Nothing of a region's ever enters the block `CRestart::Save`
  writes, so a save is byte for byte what vanilla writes — if that stopped being true, loading is
  where it would show, as garbage in the blocks *after* the restart one (pickups in the wrong place,
  car generators, radar blips).

**Failures.**

- *No `restarts:` line at all at start-up* — it never installed. Either the region has no
  `restarts.dat` (re-import), `RegionRestartPoints` is 0, or this is SA-MP; the log says which.
- *Start-up says the two searches do not start with the bytes San Andreas 1.0 US ships* — another mod
  owns `CRestart::FindClosestHospitalRestartPoint` or `FindClosestPoliceRestartPoint` and nothing was
  patched, deliberately. Both are left vanilla or neither is hooked.
- *You still wake in All Saints* — read the log line for that death. If there is none, the hook did
  not fire, which means the position the game searched from was not inside the region's tile grid.
  F8 where you died and compare.
- *You wake in the right city but underground, or on a roof* — that is a point whose height came from
  the ground search. The log names it; the fix is a real z in `restarts.dat` for that line.
- *`the %s point at (...) asks for the ground and the region's collision has none there`* — the
  region's `.col` did not cover that spot. You will be put at the region's floor plus 20, which is at
  least the right city.
- *`this region has no police point, so its hospital point is used instead`* — the mine found only one
  kind. Neither shipped region should ever say this; if one does, its `restarts.dat` is truncated.
- *San Andreas' own deaths changed in any way* — stop and report it. Nothing outside a region is
  supposed to be touched, and the hook is a plain forward there.

---

## What a clean run looks like

San Andreas plays exactly as it always did — same traffic, same frame rate, same everything — and the
imported cities have people on the pavements, cars on the roads, police that respond, and a fall
through the world puts you back on the street you fell through rather than in Los Santos — and so
does dying in one, at that city’s own hospital.
