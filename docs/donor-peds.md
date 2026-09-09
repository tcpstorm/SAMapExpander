# Importing the donor games' pedestrians

How Vice City's and GTA III's own people are brought into San Andreas as San Andreas peds, what was
measured to make that work, and what is still only true on paper.

Everything here was measured against San Andreas 1.0 US (HOODLUM), a retail Vice City and a retail
GTA III, by `tools/convert_peds.py --check` and the survey scripts behind it. Where a number appears
it came out of those files.

---

## The short version

**It converts, in bulk, with no per-ped handwork.** 105 Vice City peds and 78 GTA III peds - every
one either game declares and ships a model for - come through `tools/convert_peds.py`, and all 183
are then re-read from the written archive by a separate parser in `tools/check_peds.py` and match
San Andreas' own peds in frame count, frame names, parenting, bone ids, hierarchy node order,
keyframe size, skin layout and bind-pose consistency. Nothing is skipped and nothing is hand-fixed.

(105 rather than 107: Vice City's archive holds `pga` and `pgb` twice, two of the five duplicate
`.dff` names `docs/asset-namespacing.md` records in its directory, so 107 entries are 105 distinct
model names.)

The hand-written part of the job is **one table of sixteen lines**, mapping GTA III's limb names onto
San Andreas' bone ids. Vice City needs no table at all: its bone ids already *are* San Andreas'.

That is the whole of it, and the reason is worth stating plainly, because it is not what the prior
art would lead you to expect:

> Vice City ped models are already skinned, and already numbered for San Andreas' skeleton. Both
> games export the same 3ds Max Biped. The conversion is a re-encoding, not a re-rig.

---

## The skeleton comparison

| | GTA III | Vice City | San Andreas |
|---|---|---|---|
| ped `.dff` in `gta3.img` | 78 | 107 | 265 |
| RenderWare | 3.2.0.0 | 3.3.0.0 (104), 3.4.0.3 (3) | 3.6.0.3 |
| frames | 17 | 25 | 33 |
| geometries | **16** | 1 | 1 |
| skin plugin (0x116) | **none** | yes | yes |
| animation hierarchy (0x11E) | **none** | 24 nodes | 32 nodes |
| distinct skeletons across the set | 1 | 2 (the two toe ids vary) | 1 |
| max weights per vertex | n/a | 4 (88 models), 3 (19) | 4 (215), 3 (48), 2 (2) |

### Bone ids

San Andreas' 32 nodes, in the order its hierarchy lists them - which is **not** the frame order, and
is the order a skin's per-vertex bone index indexes into:

```
0 Root      1 Pelvis    2 Spine     3 Spine1    4 Neck      5 Head
8 Jaw       6 L Brow    7 R Brow
31 Bip01 L Clavicle  32 L UpperArm  33 L ForeArm  34 L Hand  35 L Finger  36 L Finger01
21 Bip01 R Clavicle  22 R UpperArm  23 R ForeArm  24 R Hand  25 R Finger  26 R Finger01
302 L breast  301 R breast  201 Belly
41 L Thigh  42 L Calf  43 L Foot  44 L Toe0
51 R Thigh  52 R Calf  53 R Foot  54 R Toe0
```

Vice City's 24, in its own order:

```
0 Root  1 Pelvis  2 Spine  3 Spine1  4 Neck  5 Head
31 Bip01 L Clavicle  32 L UpperArm  33 L Forearm  34 L Hand  35 L Finger
21 Bip01 R Clavicle  22 R UpperArm  23 R Forearm  24 R Hand  25 R Finger
41 L Thigh  42 L Calf  43 L Foot  2000 L Toe0
51 R Thigh  52 R Calf  53 R Foot  2001 R Toe0
```

**Twenty-two of the twenty-four ids are identical, with identical names.** The differences are:

- **The toes.** Vice City numbers them 2000 and 2001, San Andreas 44 and 54. One ped, `fireman`,
  numbers them 54 and 55 instead - and 54 is San Andreas' *right* toe, so a map keyed on the id
  would put its left foot on its right leg. The converter keys on the frame **name** and falls back
  to the id, which gets `fireman` right along with everything else.
- **Eight bones San Andreas has and Vice City has not**: 6 and 7 the brows, 8 the jaw, 26 and 36 the
  second finger joints, 201 the belly, 301 and 302 the breasts. All eight are created - San Andreas'
  code and its animations reach for them by id - and nothing is weighted to them.
- **One difference in parenting**: Vice City hangs both thighs off the **Spine**, San Andreas off the
  **Pelvis**. This is absorbed for free, because every local matrix is recomputed as
  `world × inverse(parent world)` against San Andreas' parenting, so a thigh lands in the same place
  either way.
