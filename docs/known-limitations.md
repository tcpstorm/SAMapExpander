# Known limitations

What does not work, why, and what it would take. Each entry says how it was established, so nobody
has to rediscover it — several of these were found the expensive way.

Anything here that is "not fixed" is a considered decision, not an oversight. Where a fix exists but
was rejected, the reason is given.

---

## Traffic does not spawn in an imported city

Nothing drives. Pedestrians and police on foot now do — see below — but no vehicle ever appears.

**Why.** San Andreas stores path node positions as `int16 × 0.125`, so a node can only describe a
point within **±4095.875**. Vice City sits near SA x 6000 and Liberty City near y 6000; neither fits,
and there is only about 1096 units of headroom either side of San Andreas' own map. The 8×8 grid of
750-unit path areas that `CPathFind::FindRegionForCoors` clamps into is a second obstacle but not the
fatal one — nothing validates a node against its area's box, so that part is survivable.

Two things that are **not** the problem, both checked: population density is live regardless of where
the player is, and there is no map-extent gate anywhere in the spawning code. So paths really are the
whole of it.

**What it would take — measured, not estimated.** `tools/scan_path_scale_sites.py` classifies every
one of the 491 instructions that read the shared `0.125` at `0x858C48` and writes
`tools/path_scale_sites.txt`. The result:

| | |
|---|---|
| 187 | `CPathNode` coordinate decodes |
| 94 | `CCarPathLink` coordinate decodes — **a second compressed path struct**, 14 bytes with its coordinates at +0/+2, which the earlier count missed entirely. Repoint only the nodes and traffic drives to the wrong places while pedestrian routing looks right |
| 3 | encode sites, all in `CPathFind::CompleteNewInterior`, through `8.0` at `0x859000` |
| 1 | load-time rescale of San Andreas' own shipped nodes. `LoadPathFindData` copies the 28-byte records out of `nodes<N>.dat` verbatim with no transform, so one hook after the read — the plain call at `0x452F75`, not the HOODLUM-stolen entry at `0x4529F0` — rescales all of them with no data file edited |
| 4 | region constants at `0x859A8C/90/94/98` (1/750, −3000, 3000, 750), or nodes are filed into the wrong area |
| 207 | sites that read the same constant and must be **left alone** — `CPickup`, `CTrainNode`, `CCarGenerator`, `COccluder`, the audio boxes, several render pipelines |
| 3 | honestly unclear: their feeding store is in another basic block and a linear walk cannot prove it |

Every one of the 281 confident sites is the same six bytes, `D8 0D 48 8C 85 00`, with its `disp32`
at instruction offset +2. So each change is mechanically *"expect those bytes at A, write the new
address into the last four"*, and every one is checkable against the file before and after with
nothing running. **That makes this materially more tractable than "271 sites that cannot be
validated" suggested** — the bytes are verifiable offline to the same standard as
`src/limits/ModelLimits.cpp`. What is not verifiable offline is the *set*: a decode that was missed
reads a widened node through the old scale and silently produces a position four times too small,
and the A2 evidence is inference from struct-offset sets rather than a proven pointer chain. Only
running the game settles that, and nobody has.

**Half of it is now done.** `src/limits/PathScale.cpp` repoints all 288 sites — 188 node decodes,
94 link decodes and 6 encodes — to floats of its own, and rescales San Andreas' own shipped nodes
as each area loads, so nothing in `data\paths` is edited. The scale is derived from the region set:
the smallest power of two whose ±32767 covers it, which for the two shipped cities is 4, giving
±8191.75 in steps of 0.25. Vice City's roads reach SA x 6873 and Liberty City's y 6570, so vanilla's
±4095.9 is short and the next step up is enough.

What that costs San Andreas is measured, not asserted (`tools/measure_path_rescale.py`, run over all
68237 shipped nodes): 56% of coordinates survive exactly, the rest move by exactly **0.125 units —
one step of the encoding the game shipped with**. Worst 3D displacement 0.2165, mean per axis 0.055.
San Andreas' lane width is 5.4 (the float at `0x858C50`, read by 26 instructions across `CCarCtrl`'s
lane placement and steering), so the worst case is 4% of a lane and the mean 1%. Only position is
touched: `CPathNode`'s x/y/z at +8/+0x0A/+0x0C are followed at +0x0E by a field holding a constant
`0x7FFE` in every shipped node, all three carry both parities freely, and no bitwise or
zero-extending access to those offsets exists anywhere in the path code. `CCarPathLink` keeps its
lane counts in two nibbles at +0x0B and its direction in two signed bytes at +8/+9.

