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
