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

**The other half — merging a region's roads into `CPathFind` — cannot be done for Vice City, and
the reason is a second encoding limit nobody had found.**

A vehicle link is addressed by a packed `uint16`: `area = id >> 10`, `index = id & 0x3FF`. Ten bits
of index means **1024 `CCarPathLink` per area, hard**, and San Andreas already uses a good share of
that. Measured against the real files:

| | areas its ground clamps into | SA already uses | free of 1024 each | region needs |
|---|---|---|---|---|
| Vice City | 31, 39, 47, 55, 63 | 2451 | 2669 | **8404 — 3.1× the whole capacity** |
| Liberty City | 57–62 | 2146 | 3998 | 3340 — fits, with 16% spare |

There is no way round it by choosing different areas: everything past x 2250 clamps to column 7 by
construction, and the alternative — redefining the 8×8 grid — would re-file San Andreas' own 64
shipped node files against boundaries they were not authored for. So the merge could give Liberty
City traffic and could not give Vice City any, which is not a feature worth the risk of rebuilding
the game's path arrays.

Other things the merge would have had to get right, recorded because they were expensive to find
and any future attempt needs them:

- **Node order is positional.** `m_anNumNodes = m_anNumVehicleNodes + m_anNumPedNodes`, with vehicle
  nodes at `[0, numVehicleNodes)` and ped nodes after. Appending is not possible; the whole array
  has to be rebuilt and every index remapped — a node's own id at `+0x14`, its first-link index at
  `+0x10`, every `CNodeAddress` in the `+0xA44` array, and every `CCarPathLink+0x04`.
- **`+0x17` is a connected-component id and `DoPathSearch` aborts if two nodes disagree on it.** The
  flood fill that would recompute it is dead code in this build, so it comes only from the file.
- **`+0x1A & 0x0F` is the traffic spawn rate**, rolled against `rand() & 0xF`. A region whose nodes
  carry zero there would get one car in sixteen attempts even if everything else were right.
- **The aux arrays carry a 192-entry dynamic-link reserve** (`+0x300`/`+0xC0` tails), handed out in
  blocks of 12 by `AddDynamicLinkBetween2Nodes_For1Node`, whose free-block scan looks for `0xFFFF`.
- **`LoadPathFindData` bounds-checks nothing** — it mallocs and freads exactly what the header says,
  so a wrong count is heap corruption rather than a rejection.

One useful negative: **borrowing a `CZoneInfo` slot costs nothing for car density.** `GetZoneInfo`
searches only the info-zone array and falls back to zone 0 — the whole-map `SAN_AND` — so a player
out at x 6000 gets the same popcycle row as one standing in the badlands. Creating type-2 info zones
was never the blocker.

So `RegionPaths` is off, and turning it on is refused: with no merge it would enable the scale and
nothing else, changing how the game reads its own paths for nothing in return.

### There is one mod that does lift both limits — and what it would cost

fastman92's limit adjuster ships an extended path format, and **it repoints San Andreas' own code
rather than only widening a file**: verified from its source (`MapLimits.cpp`,
`PatchPaths_GTA_SA_PC_1_0_HOODLUM`), the SA path patch alone is 609 pointer patches, 501 dword, 531
byte, 519 code-mover fixups and 224 rewrites of `movsx word` into `mov dword`, including all 158
`and eax, 3FFh` sites and all 175 `shr reg, 0Ah` sites. A navi link becomes a 32-bit
`{uint16 area, uint16 index}`, `CPathNode` becomes 40/42/44 bytes with int32 coordinates, and a
single area then holds 65535 links. **Vice City's 8404 links would fit in one.** This is how GTA
Underground carries five cities.

Two things make it more expensive than it first looks:

- **The format is per-file but the grid is global.** A vanilla `nodes<N>.dat` is converted in memory
  as it loads, so San Andreas' own 64 files need not be touched — *as long as the grid is left
  alone*. But the grid is `pathsMapSize / 750` per side and defaults to 6000, i.e. the same 8×8 over
  ±3000. Reaching x 6000 needs `Paths map size = 24000`, which makes it 32×32 — and that renumbers
  every area, so **San Andreas' own 64 files would have to be regenerated into the new 1024-slot
  grid**. FLA has a remapper for exactly this and it is compiled out. It also requires
  `[ID LIMITS] Apply ID limit patch = 1`.
