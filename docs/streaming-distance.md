# Streaming distance, and the difference between a position and a distance

Scenery appeared closer to the player than it used to, and got closer every time the sector grid
improved. Nothing in this project ever set a streaming distance, which is the interesting part: the
distance was a side effect of the sector grid, and it had been one since the grid was first
stretched.

## What the near-field pass actually does

`CStreaming::AddModelsToRequestList` (0x40D3F0) runs every frame and asks the streamer for the
models standing round the camera. Disassembled out of gta_sa.exe 1.0 US, it is:

```
r  = 80.0f                                  ; 40.0f on the near call, chosen at 0x40D400/0x40D40A
R  = (int)(r * FINE_SCALE)                  ; 0x40D444 - a DISTANCE, converted to a count of cells
box = pos +/- r                             ; four corners, world units, 0x40D416..0x40D43C
for every sector the box touches:           ; 0x40D451..0x40D57E - POSITIONS, converted to indices
    d2 = (sx - cx)^2 + (sy - cy)^2          ; in cells
    if   d2 <= (R - 1)^2:  CStreaming::ProcessEntitiesInSectorList(list, flags)          [0x40C450]
    elif d2 <= (R + 2)^2:  CStreaming::ProcessEntitiesInSectorList(list, x, y,
                               minX, minY, maxX, maxY, r, flags)                         [0x40C270]
```

The two list processors are not the same function and this is the whole story:

- **0x40C450 takes no coordinates at all.** Every entity in the list gets `CStreaming::RequestModel`
  with no distance test and no draw-distance test. This is the "you are standing here, load
  everything" ring.
- **0x40C270 takes the box and the radius.** It rejects any entity outside the box, and then any
  entity further than `min(r, drawDistance * ms_lodDistScale)` — squared at 0x40C271, compared at
  0x40C3FF. Everything about this half is in world units and is unaffected by the cell size.

The float-to-int at 0x821B40 is the MSVC `_ftol`: it rounds with `fistp` and then corrects back
toward zero, so it **truncates**. `(int)(80 * 0.02)` is 1, not 2. That matters, and it is why the
inner ring is `d2 <= 0` — one sector, the one the player is standing in.

## The bug

Every one of the four position conversions in that function needs a scale **and** a centre, and
`SectorGridSites.h` repoints both halves of all four. The conversion at 0x40D444 needs the scale
and must not have the centre, because it converts a distance rather than a position — and the site
scan recognises a conversion by an `fmul` of the scale followed by an `fadd` of the centre. Having
no centre, it was never recognised, and its operand was left reading San Andreas' pooled 0.02.

On a vanilla grid that is correct by definition: 80 world units over 50-unit cells is 1.6 cells, R
is 1, and the unconditional ring is the player's own 50-unit sector. Once `WorldSectors` stretches
the cell, R is still 1 — it is still computed against 0.02 — but the sector it names is no longer
50 units across. So the ground the streamer loads without asking any distance question became the
cell, and followed it everywhere it went.

| grid | cell | unconditional ring | distance-tested | effective reach |
|---|---|---|---|---|
| vanilla San Andreas | 50 | the player's 50-unit cell | 80 units | 80 |
| Liberty City off the west coast | 128 | the player's 128-unit cell | 80 units | up to 128 on an axis, 181 into a corner |
| both cities, current fit | 121 | the player's 121-unit cell | 80 units | up to 121, 171 |
| `FitSectorsToContent` measuring real extents | 87 | the player's 87-unit cell | 80 units | up to 87, 123 |
| **MapWindow, if it were switched on** | **50** | the player's 50-unit cell | 80 units | **80 — the shortest possible** |

Read the table downwards and the history reads back out of it: every improvement to the grid took
ground away from the streamer, and nobody touched a streaming setting to do it. Read the last row
and the reason this had to be fixed before `MapWindow` is switched on is plain — its entire purpose
is to get the cell back to about 50, and on the old behaviour that would have cut the near field to
vanilla's 80, the shortest this plugin has ever run.

Note which way the bug pointed. It made this plugin stream **further** than San Andreas ever
intended, and the "regression" was that accident being withdrawn. A strictly correct fix - pin the
radius to the 80 units the game means - would be shorter than any build that has shipped.

## What changed

`src/limits/StreamRange.cpp`. The radius becomes a fixed number of world units, from
`[World] StreamDistance`; `-1` (the default) means 200 units and only applies when this plugin is
the one that stretched the grid, `0` leaves San Andreas alone, anything else is world units.

| | far pass | near pass |
|---|---|---|
| vanilla San Andreas | 80 | 40 |
| this plugin before the fix | 80 + the player's whole cell | 40 + the same |
| after, default | 200 | 100 |

Four byte-verified sites, all inside that one function; nothing is written unless all four still
hold what this build of gta_sa.exe should have, and a mismatch leaves the game vanilla with a log
line.