- **Case**: `L Forearm` against `L ForeArm`.

GTA III shares nothing. Its 17 frames are `male01` (the ped's own name), `Swaist`, `Smid`, `storso`,
`Shead`, `supperarml/r`, `slowerarml/r`, `SLhand`/`SRhand`, `supperlegl/r`, `slowerlegl/r`,
`sfootl/r` - and no hierarchy, no bone ids and no weights anywhere. The sixteen-line map in
`tools/ped_rig.py` is the whole correspondence.

### Bind poses, and why they cost nothing

Vice City's ped file is stored in a **T-pose**; San Andreas' in an **A-pose**. That looks like the
expensive difference and it is free, because skinning evaluates

```
v_out = Σ w[i] · v · skinToBone[i] · boneWorld[i]
```

and a GTA animation **replaces** a bone's local rotation rather than adding to it. So `boneWorld` at
run time is whatever San Andreas' animation says, `skinToBone` correctly un-poses the donor vertex
into its bone's space, and the pose the file was stored in never reaches the screen. Nothing has to
be re-posed and no vertex has to move.

What would *not* cancel is a difference in the **bone-local axes** - if the donor's forearm bone were
rolled relative to San Andreas', the mesh would come out twisted. So that was measured rather than
assumed, and it is the measurement the whole approach rests on:

- **Every bone in both games has its local +X pointing at its first child**, to a cosine of 1.000
  across all 13 parent/child chains in both games. Same Biped, same convention.
- Bone by bone, the rotation carrying a Vice City frame onto San Andreas' is **0.0°** at the pelvis,
  spine and spine1; **2.9-4.6°** at the neck, head and legs; **8.7°** at the clavicles; and **~60°**
  at the arms - and that 60° is a swing about the bone's local **+Y**, with under 7° about the bone's
  own +X. A T-pose against an A-pose and nothing else.

### One subtlety that does bite

A ped clump states where its bones are **twice** - in the frame list, and again as the skin's
inverse-bind matrices - and the two need not agree. They differ by one constant matrix `P`, where
`P = skinToBone[i] · frameWorld[i]`, which is the change of basis between the space the vertices are
stored in and the space the frames are posed in. It cancels in the skinning, so it is invisible
until you rebuild one of the two halves.

- 106 of Vice City's 107 ped models have `P` = the identity.
- **San Andreas' own peds do not.** Their `P` is a cyclic permutation of the axes: their frame list
  holds a Y-up A-pose and their mesh is stored X-up.
- Vice City's `fireman` alone is like San Andreas'. `pga` and `pgb` carry the same later version
  stamp and are still written the older way, which is why the skin layout is decided by which one
  accounts for every byte rather than by the version in the header.

So the converter rebuilds the frame list from the donor's **frame list** and the skin from the
donor's **skin**, carrying `P` across rather than flattening it. Deriving the frames from the skin
instead - the first thing tried, and it passed every structural check - quietly put `fireman` alone
in a different basis from the region's other 104 peds. It was caught not by a structural check but
by a measured one: its feet ended up 4 mm below its origin where San Andreas' own peds put theirs
0.746 to 0.953 below.

---

## The route taken, and the one rejected

**Taken: keep the donor's mesh and weights, and re-encode the skeleton around them.**

**Rejected: re-rig the donor mesh onto San Andreas' skeleton** - that is, transplant San Andreas'
bind pose and bone offsets and move the vertices to suit.

The reason is not effort, it is that the two are **arithmetically identical** and the second one
throws information away. Rebuilding the frames in San Andreas' bind pose while transforming the
vertices by `newBind · oldBind⁻¹` produces, after the inverse-bind matrices are recomputed, exactly
the same product `v · skinToBone[i]` as leaving both alone. The rendered result is the same to the
last bit. What differs is that the re-rig also replaces the donor's **bone offsets** with San
Andreas', which discards the donor ped's proportions - and Vice City's peds vary in stature from
1.33 to 1.64, which is most of what makes its crowd look like its crowd rather than San Andreas'.

The brief's fourth item - compute the bind-pose correction arithmetically rather than assuming it is
small - was done, and the answer is that the correction is exactly the identity. That is not an
assumption dodged; it is the T-pose/A-pose difference cancelling, verified against the axis
measurements above.

Scaling was likewise considered and rejected **on measurement**: San Andreas' own 265 peds put the
foot bone anywhere from 0.746 to 0.953 below the model origin, a spread of 0.207, and Vice City's 107
fall inside that band unaided (0.753 to 0.933). GTA III's land inside it too once rotated. Scaling
donor peds to match would only be choosing which San Andreas ped to be the same height as - and it
showed: the fitted scale moved from 1.06 to 0.88 purely by changing the reference model from a man to
a woman. So the conversion applies a rotation and nothing else.

---

## GTA III: building a skin that was never there

A GTA III ped is 16 rigid atomics on 16 frames, drawn by transforming frames. There is no skin
plugin and no hierarchy to convert, so both are built:

1. Each part's vertices go through its own frame's world matrix into one common model space, and its
   normals through the rotation alone.
2. The 16 geometries are welded into one - triangles reindexed, material lists concatenated and then
   folded by `import_city.merge_materials`, bounding sphere recomputed, bin mesh rebuilt as lists.
3. Every vertex is weighted **1.0** to the San Andreas bone its frame stood for, and that bone's
   inverse-bind matrix is the inverse of the same world matrix - so the product is the identity at
   bind and the mesh starts exactly where GTA III drew it.

It therefore deforms exactly as GTA III's own ped did: rigidly, one part per bone. That is the honest
conversion. Smoothing the joints would mean inventing weights the donor never had, and a rigid elbow
is what GTA III looked like.

Two GTA III specifics:

- **Its frames carry baked scale** - up to 0.571 off orthonormal on `worker1`, a limb stretched by
  more than half. A matrix that is not a rotation cannot be inverted by transposing it, which is
  what RenderWare does, so the bone keeps the nearest true rotation and the stretch stays baked into
  the mesh in the bone's own space - which is where GTA III had it.
- **Its model space is Z-up and turned** relative to San Andreas'. The rotation is solved per model
  by an orthogonal Procrustes fit of its joints onto San Andreas', using Horn's quaternion method so
  the answer is always a rotation and never a reflection - the mesh is only ever turned, and its
  winding and normals survive.

### The trap in that fit, and what it cost

Which way round GTA III's `supperlegl` is has to be decided, not assumed - nothing in its files
guarantees the naming is consistent. Two ways of deciding were tried and **both were wrong**:

- **Smaller Procrustes residual.** A ped skeleton is a metre and a half tall, half a metre across and
  barely fifteen centimetres deep, so relabelling left as right is *almost exactly a half turn about
  the vertical*. Over all 78 peds the two readings' residuals differ by at most 1%. Choosing on that
  classified 10 peds as mirrored, which would have built them **facing backwards**.
- **Sign of the cross-covariance determinant**, the textbook Kabsch reflection test. Same cause: the
  smallest singular value is tiny, so the sign is noise. `female01` and `li_wom2` have joints
  agreeing to three decimal places and fall on opposite sides of it.

What settles it is asking the model which way its feet point - a foot's mesh sticks out in front of
its ankle by a good part of the foot's length, and San Andreas' ped space has +Z forward. Measured
that way the answer is unanimous and not close: **all 78 read straight**, every one between +0.05 and
+0.21 forward, with the other reading at exactly the negative. `supperlegl` really is the left leg on
every GTA III ped.

---

## The skin plugin, byte for byte

San Andreas (RenderWare 3.6):

```
u8   numBones, numUsedBones, maxWeightsPerVertex, pad
u8   usedBones[numUsedBones]
u8   boneIndex[numVertices][4]
f32  weight[numVertices][4]
f32  inverseBindMatrix[numBones][16]      4x4 row-major, last column (0,0,0,1)
i32  boneLimit, numMeshes, numRLE         (0, 0, 0) on all 265 of its peds
```

Vice City (3.3, and the 3.4 peds written the same way):

```
u32  numBones                             no used-bone list at all
u8   boneIndex[numVertices][4]
f32  weight[numVertices][4]
per bone: u32 filler + f32 matrix[16]     stride 68, not 64
```

The 68-byte stride is not taken on faith. It is what makes the chunk length add up exactly
(`4 + 20·verts + 68·bones`), and it is what makes the inverse-bind matrices come out as the exact
inverses of the frame world matrices, to 0.000000, on every 3.3 ped. The filler is 0 on 802,120 of
the set's 804,616 bones and `0xDEADDEAD` on the other 2,496 - RenderWare's own uninitialised marker.

`tools/convert_peds.py:read_skin` decides which of the three shapes a chunk is by **which one
accounts for every byte**, not by the version in the header, because `pga` and `pgb` are stamped
3.4.0.0 and still written the 3.3 way.

Two things must be generated rather than copied:

- **The used-bone list.** RenderWare's D3D9 skin pipeline uploads only the bones it names. A Vice
  City skin restamped as 3.6 declares none, which means no bone matrices to skin with.
- **`maxWeightsPerVertex`.** San Andreas ships peds with 2, 3 and 4, so 4 is safe; it is recomputed
  from the weights rather than carried over, since Vice City's 3.3 layout does not have the field.

**This is a live bug in the existing importer**, not a hypothetical one: `import_city.convert_extension`
passes an unrecognised plugin through byte for byte with only the header restamped, so a ped reaching
it today would get a 3.3-layout skin wearing a 3.6 label. It is harmless only because `CityLoader`
skips `peds` sections, so no ped has ever reached it.

---

## Names are not load-bearing, and San Andreas proves it

San Andreas' own ped frame names carry leading spaces - `' Pelvis'`, `' R Thigh'`, `' L UpperArm'` -
on 262 of its 265 peds. The other three disagree with those and with each other: `bmymib` and
`wmomib` write `' Spine '` and `' Spine 1'`, and `wmyst` alone calls the hierarchy root `'Normal'`
where the other 264 call it `'Root'`. Three spellings of one skeleton ship in one archive and all
three work, because everything that looks a bone up - the animations in `ped.ifp`, the code that puts
a weapon in a hand - looks it up by the **id**.

The converter writes the majority spelling anyway, character for character, because it costs nothing
and removes the question. (`wmyst` being the odd one out is also why it is not the reference model:
it was, and it made the checker disagree with 264 peds.)

---

## Textures and model info

**TXDs** go through `import_city.convert_txd` unchanged - the same path every map texture takes. 106
Vice City and 78 GTA III ped dictionaries convert.

**Names** follow `docs/asset-namespacing.md`: `v_` and `l_` tags on the model and TXD names. Ped stems
are short - the longest in either donor is 11 characters - so every one keeps a fully readable name
inside the 19-character budget and none needs the hash fallback.

**`peds.ide`** is written per region. A donor line is shorter than San Andreas' and the columns it is
short of have to be invented:

| | GTA III | Vice City | San Andreas |
|---|---|---|---|
| columns | 7 | 10 | 14 |
| | id, model, txd, ped type, stat, anim group, car mask | + anim file, radio 1, radio 2 | + a second flag word, and `PED_TYPE_` and two voices |

- **Ped type** and **stat** carry across where San Andreas declares them (its `pedstats.dat` declares
  43 stat names), and fall back to `CIVMALE` / `STAT_STREET_GUY` where it does not.
- **Animation group** is checked against `data/animgrp.dat`, which declares 21 - not against the 16
  that `peds.ide` happens to use, because the five it adds (`shuffle`, `oldfatman`, `drunkman`,
  `blindman`, `fatwoman`) are exactly the ones a donor asks for. A group San Andreas has no
  animations for leaves the ped standing in its bind pose.
- **The car mask is renumbered.** GTA III has no `NORMAL_CAR` bit, so everything below its `BIG_CAR`
  sits one place lower than in the other two games, and it has a `SPECIAL_CAR` bit that neither
  successor has. Copied across unchanged, its `POOR_FAMILY` becomes San Andreas' `NORMAL` and its
  `SPECIAL` becomes `WORKER`.
- **Animation file** is forced to `null`. A donor's would name an `.ifp` block San Andreas does not
  have.
- **Voice** is `VOICE_GEN_MALE01` / `VOICE_GEN_FEM01` by ped type. The donors' voices live in their
  own audio archives, which nothing here converts.

---

## What was built

| | |
|---|---|
| `tools/ped_rig.py` | San Andreas' skeleton as measured data, the GTA III bone table, the matrix and Procrustes arithmetic, and the rules for placing bones San Andreas has that a donor has not |
| `tools/convert_peds.py` | the converter and its writer, plus `verify()` - everything about a converted ped that can be settled offline |
| `tools/check_peds.py` | an independent read-back: re-parses the written archive with a separate reader and holds every model against a real San Andreas ped |

```
python tools/convert_peds.py <vice city dir> <sa dir> --region=vicecity    --out=DIR
python tools/convert_peds.py <gta3 dir>      <sa dir> --region=libertycity --out=DIR
python tools/check_peds.py   DIR <sa dir>
```

It runs in the same throwaway `python:3-slim` container the rest of `tools/` uses and needs nothing
installed.

### What it produced

| | Vice City | GTA III |
|---|---|---|
| peds converted | **105 of 105** | **78 of 78** |
| skipped | 0 | 0 |
| texture dictionaries | 106 | 78 |
| `peds.img` | 9.8 MB | 10.4 MB |
| bones mapped / derived per ped | 24 / 8 | 16 / 15 |
| independent read-back | **105 clean** | **78 clean** |

Each region gets `peds.img`, `peds.ide` and `peds.txt` - the last naming every model, what it became,
its vertex and triangle counts, how many bones were mapped and how many derived, its bounding box,
its stature as a percentage of San Andreas', and for GTA III the fitted rotation and the foot-forward
margin that chose it.

### What the checks actually check

Per model, from the bytes on disk: the RenderWare library id is San Andreas' on every nested chunk;
the clump declares one atomic and no lights or cameras; 33 frames with San Andreas' names, parenting
and hierarchy ids; the 32-node array in San Andreas' order with keyframe size 36; every frame's
rotation orthonormal; the skin at 64-byte stride with 32 bones, a non-empty used-bone list and 1-4
weights per vertex; every vertex's weights summing to 1; no vertex naming a bone outside the used
list or outside the hierarchy; the skin and the frame list agreeing on one constant change of basis
across all 32 bones; every triangle naming a vertex that exists; the mesh 1.2-2.6 m tall; and the
feet between 0.65 and 1.05 below the origin, the band San Andreas' own peds occupy.

---

## What only running the game can settle

Everything above is structural. None of it can tell you how a ped **looks in motion**, and these are
the specific things to watch for:

1. **Whether the animations drive it correctly.** The argument that the bind pose cancels is sound
   and the axis measurements support it, but "the mesh follows the animated bone" is a claim about
   RenderWare's skin pipeline that has not been observed. The failure would be visible immediately:
   limbs bending about the wrong axis, or an arm rolled 90°.
2. **Whether the ~7° roll on the upper arm shows.** Measured, small, and expected to be invisible.
3. **GTA III's rigid joints.** Elbows, shoulders, knees and the neck will show seams and
   interpenetration under San Andreas' animations, which bend further than GTA III's did. This is
   certain to be visible to some degree; the question is only how much. It is also the one place
   where a later improvement is obvious - blend the weights across a joint band - and that would be a
   change to the converter, not handwork per ped.
4. **The eight and fifteen derived bones.** Nothing is weighted to them, so they should be inert. If
   San Andreas' facial animation or its IK reaches for a jaw or a toe and finds it in an odd place,
   that shows up here.
5. **Ground contact.** The feet are inside San Andreas' own measured band, but whether a ped stands,
   floats or sinks depends on code that has not been read.
6. **Weapons in hands.** Bone 24 (`R Hand`) is present with the right id and the donor's own offsets,
   so a weapon should attach - but a hand that is a different size from San Andreas' may hold it
   loosely.
7. **Textures.** The TXDs go through the map path, which is well tested, but ped textures have not
   been looked at on a ped.

---

## What is still to do, and it is not in `tools/`

Two things stand between these archives and people walking around an imported city, and both are on
the loader side.

**1. `CityLoader` skips `peds` sections.** `src/region/CityLoader.cpp` sends IDE lines to
`CFileLoader::LoadObjectTypes` (0x5B8400) after rewriting them, and explicitly drops `peds`, `cars`
and `hier` (the `IdeSection::Skip` branch). A ped line has to be let through and given a model id
from the same allocator `objs` lines use. San Andreas parses one in `CFileLoader::LoadPedObject`
(0x5B7420), which builds a `CPedModelInfo` - a different and larger structure than the
`CBaseModelInfo` an `objs` line makes, so `ModelLimits`' slot accounting has to know the difference.
`docs/asset-namespacing.md` already notes that the `peds` columns are never renamed although their
assets are packed; that is the same gap seen from the other side.

**2. `pedgrp.dat`'s pairing has to be pointed at the imports.** This is the easy half, and it is what
the whole `DONOR/SAMODEL` design was for. Each line already reads

```
group 3  DOWNTOWN  WMYST/WMYST  BFYST/BFYST  ...
```

and the right-hand side becomes `v_wmyst`, `v_bfyst` and so on. `tools/import_city.py`'s
`build_ped_map` is where that changes: instead of `map_vc_ped` choosing a San Andreas stand-in, the
pair's right-hand side is the imported name whenever `convert_peds` produced one, and falls back to
the stand-in when it did not. Nothing in `src/peds/` changes at all - which was the point of putting
the indirection in a data file.

Until (1) is done, `peds.img` and `peds.ide` are written and correct but nothing loads them.

---

## Scale of the job, answered

**A converter that runs over all of them.** 183 peds, zero exceptions, no per-ped handwork, one
sixteen-line table written by hand for GTA III and none for Vice City. It runs in a few seconds per
region.

The residual work is not per-ped either: it is the two loader-side items above, and - if GTA III's
rigid joints look bad enough to matter - one change to how the converter assigns weights near a
joint, applied to all 78 at once.
