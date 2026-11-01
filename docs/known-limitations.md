# Known limitations

What does not work, why, and what it would take. Each entry says how it was established, so nobody
has to rediscover it — several of these were found the expensive way.

Anything here that is "not fixed" is a considered decision, not an oversight. Where a fix exists but
was rejected, the reason is given.

---

## Traffic and pedestrians do not spawn in an imported city

The cities are silent: nothing drives, nobody walks.

**Why.** San Andreas stores path node positions as `int16 × 0.125`, so a node can only describe a
point within **±4095.875**. Vice City sits near SA x 6000 and Liberty City near y 6000; neither fits,
and there is only about 1096 units of headroom either side of San Andreas' own map. The 8×8 grid of
750-unit path areas that `CPathFind::FindRegionForCoors` clamps into is a second obstacle but not the
fatal one — nothing validates a node against its area's box, so that part is survivable.

Two things that are **not** the problem, both checked: population density is live regardless of where
the player is, and there is no map-extent gate anywhere in the spawning code. So paths really are the
whole of it.

**What it would take.** Widening the encoding means about 271 hand-verified instruction sites, none
of which can be validated without running the game. That was judged too risky to attempt blind.

**The way in, if this is ever wanted.** Ped attractors (`GeneratePedsAtAttractors`) bypass the path
graph completely and need no patching, so pedestrians on the pavement are reachable today. Vehicles
are not: a Landstalker driving Ocean Drive needs the encoding work. A converter for both games' path
data exists and is verified offline — totals reconcile exactly, zero orphans — so the data half is
done if anyone takes the site work on.

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

**Frame rate is no longer what stops a third map, though.** Identifiers are. That is a different
problem with its own document — see below.

---

## How many regions fit: three, and the third is tight

The frame-rate cost of another map is solved. What is not is that every model, texture dictionary,
collision file and IPL a region brings needs an **id**, and those ids all come out of **one budget
with a hard ceiling of 32768 entries** — not, as is easy to assume, out of separate pools.

The short version, with San Andreas' own consumption counted out of its `data/` and `models/`:

| limit | San Andreas takes | Vice City + Liberty City take | left | binds at |
|---|---|---|---|---|
| archive slots (raised 8 → 32) | 6 | 2 | 24 | never |
| **texture dictionary ids (5000)** | **3687** | **1242** | **71** | **three regions** |
| model ids (11568 free) | — | 7238 | 4330 | four regions |
| collision ids (255) | 251 | 0 | 4 | — |
| IPL ids (256) | 190 | 0 | 66 | — |

**Texture dictionaries are the wall**, and unlike the others it cannot be widened, because there
are 52 spare entries in the whole id space to widen into.

It is nevertheless the cheapest of these to fix, and the fix needs no patching at all: Vice City
splits 4131 models across **820 dictionaries, about five models each**, where San Andreas shares one
per area across hundreds. Merging by area takes the two shipped cities from 1242 dictionaries to
roughly 130, which clears the wall for five regions as a change to the importer.

[region-limits.md](region-limits.md) is the whole account: how each number was established, what was
raised and how, the arithmetic for five regions, and the two routes to more budget with their costs.
It also records a savegame bug this work found and fixed — the model id limit had been growing the
savegame's Streaming block, which in an unframed save silently shifts every block behind it.

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