| site | was | is |
|---|---|---|
| 0x40D404 | imm32 `80.0f` | the configured distance |
| 0x40D40E | imm32 `40.0f` | half of it, keeping vanilla's 2:1 |
| 0x40D446 | disp32 `0x858B38` (0.02) | our own cells-per-world-unit |
| 0x40D63C | `dec edx; imul edx, edx` | `xor edx, edx` and two nops |

The last one is what keeps it cheap. `(R - 1)^2` is the ring requested with no distance test at
all, so raising the radius without zeroing it would ask the streamer for every entity within two or
three cells whatever its draw distance says — a great deal of streaming for models that will never
be drawn, which is precisely the per-frame cost the sector-grid work exists to reduce. Zeroed, the
unconditional ring stays the player's own cell, exactly what vanilla does at its own 80-over-50, and
the extra distance is carried by the outer ring where every entity is still tested.

With the inner ring pinned, an over-large `R` costs nothing — the loop only ever walks the sectors
the box touches, and `R` can only reject sectors inside it — so the conversion factor is set
deliberately generous, `R = ceil(distance / cell) + 3`. That reaches `distance + 3.5` cells and can
never reject a sector that could hold an entity inside the box. Erring the other way is what
silently clips streaming, so it errs this way on purpose. `WorldSectors::Refit` recomputes it, so a
`MapWindow` crossing keeps the distance it had.

### What it costs

The pass now walks the sectors touching a 400-unit box instead of a 160-unit one — 6.25 times the
ground, every frame. In sectors, at 121-unit cells that is about 4x4 instead of 2x2; at 87-unit
cells about 6x6 instead of 3x3. Entities inside those sectors are still rejected by the box and by
their own draw distance, so the number of models actually requested grows by much less than the
area, but the walk itself is not free and it is paid on the same frame as the collision and render
scans the grid work was making cheaper. `StreamDistance` is the number to bring back down; 80 is
vanilla, 0 leaves the function untouched.

## The audit: every site where a distance and a position could have been confused

`tools/scan_sector_sites.py` classifies a site by which constant it reads and whether that constant
is a scale or a centre. It does not ask whether the value being converted is a position or a
distance, and those want different handling: a position needs the scale and the centre, a distance
needs the scale alone. Getting either wrong is silent.

Every instruction in the executable that names one of the four constants was enumerated and matched
against the 604 entries in `SectorGridSites.h`: 1036 references, 604 repointed. The 432 that are
not were then narrowed to the 65 functions that either contain a repointed site or address one of
the world-sector arrays. **Six of those 65 contain a reference that was left alone**, and every one
is accounted for below.

| function | site(s) | verdict |
|---|---|---|
| `CStreaming::AddModelsToRequestList` 0x40D3F0 | 0x40D444 `fmul` fine scale | **the bug.** A distance conversion the scan could not see because it has no centre. Fixed above. |
| `Camera::Process_Editor` 0x50F3F0 | 0x50F746, 0x50F786, 0x50F7D6, 0x50F826 `fld` fine scale, and 0x50F74E, 0x50F78E, 0x50F7DE, 0x50F82E `fadd` fine centre | **a real miss, from the instruction shape rather than the kind.** Four `while` loops that step a coordinate by 1.0 until its sector index leaves 5..115, keeping the editor camera inside the grid. The loop's entry test is written `fmul [scale]; fadd [centre]` and is repointed; the loop body is written `fld [scale]; fmul st(1); fadd [centre]` and is not, because the scan only looks for `fmul` of the scale. The body therefore clamps against vanilla's grid, confining the editor camera to about +/-2750 whatever the world covers. Bounded, so it cannot hang, and it is the debug editor camera rather than anything reachable in play — but the scan's pattern is one shape short and that should be written down. |
| `Entity::ProcessShiftSectorList` 0x546670 | 0x546B20, 0x546B31 `fld` coarse scale | **correctly left alone.** `fld 0.005; fcomp` and `fld 0.005; fstp` — 0.005 as an epsilon and a floor on a shift amount. Not a sector conversion; it happens to share the pooled constant. |
| `CPlayerInfo::Process` 0x56F8D0 | eleven `fmul` fine scale, 0x56F9DB..0x570044 | **correctly left alone.** `fld [global]; fmul 0.02; fmul [scale]; ftol; add [esi+N]` eleven times over — distance-travelled statistics accumulating at 2% a step. Not a sector conversion. |
| `CEntityScanner::ScanForEntitiesInRange` 0x5FFA20 | 0x5FFDFF `fcom` coarse centre, 0x5FFE0E `fld` coarse centre | **correctly left alone, and this is the confusion in the other direction.** `fild` an integer, clamp it to 15.0, `ftol` back: it is `min(count, 15)`, the last index of the scanner's 16-entry insertion sort at `[ebx + esi*4 + 0xc]`, `cmp esi, 0x10`. 15.0 is an array bound that happens to be the pooled constant the coarse grid uses as its centre. Repointing it would have written a grid coordinate into an array index. The scan avoids it only because it insists on the `fmul`-then-`fadd` pairing. |
| `CCover::Update` 0x6997E0 | 0x69992A `fcomp` coarse scale | **correctly left alone.** Immediately after an `fsqrt`: a near-zero test on a vector length against 0.005. |