**The other half — `RegionPaths` — is not done, and is off.** A region's roads are not in `CPathFind`
until something merges them into the path areas its ground falls into, and **all 64 areas already
hold nodes**, so there is no empty area to write into: it has to be a merge, with every node index
remapped. None of that can be checked without running the game, and getting it wrong damages San
Andreas' own traffic rather than only the imported city's. So `PathScale` refuses to patch anything
unless `RegionPaths` is on — widening the scale alone would change how the game reads its own paths
and buy nothing.

**Where the maps join.** Vice City's ground clamps into areas 31, 39, 47, 55 and 63 (column 7,
rows 3–7); Liberty City's into 57–62 (row 7, columns 1–6). They do not overlap each other. Driving
east out of San Andreas into Vice City stays in the *same* path area the whole way, so nothing
reloads at the boundary. But there is no road between the maps: San Andreas ends at x 3000 and Vice
City's westmost node is at 4176, about 1170 units of ground with no nodes at all (500 with
`PackRegions`). Traffic stops where the source game's roads stop, exactly as it does at San Andreas'
own map edge today — a connecting road would be content, not code. The cost of the merge, when it
lands, is that those five or six areas roughly double in node count, and `FindNodeClosestToCoors` is
linear over an area, so node searches along San Andreas' eastern and northern edges get about twice
as slow. Bounded, and in the emptiest part of the map.

**A 65th path area is not the way round it.** Four separate things are hard-wired to 64: the 0..7
clamps and `xreg + yreg*8` in `FindRegionForCoors`, a hard-coded `+0x100` interior block in nine
instructions of `CompleteNewInterior` alone, twelve per-area array base displacements, and the
streaming ids 25511..25574 which are exactly 64 wide with the animation block immediately after.

---

## What a region gets instead: pedestrians and police on foot

Since San Andreas cannot be made to populate these streets, the plugin does it — `src/peds/`.

The source game's own pedestrian graph is written out by the importer to `pednodes.dat` (Vice City
keeps it in the `path` sections of its IPLs at world coordinates; GTA III attaches a group to a road
model and means it once per placement of that road) and held in ordinary memory, where nothing is
compressed and nothing has a range. Peds are made by `CPopulation::AddPed` with `makeWander` false
and walked with `CTaskComplexGoToPointAndStandStill`, which never consults `CPathFind` — confirmed
by disassembling every method of it and its subtask. Who appears comes from the donor too: its
`pedgrp.dat`, its zone boxes, and the `SET_ZONE_PED_GROUP` calls in its `main.scm`, so each district
draws its own crowd. San Andreas' models stand in, paired one to one in the region's `pedgrp.dat`.

