# Testing checklist

Four bodies of work are finished and none has been run in a game. Nothing in this project is tested
by the people who write it — the plugin cannot be exercised without launching San Andreas, and that
is done by hand. This is the list, in the order that finds the most with the least driving.

Set `[World] Debug = 1` first. Debug keys: **F6** drops you under the world on purpose, **F7** cycles
to the next region, **F8** prints position, zone and subsystem counts, **F9** gives the imported
weapons. `MapExpander.log` is the other half of every check below — most failures announce themselves
there rather than on screen.

The branches, and what each needs:

| branch | what it changes | risk |
|---|---|---|
| `feature/sector-grid-fps` | grid fitted to real building extents | low — nothing moves |
| `feature/fall-recovery` | recovery when you fall through the world | low — inert outside a region |
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

## What a clean run looks like

San Andreas plays exactly as it always did — same traffic, same frame rate, same everything — and the
imported cities have people on the pavements, cars on the roads, police that respond, and a fall
through the world puts you back on the street you fell through rather than in Los Santos.