- **It is a separate install with required config editing.** `Apply paths limit patch` ships
  commented out; the repository has no LICENCE file and says the source is "for viewing purposes",
  so it cannot be redistributed with this plugin. It is SA 1.0 US HOODLUM only.

**This plugin therefore stands aside from it rather than depending on it.** `src/core/Fla.cpp`
checks the four instructions that state the path encoding — the navi link's area shift and index
mask and the two struct strides — and anything other than San Andreas' own values means the path
system belongs to somebody else. That check is *not* redundant with `PathScale`'s own site
verification: FLA rewrites the 16-bit loads feeding those multiplies rather than the multiplies, and
keeps the 0.125 at `0x858C48` because its extended coordinates are int32 at the same ÷8 scale, so
`PathScale`'s 288 sites still verify clean under it and it would have widened a scale that must be
left alone.

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
ground it covers. This plugin can move and stretch that grid — it repoints the scale and centre
across 604 sites — but not make it finer, so the further the maps are spread, the larger every
cell, and every collision and render query walks a cell's worth of entities.

**What that actually costs, measured.** `tools/sector_cost.py` reads San Andreas' own 45884 `inst`
placements out of `gta3.img` and `data/maps`, bins them into the grid a given span produces, and
reports how many entities sit in the cell a player standing on the map is in. That is the number
the frame time cares about — not the cell's area, which overstates it, because a bigger cell also
picks up more empty ground:

| span | cell | entities in the cell you are in | vs vanilla |
|---|---|---|---|
| 6000 (vanilla San Andreas) | 50.0 | 12.2 | 1.00× |
| 9936 (packed, any number of maps) | 82.8 | 27.4 | 2.24× |
| 10282 (shipped two cities, grid fitted to the buildings) | 85.7 | 28.3 | 2.32× |
| 11000 (shipped two cities, grid fitted to the radar tiles) | 91.7 | 31.7 | 2.59× |
| 13936 (five maps packed against a side) | 116.1 | 46.7 | 3.81× |
| 15348 (two cities, opposite coasts) | 127.9 | 55.3 | 4.52× |
| 19500 (five maps, the old placement) | 162.5 | 83.0 | 6.79× |

**Why the grid cannot be enlarged here.** Every index into it is clamped, and the upper half of
every clamp is a three-byte `cmp reg, imm8` with a sign-extended immediate — at all 136 fine and 13
coarse sites, without exception. The largest value that encodes is **127**, so a grid patched in
place can never exceed 128 a side. Widening one to a four-byte immediate needs six bytes and the
three it would take are the branch that reads its result; a trampoline does not help either,
because only 103 of the 149 clamps are the contiguous compare-and-assign, the rest split the two or
jump over a block with targets landing inside the sequence.