Nothing else in the executable both reads one of the four constants and lives in a function that
touches the world-sector grid.

### The scans that are clean

- **`CStreaming::AddLodsToRequestList` 0x40C520**, the big-building and LOD half of the streamer, has
  no radius-to-sectors conversion at all: its radius is a world-units global (0xB76848), it converts
  only the four box corners, and it iterates the whole rectangle with no ring test. Cell-size
  independent as written.
- **The renderer** — `CRenderer::ScanWorld` 0x554FE0, `GetObjectsInFrustum` 0x554C60,
  `RequestObjectsInFrustum` 0x555960 — converts frustum corners in world space to sector indices and
  has no distance conversion. All of its references are repointed.
- **The collision camera cache and the ped scanners** are among the 65 functions and have no
  un-repointed reference other than the `CEntityScanner` array bound above, which is not a grid
  constant in use.

### Not resolved

`tools/verify_sector_sites.py` is the check that closes this off, and it has not been run. It walks
the executable once and reports two things: sites in the table whose result never reaches the
float-to-int helper — an unrelated 0.02 that happened to sit within ten instructions of an unrelated
60.0, which repointing would break — and references outside the table, inside a function that
touches the grid, that are not one of the fourteen accounted for above. It should print `OK`. The
Docker container carrying the disassembler died partway through the session that wrote it, so what
stands behind the audit above is the per-function enumeration (1036 references, 604 repointed, 65
grid functions, 6 with anything left alone) plus hand disassembly of each of those six, which is
enough to place every site but is not the same as the sweep having been run end to end.

    ./tools/dis.sh verify_sector_sites.py     # or run it against gta_sa.exe however you disassemble

## `DropUnlinkedLods` — ruled out as the cause, with one latent bug found

Distant geometry going missing reads exactly like short streaming, so it had to be checked.

It is not the cause here: it has been on by default since it landed and nothing on this branch
changed it, whereas the symptom changed between builds. But it does change how far an imported city
*looks* like it draws, and in one direction only. III and Vice City ship island-sized LOD meshes
(`IslandLODmainland` and their kin) drawn out to 3000 units, standing in for whole districts that
those games kept unloaded. San Andreas streams everything, so both the mesh and the real geometry
draw at once and the mesh sits through the buildings. Dropping them is right; the consequence is
that distant parts of an imported city are then filled in only by real geometry at its own draw
distance, where a vanilla San Andreas vista is filled in by LODs. If the complaint is specifically
about *far* distance in an imported city and not in San Andreas, `DropUnlinkedLods = 0` is the
one-line experiment — at the cost of the overlapping mess it exists to remove.

**The latent bug** is in `CityLoader.cpp`. The pairing pass skips any inst line that already carries
a LOD column (`if (inst.hadLodColumn || inst.lod >= 0) continue;`), so the LOD such a line points at
is never marked `claimed`. The drop pass then removes any unclaimed `lod*` line, and the fix-up
`inst.lod = moved[inst.lod]` writes -1 over the surviving HD's link. The comment above it — "Every
LOD an HD points at survived, so no link is broken by this" — is only true while no inst line
arrives with a LOD column, which is true of III and Vice City IPLs and not true of an SA-format
region IPL. Silent when it happens: the link is lost with no counter and no log line. Worth marking
the referenced LODs as claimed before the drop.

## What to look for in the game

Set `[World] Debug = 1` and read `MapExpander.log` first — everything here announces itself there.

- **At start-up the log should say** `streaming distance pinned to 200 world units (100 on the near
  pass)` followed by the note about the box growing. If it says nothing, either `StreamDistance = 0`
  or the grid was never stretched (auto mode leaves vanilla alone) — the world-sectors line just
  above says which.
- **If it says `ERROR: streaming distance left at San Andreas' own`**, one of the four sites did not
  hold what it should and nothing was written; the message names the site and what it found. That is
  another mod on the same function, and the game is left vanilla on purpose.
- **Drive a straight road in Los Santos and in an imported city.** Buildings should resolve at the
  same distance in both. Before the fix they resolved further out in whichever city had the coarser
  grid, which is the tell that this was ever cell-dependent.
- **Change nothing but the grid and check the distance does not move.** Turn `FitSectorsToContent`
  off and on, or move a region's origin: the log's sector size will change and the streaming
  distance must not. That is the actual regression test for this fix, and it is cheap.
- **Frame rate in the same busy spot before and after**, since the pass now walks about four times
  the sectors. If it costs more than the pop-in was worth, `StreamDistance = 120` or `150` is the
  dial, and `0` puts San Andreas' own 80 back.
- **When `MapWindow` is eventually switched on**, the distance must still read 200 in the log after
  a map crossing, not fall back toward 80. `WorldSectors::Refit` recomputes the conversion factor
  for the new cell; if a crossing ever shortens the draw distance, that is the thing that did not
  run.