Police ride on the same graph, because all four of San Andreas' routes to a policeman are path-based
(`AddToPopulation`, `GenerateOneRandomCar`, `CreateWaitingCoppers`' car half, `GenerateRoadBlocks`).
The wanted system itself needs nothing: the stars, the decay, `CWanted`'s pursuit bookkeeping and
`CTaskComplexKillPedOnFoot` are all free of any map or path dependency.

**What is still missing out here, all of it for the same reason:** patrol cars, the police
helicopter, roadblocks, and the SWAT, FBI and army responses at four stars and above. Every one is a
vehicle placed on a car path node. Also the police radio's *location* dialogue — 
`CAEPoliceScannerAudioEntity::AddAudioEvent` matches the zone against a hard-coded 194-entry table of
San Andreas' own zones and says nothing when it does not find one; that is a zone dependency rather
than a path one, and fixable, but it is not fixed.

---

## Six Vice City models exceed San Andreas' own mesh ceiling

San Andreas' 16264 geometries never exceed **57 meshes**. After identical materials are merged, six
Vice City models still do:

| model | meshes |
|---|---|
| `v_havanahouse12` | 70 |
| `v_lhaitnewbt` | 67 |
| `v_lithitsjm` | 59 |
| `v_sjmkhus` | 52 |
| `v_IslandLOD1` | 52 |
| `v_miamiland027a` | 51 |

**Why they are like that.** Vice City builds some models from one material per face — `v_lithitsjm`
carried 165 materials over 1592 triangles before merging. RenderWare gives every material its own
mesh, so the model reaches the D3D pipeline as a mesh count far past anything San Andreas ships, and
the instance it builds for that is what the render then calls through as garbage. That is what the
Little Haiti crash was, and merging identical materials is what fixed it — 165 meshes down to 59.

**Why these six are not fixed.** Their materials genuinely differ, so merging cannot reduce them
further without changing how they look. None of them has crashed since the merge, so the ceiling is
evidently soft rather than hard.

**If a new crash appears in Vice City**, this list is the first place to look. The next step would be
splitting a geometry by texture in the importer, which is a larger change than merging and was not
worth making speculatively.

---

## A map placed far from San Andreas costs frame rate

San Andreas files every static entity into a grid that is a fixed **120 × 120 sectors**, whatever
ground it covers. The plugin can move and stretch that grid — it repoints the scale and centre across
588 sites — but not make it finer, so the further apart the maps are, the larger every cell:

| placement | span | cell | ground per scan |
|---|---|---|---|
| vanilla San Andreas | 6000 | 50 | 1× |
| Vice City east, Liberty City north (shipped) | 11000 | 92 | 3.4× |
| Vice City east, Liberty City west | 15348 | 128 | 6.5× |

Every collision query — the camera cache, the vertical line, the sphere test, the line of sight —
builds a box a few units across, far smaller than one cell, and a query smaller than a cell always
walks the whole cell. The cost is therefore the cell's area.

**Why the grid cannot simply be enlarged.** Every index into it is clamped, and the upper half of
every clamp is a three-byte `cmp reg, imm8` with a sign-extended immediate — at all 136 fine and 13
coarse sites, without exception. The largest value that encodes is **127**, so the grid can never
exceed 128 a side. Widening one to a four-byte immediate needs six bytes and the three it would take
are the branch that reads its result; a trampoline does not help either, because only 103 of the 149
clamps are the contiguous compare-and-assign, the rest split the two or jump over a block with
targets landing inside the sequence. 128 against the 307 a side that 50-unit cells would need is
6.7 per cent, for about 350 patch sites.

**What is shipped instead.** `PackRegions` moves each region against ground already in use, on
whichever side leaves the world squarest, which takes the west-coast case from 128-unit cells to
87.5 — less than half the scan cost. It is **off by default**, because moving a map moves things that
remember where they were: savegame pickups, parked cars, teleport coordinates, screenshots. The log
prints what packing would save whether it is on or off.

**The route to real 50-unit cells**, if anyone wants it: fit the 120 cells to the ground the player is
standing on and re-file every static entity when they cross between maps. No new binary sites at all,
but a Remove-all/change-constants/Add-all pass over the building, dummy, object, ped and vehicle
pools that must be exactly symmetric or it leaves dangling pointers in sector lists.

---

## Imported weapons: what is not perfect

Ten of Vice City's guns exist as weapon types of their own and San Andreas' own weapons are untouched.
Three rough edges remain, all cosmetic:

- **Hit and death reactions are generic.** `Events::ComputeDamageResponse` and `ComputeDamageAnim`
  branch on `type >= 47` into the pseudo-weapon arm, and that bound cannot be raised without taking
  `RAMMEDBYCAR` and the other damage pseudo-weapons with it. The damage itself is correct, because it
  comes from the new weapon's own record.
- **`CDarkel` will not hand a new gun back after a rampage.**
- **A savegame carrying an imported weapon expects the mod installed.** The weapon types live above
  San Andreas' own, so a save made holding one will not resolve it in a vanilla game.

Only one imported weapon can have the black fill around its scope, because that comes from a spare
comparison in `Render2dStuff` and there is exactly one spare. Of the ten imports, exactly one has a
donor that fill is drawn for, so this costs nothing today — but a second scoped import would have
nowhere to go.

---

## Other things worth knowing

- **GTA III contributes no weapons.** Its only gun San Andreas lacks is the M16, which *is* the M4,
  and its meshes live in `models/Generic/weapons.dff`, a `MODELFILE` the importer does not read.
- **There is no money pickup to import.** Neither donor game places one in the world from its script;
  cash comes from kills. Street money would have to be invented rather than ported.
- **25 street pickups are deliberately left out** — mostly safehouse weapon stashes at the Vercetti
  Estate and Ocean Beach — because they are created by mission threads rather than an always-running
  one, and only always-running threads give permanent placements.
- **Vice City's stadium interiors are written commented out.** The three arenas float at z 250–325
  over the Hyman Memorial Stadium and the game only ever teleported the player into them from a
  mission. Switching one on because something flew through that airspace would black out the world.
- **The `!` donor-takeover in `weapons.dat`** gives one gun its donor's weapon type outright, for
  behaviour that genuinely cannot be reached from a new type. Nothing shipped uses it, and it costs
  exactly what it says: that San Andreas weapon becomes the import everywhere.