**It can be enlarged, though — by replacing the routines rather than the operands.** fastman92's
limit adjuster does exactly that: it allocates a bigger `CWorld::ms_aSectors`, rewrites the base
displacement of all 74 indexed accesses and the row-stride multiply of all 71, patches the clamps
that can be patched, and for the ones that cannot — these imm8 sites — replaces the whole function
with hand-written assembly that reads the dimension from a variable
(`CWorld::GetIsLineOfSightClear` and `CRenderer`'s sector scan among them). Its `[MAP LIMITS]`
section has *separate* keys for the world size and the sector size, and the count falls out of the
division:

```
World map size        = 24000    ; +/-12000 instead of +/-3000
World sector size     = 50       ; leave this alone - 50 is vanilla's cell
World LOD sector size = 200
```

which is 480 × 480 sectors of exactly the ground a vanilla one covers. That is how **GTA
Underground** carries Vice City, Liberty City, Carcer City, Bullworth and Cottonmouth over a world
of ±24000 without the cell growing, and how **GTA Mixed / SAxVCxLC** adds Liberty City and Vice
City beside a San Andreas that stays fully playable. The trap is that fastman92's own example ini
raises the sector size along with the map size, which is precisely what costs the frame rate.

This plugin now **stands aside when something else owns the grid**: the fine-scale conversions all
read one pooled constant in a stock executable, so a set of them that has moved together has moved
for a reason. It reads the cell size that mod chose, says so, and writes nothing. When it does own
the grid and the cell has grown, the log prints the `[MAP LIMITS]` values that would give 50-unit
cells over the same world.

(For the record, **GTA United** is not evidence either way: it *replaces* the San Andreas map
rather than adding beside it, and says so — "the map of GTA San Andreas gets replaced". Its own
history gives the reason, which is this problem: it started out meaning to add Vice City next to
the default map, hit "parts of the map are not rendered when they are mapped outside of a fixed
boundary", and gave up on it.)

**What is shipped here instead.**

- **`FitSectorsToContent`** (on by default) fits the grid to where a region's `inst` lines actually
  stand rather than to the radar tile grid its ini declares. Vice City declares 4000 × 4000 of
  radar and occupies 3436 × 3681 of it. On the two shipped cities that is 92-unit sectors down to
  86, for no map movement at all.

- **`PackRegions`** (off by default) puts each region in a *corner* of the ground already in use
  and keeps whichever placement leaves the smallest square, trying every ordering while that is
  cheap. Corners rather than sides is the whole of it: San Andreas is 6000 square and Vice City is
  3436 wide, so the moment Vice City goes east the world is 9936 a side and an entire 3436 × 9936
  column and 6000 × 3436 strip inside that square are empty. Liberty City, Carcer City, Bullworth
  and a Warriors level all fit in there **without the square growing at all** — five maps cost what
  one costs. `tools/pack_test.cpp` checks that offline. It stays off by default because moving a
  map moves things that remember where they were: savegame pickups, parked cars, teleport
  coordinates, screenshots.

  There is no room *inside* San Andreas, before anyone asks: over its own 45884 placements the
  largest square of ±3000 containing no static entity at all is **400 units** on a side. 46 per
  cent of its 14400 cells are empty but they are the scattered fringes of the coast, not a bay you
  could hide a city in.

- **`MapWindow`** (off by default, experimental) is the route to genuinely vanilla-sized cells with
  no new instruction sites: fit the 120 cells to the one map the player is standing on and re-file
  the world when they cross to another. The removal has to be exactly symmetric or it leaves a
  pointer in a sector list that outlives the entity, so it does not guess — the set of filed
  entities is read out of the sector lists themselves and removed with the game's own
  `CEntity::Remove` while the old constants are still in place, and afterwards every one of the
  14400 sectors, 900 LOD lists and 256 repeat sectors must be empty. If one is not, it says so and
  freezes the window rather than risking another crossing. **Whether that check ever fires is the
  one thing here that has not been run**, which is why it is off.

  Two things limit it. A crossing costs a frame — forty thousand entities out and back in. And
  there is one scale and one centre for the whole grid rather than one per axis, so the window is
  the square that holds *both* of a map's projections: Vice City is 3436 × 3681 but sits at
  x 3846..7282, y −914..2767, and the range holding both is 8196. A map out on the diagonal from
  San Andreas gets a window its own size; one out along an axis does not. `PackRegions` therefore
  breaks its ties on that — among placements that leave the same square, the one whose own window
  is narrowest — which costs the square nothing and takes the worst of five maps from a 10317-unit
  window to 7617, i.e. from 86-unit sectors to 63. Splitting the 268 fine conversions into the ones
  that convert an x and the ones that convert a y would end the constraint outright and give every
  map a window its own size on each axis, but telling them apart needs dataflow analysis of each
  site, and a site classified the wrong way round files its entities in transposed cells.

**Where that leaves the numbers**, measured against San Andreas' own placements:

| | span | cell | vs vanilla |
|---|---|---|---|
| two cities as the ini writes them, before this work | 11000 | 91.7 | 2.59× |
| the same, grid fitted to the buildings (now the default) | 10432 | 86.9 | 2.38× |
| the same, `PackRegions` | 10236 | 85.3 | 2.31× |
| **five** maps, `PackRegions`, old four-sides placement | 14236 | 118.6 | 3.94× |
| **five** maps, `PackRegions`, corners | 10236 | 85.3 | 2.31× |
| five maps, `MapWindow`, worst map | 7617 | 63.5 | ~1.6× |
| five maps, `MapWindow`, most maps | 6000 | 50.0 | 1.00× |
| a limit adjuster with `World sector size = 50` | any | 50.0 | 1.00× |

The thing the user asked for — several maps at a frame rate that does not degrade as more are
added — is the fourth row against the fifth: adding a third, fourth and fifth map now costs
nothing at all, where before each one widened the world.

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

## Dying or being arrested in an imported city wakes you in San Andreas

Fall through the map and you now come back on the region's own ground — that is `FallRecovery`, and
it is a different mechanism entirely (`CWorld::RemoveFallenPeds`, which relocates against the vehicle
path graph). But **die** or **get arrested** in Vice City and you still wake in All Saints or in a
Los Santos cell, thousands of units away.

**Why.** Death and arrest go through `CGameLogic::Update` (0x442AD0), which calls
`CRestart::FindClosestHospitalRestartPoint` (0x460850) at +0x4A0 or
`FindClosestPoliceRestartPoint` (0x460A50) at +0x84B and +0xAF6, and hands the answer to
`CGameLogic::RestorePlayerStuffDuringResurrection` (0x442060). Every restart point the game has is
registered from San Andreas' own `main.scm`, so "closest" from Vice City is by definition a San
Andreas one. There is no distance limit and no fallback.

**The data, read off the executable**, since it is most of the work of fixing this:

| | hospital | police |
|---|---|---|
| count (`uint16`) | 0xA4326C | 0xA43268 |
| positions (`CVector[10]`) | 0xA43318 | 0xA43390 |
| headings (`float[10]`) | 0xA432E8 | 0xA43298 |
| town id (`int[10]`) | 0xA432C0 | 0xA43270 |
| the call that fills a slot | `CRestart::AddHospitalRestartPoint` 0x460730 | `AddPoliceRestartPoint` 0x460780 |

Both arrays are **ten entries** — the gaps between the base addresses say so exactly — and San
Andreas' `main.scm` uses **eight** hospital and **seven** police points (counted offline by walking
the script for opcodes `016C` and `016D` and validating the five parameters of each hit). So two
hospital and three police slots are genuinely free, which is enough for one of each in two regions.
The town id is compared against `CStats::GetStatValue(0xB5)` at 0x460973, so a region point wants
town 0 to be eligible from the start of the game.

**Why it is not done.** `CRestart::Save` (0x5D3620) writes those counts and then that many points
into a savegame block, so adding region points changes the shape of a vanilla save block and puts
Vice City coordinates inside it. A save made with the mod, loaded without it, would give a vanilla
San Andreas two hospitals in the middle of the ocean — and `docs/save-compatibility.md` is explicit
that imported content goes above the vanilla window, never inside it. Restart points have no window
above: there are ten slots and the save copies all of the ones in use.

**What it would take.** Mine `ADD_HOSPITAL_RESTART` / `ADD_POLICE_RESTART` (III `0175`/`0176`, VC
`016C`/`016D`) out of the donor `main.scm` the way `tools/import_city.py` already mines
`SET_ZONE_PED_GROUP`, write them into the region folder as readable data, register them at the same
point `RegionPickups` places pickups — and then either hook `CRestart::Save`/`Load` to write only
San Andreas' own points, or hook the two `FindClosest...` functions instead and never put a region
point in the array at all. The second is smaller, keeps the save byte-identical, and is the one to
do: both are self-contained `__cdecl` and read
`(CVector where, CVector* outPosition, float* outHeading)` — the vector by value, twelve bytes of
stack, the two results by pointer — so a region point can be answered without the arrays ever
knowing about it, and the savegame stays byte for byte what vanilla writes.

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
