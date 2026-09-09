"""The skeleton half of importing a donor game's pedestrians: what a ped's rig is, and how a
donor's maps onto San Andreas'.

Everything in this file was measured against the retail files - San Andreas 1.0 US (HOODLUM), a
retail Vice City and a retail GTA III - and the measurements are re-run by
`tools/convert_peds.py --check` and `tools/check_peds.py`.  Nothing here is taken from documentation.
The numbers that matter:

  | | GTA III | Vice City | San Andreas |
  |---|---|---|---|
  | ped .dff in the archive | 78 | 107 | 265 |
  | RenderWare | 3.2.0.0 | 3.3.0.0 (104), 3.4.0.3 (3) | 3.6.0.3 |
  | frames | 17 | 25 | 33 |
  | geometries | **16** | 1 | 1 |
  | skinned (skin plugin 0x116) | **no** | yes | yes |
  | animation hierarchy (0x11E) | **no** | 24 nodes | 32 nodes |
  | distinct skeletons across the set | 1 | 2, differing only in the two toe ids | 1 |

(Vice City's 107 archive entries are 105 distinct model names: it holds `pga` and `pgb` twice, which
`docs/asset-namespacing.md` already records as two of the five duplicate `.dff` names in its
directory.  Only `fireman`, one of the three stamped 3.4.0.3, differs in its skeleton; the other two
carry a later version stamp but are still written the 3.3 way, which is why `convert_peds.read_skin`
decides the layout by which one accounts for every byte rather than by the version.)

So the two donors are at opposite ends of the problem, and neither is a per-model problem:

* **Vice City is already skinned to San Andreas' own bone numbering.**  Its 24 HAnim node ids are
  1 Pelvis, 2 Spine, 3 Spine1, 4 Neck, 5 Head, 21/31 clavicles, 22-25 and 32-35 the arms, 41-43 and
  51-53 the legs - the same ids San Andreas uses, in the same order, because both are exports of
  the same 3ds Max Biped.  San Andreas adds eight: 6 and 7 the brows, 8 the jaw, 26 and 36 the
  second finger joints, 201 the belly, 301 and 302 the breasts; and it numbers the toes 44 and 54
  where Vice City writes 2000 and 2001 (and, on `fireman` alone, 54 and 55 - which is why the map
  below is keyed on the frame *name* and only falls back to the id).

* **GTA III is not skinned at all.**  Its ped is 16 separate atomics on 16 frames - one per limb -
  drawn rigidly, with no skin plugin and no animation hierarchy anywhere in the clump.  Converting
  one means *building* a skin: every part's vertices baked into one geometry and weighted 1.0 to
  the bone its frame stood for.

The bind poses differ too - Vice City's file is stored in a T-pose, San Andreas' in an A-pose - and
that turns out not to matter at all, which is the finding that makes this cheap.  Skinning evaluates
`v * skinToBone[i] * boneWorld[i]`, and an animation *replaces* a bone's local rotation rather than
adding to it, so the bind pose cancels: whatever pose the donor was stored in, the animated result
is the pose San Andreas asked for.  What would not cancel is a difference in the bone-local axes,
and there is none - measured over all 13 parent/child chains in both games, every bone's local +X
points at its first child to within 0.001 of a cosine.  The frame-to-frame rotation difference
between a Vice City ped and a San Andreas one is 0.0 degrees at the pelvis, spine and spine1, under
5 degrees at the neck, head and legs, and about 60 degrees at the arms - and that 60 degrees is a
swing about the bone's local +Y (the component about the bone's own +X, a roll, is under 7 degrees).
That is a T-pose against an A-pose and nothing else.

GTA III needs one thing Vice City does not: its ped model space is Z-up and turned relative to San
Andreas', so its skeleton is brought over by a rotation.  That rotation is not guessed either -
`fit_basis` solves it, per model, as an orthogonal Procrustes fit of the donor's joints onto San
Andreas', by Horn's quaternion method so that the answer is always a rotation and never a
reflection.  The mesh is therefore only ever turned, and its triangle winding and its normals
survive untouched.

There is a trap in that fit worth writing down, because the obvious way round it is wrong and the
files do not complain.  A ped skeleton is very nearly symmetric front to back - a metre and a half
tall, half a metre across and barely fifteen centimetres deep - so **relabelling left as right is
almost exactly a half turn about the vertical axis**.  Fitting both readings of GTA III's
`supperlegl` and taking the smaller residual therefore decides nothing: over all 78 of its peds the
two residuals differ by at most 1%, and the reading that loses a coin toss produces a ped built
facing backwards.  Deciding on the determinant of the cross-covariance instead is no better, for the
same reason - the smallest singular value is tiny, so its sign is noise, and `female01` and
`li_wom2` have joints agreeing to three decimals and fall on opposite sides of it.

What settles it is asking the model which way its feet point: a foot's mesh sticks out in front of
its ankle by a good part of the foot's length, and San Andreas' ped model space has +Z forward.
Measured that way the answer is unambiguous and unanimous - all 78 read straight, every one of them
between +0.05 and +0.21 forward, with the other reading at exactly the negative of that, which is
the half turn showing itself.  `supperlegl` really is the left leg on every GTA III ped.
"""

import math
import struct

# ---------------------------------------------------------------- San Andreas' ped skeleton
#
# Read out of San Andreas' own bfori.dff and checked against all 265 ped .dff files in gta3.img:
# every one of them has exactly this parenting, these bone ids and this node order.  There is no
# second shape to handle.
#
# (bone id, frame name, parent bone id).  Bone 0 is "Root", the frame the hierarchy hangs on; its
# parent is frame 0, which is unnamed and carries no extension of any kind.
#
# The names are San Andreas' own, character for character, **leading spaces included** - ' Pelvis',
# ' R Thigh', ' L UpperArm' - because that is what 262 of its 265 peds write.  The spaces are not a
# transcription slip.
#
# They are also, demonstrably, not load-bearing, and it is worth having measured that rather than
# worrying about it: San Andreas does not agree with itself.  `bmymib` and `wmomib` write ' Spine '
# and ' Spine 1' where every other ped writes ' Spine' and ' Spine1', and `wmyst` alone calls the
# hierarchy root 'Normal' where the other 264 call it 'Root'.  Three spellings of the same skeleton
# ship in the same archive and all three work, because everything that looks a bone up - the
# animations in ped.ifp, the code that puts a weapon in a hand - looks it up by the id.  Matching the
# majority spelling anyway costs nothing and removes the question.
SA_BONES = [
    (0,   "Root",             None),
    (1,   " Pelvis",           0),
    (51,  " R Thigh",          1),
    (41,  " L Thigh",          1),
    (2,   " Spine",            1),
    (201, "Belly",             2),
    (3,   " Spine1",           2),
    (301, "R breast",          3),
    (302, "L breast",          3),
    (4,   " Neck",             3),
    (21,  "Bip01 R Clavicle",  4),
    (31,  "Bip01 L Clavicle",  4),
    (5,   " Head",             4),
    (7,   "R Brow",            5),
    (6,   "L Brow",            5),
    (8,   "Jaw",               5),
    (32,  " L UpperArm",      31),
    (33,  " L ForeArm",       32),
    (34,  " L Hand",          33),
    (35,  " L Finger",        34),
    (36,  "L Finger01",       35),
    (22,  " R UpperArm",      21),
    (23,  " R ForeArm",       22),
    (24,  " R Hand",          23),
    (25,  " R Finger",        24),
    (26,  "R Finger01",       25),
    (42,  " L Calf",          41),
    (43,  " L Foot",          42),
    (44,  " L Toe0",          43),
    (52,  " R Calf",          51),
    (53,  " R Foot",          52),
    (54,  " R Toe0",          53),
]

# The order the animation hierarchy lists its nodes in, which is *not* the frame order.  A skin's
# per-vertex bone index is an index into this array, so this is the table the remapping is done
# against.  Flags are RenderWare's hierarchy-walk markers: 2 pushes the parent matrix before
# descending, 1 pops it afterwards.
SA_NODES = [
    (0, 0x0), (1, 0x0), (2, 0x2), (3, 0x2), (4, 0x2), (5, 0x2), (8, 0x3), (6, 0x3), (7, 0x1),
    (31, 0x2), (32, 0x0), (33, 0x0), (34, 0x0), (35, 0x0), (36, 0x1),
    (21, 0x0), (22, 0x0), (23, 0x0), (24, 0x0), (25, 0x0), (26, 0x1),
    (302, 0x3), (301, 0x1), (201, 0x1),
    (41, 0x2), (42, 0x0), (43, 0x0), (44, 0x1),
    (51, 0x0), (52, 0x0), (53, 0x0), (54, 0x1),
]

SA_NODE_INDEX = {bone: i for i, (bone, _flags) in enumerate(SA_NODES)}
SA_PARENT = {bone: parent for bone, _name, parent in SA_BONES}
SA_NAME = {bone: name for bone, name, _parent in SA_BONES}
# Frame index of each bone in the file we write: frame 0 is the unnamed root, so bone n is frame n+1.
SA_FRAME_INDEX = {bone: i + 1 for i, (bone, _n, _p) in enumerate(SA_BONES)}

# San Andreas puts the atomic on the Pelvis frame in 258 of its 265 peds and on Normal in the other
# seven.  Vice City puts it on the outer root.  We write San Andreas' majority convention.
SA_ATOMIC_FRAME = SA_FRAME_INDEX[1]


def norm_bone_name(name):
    """A frame name reduced to what identifies the bone: lower case, no spaces.

    'L Forearm' (Vice City) and 'L ForeArm' (San Andreas) are the same bone; 'Supperlegl' and
    'supperlegl' are the same GTA III bone on five of its 78 peds.
    """
    return "".join(name.split()).lower()


# Vice City's frame names are San Andreas' frame names once the spaces and the case are taken out,
# so the map is derived rather than written out.  Nothing needs an exception - even the hierarchy
# root agrees, both games calling it "Root" - and this is kept only as the hook for one, should a
# donor ever turn up that needs it.
VC_EXTRA_NAMES = {}

# GTA III shares no names with anything, so its 16 bones are mapped by hand.  This is the only
# hand-written correspondence in the whole conversion, and it is 16 lines long for 78 models.
#
# What GTA III does not have, and what becomes of it: it has no Neck (the head hangs off the torso),
# no clavicles (the arms hang off the torso), no second finger joint, no toes, and no jaw, brows,
# belly or breasts.  Those bones are still created - San Andreas' code reaches for them by id - but
# nothing is weighted to them, and they are placed by `derive_missing` rather than mapped.
III_BONE_MAP = {
    "swaist":     1,    # Pelvis
    "smid":       2,    # Spine
    "storso":     3,    # Spine1
    "shead":      5,    # Head
    "supperarml": 32, "slowerarml": 33, "slhand": 34,
    "supperarmr": 22, "slowerarmr": 23, "srhand": 24,
    "supperlegl": 41, "slowerlegl": 42, "sfootl": 43,
    "supperlegr": 51, "slowerlegr": 52, "sfootr": 53,
}

# The mirrored reading of the same names.  It is kept, and tried on every model, because nothing in
# GTA III's files guarantees the naming is consistent - and it is the reading that has to be ruled
# out, per model, rather than assumed away.  On the retail set it loses every time.
III_BONE_MAP_MIRROR = dict(III_BONE_MAP)
for _a, _b in ((32, 22), (33, 23), (34, 24), (41, 51), (42, 52), (43, 53)):
    for _k, _v in list(III_BONE_MAP.items()):
        if _v == _a:
            III_BONE_MAP_MIRROR[_k] = _b
        elif _v == _b:
            III_BONE_MAP_MIRROR[_k] = _a


# ---------------------------------------------------------------- 3x4 affine matrices
#
# RenderWare here is row-vector-major: a point is a row and `world = local * parent`.  That is not
# an assumption - it is what makes a Vice City ped's inverse-bind matrices come out as the exact
# inverses of its frame world matrices, to 0.000000, which they do.

IDENTITY = ((1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))


def mat_mul(a, b):
    """a then b."""
    ar, ap = a
    br, bp = b
    rot = tuple(sum(ar[i * 3 + k] * br[k * 3 + j] for k in range(3))
                for i in range(3) for j in range(3))
    pos = tuple(sum(ap[k] * br[k * 3 + j] for k in range(3)) + bp[j] for j in range(3))
    return rot, pos


def mat_inv(m):
    """The inverse of a rotation-and-translation matrix, using the transpose for the rotation.

    Every matrix this handles is orthonormal - `mat_orthonormality` says by how much, and over both
    donors' whole ped sets the worst is under 1e-5 - so the transpose is exact and this never has to
    invert a general 3x3.
    """
    rot, pos = m
    inv_rot = (rot[0], rot[3], rot[6], rot[1], rot[4], rot[7], rot[2], rot[5], rot[8])
    inv_pos = tuple(-sum(pos[k] * inv_rot[k * 3 + j] for k in range(3)) for j in range(3))
    return inv_rot, inv_pos


def mat_apply(m, v):
    rot, pos = m
    return tuple(sum(v[k] * rot[k * 3 + j] for k in range(3)) + pos[j] for j in range(3))


def mat_apply_dir(m, v):
    """A direction - a normal - which takes the rotation but not the translation."""
    rot, _pos = m
    return tuple(sum(v[k] * rot[k * 3 + j] for k in range(3)) for j in range(3))


def mat_orthonormality(m):
    """How far a matrix's rotation is from orthonormal, as a max absolute error."""
    rot, _pos = m
    worst = 0.0
    for i in range(3):
        for j in range(3):
            dot = sum(rot[i * 3 + k] * rot[j * 3 + k] for k in range(3))
            worst = max(worst, abs(dot - (1.0 if i == j else 0.0)))
    return worst


def mat_orthonormalise(m):
    """The same frame with its rotation replaced by the nearest true rotation.

    GTA III bakes scale into its ped frames - up to 0.571 off orthonormal on `worker1`, which is a
    limb stretched by more than half - and a frame matrix that is not a rotation cannot be
    inverted by transposing it, which is what both this file and RenderWare do.

    Taking the rotation out of the frame does not lose the stretch, because the parts' vertices are
    put through the *full* matrix before they are welded: the stretch ends up baked into the mesh in
    the bone's own space, which is where GTA III had it in the first place.  What it buys is a
    skeleton San Andreas can invert, animate and blend without any of its matrix-type flags being a
    lie.

    Finding the nearest rotation to a matrix is the same problem `fit_basis` solves, with the matrix
    itself standing in for the cross-covariance, so the same routine answers it.
    """
    rot, pos = m
    square = [[rot[i * 3 + j] for j in range(3)] for i in range(3)]
    return _best_rotation(square), pos


def mat_det(rot):
    return (rot[0] * (rot[4] * rot[8] - rot[5] * rot[7])
            - rot[1] * (rot[3] * rot[8] - rot[5] * rot[6])
            + rot[2] * (rot[3] * rot[7] - rot[4] * rot[6]))


def pack_frame(m, parent, flags=0):
    rot, pos = m
    return struct.pack("<9f3fiI", *rot, *pos, parent, flags)


def pack_skin_matrix(m):
    """A skin's inverse-bind matrix: 4x4, row-major, with the last column (0,0,0,1)."""
    rot, pos = m
    return struct.pack("<16f",
                       rot[0], rot[1], rot[2], 0.0,
                       rot[3], rot[4], rot[5], 0.0,
                       rot[6], rot[7], rot[8], 0.0,
                       pos[0], pos[1], pos[2], 1.0)


# ---------------------------------------------------------------- fitting a donor into SA's basis

def _best_rotation(h):
    """The **proper** rotation R maximising <R, H>, by Horn's quaternion method.

    Horn 1987, "Closed-form solution of absolute orientation using unit quaternions": build the
    symmetric 4x4 below out of the cross-covariance and take its largest eigenvector; the quaternion
    that comes out is always a rotation, never a reflection, so the reflection case never has to be
    detected and cannot be got wrong.

    That matters here, and the obvious alternatives do not work.  A polar decomposition returns
    `U V^T`, whose determinant tells you whether the correspondence is mirrored - but on a humanoid
    skeleton, which is very nearly symmetric under swapping left for right, the smallest singular
    value is small and that determinant is a coin flip: GTA III's `female01` and `li_wom2` have
    joints agreeing to three decimal places and came out on opposite sides of it.  Forcing the sign
    afterwards by negating the matrix is worse still, because in three dimensions negating flips the
    determinant by turning the answer 180 degrees away from the right one.

    With this, both readings of a mirrored skeleton get an honest proper-rotation fit and the
    **residual** decides between them - which is a difference of a factor of three or more on real
    models, where the determinant's was noise.

    Eigenvector by power iteration on N shifted to be positive definite, which needs no library and
    converges in a few dozen steps on a matrix this small.
    """
    sxx, sxy, sxz = h[0]
    syx, syy, syz = h[1]
    szx, szy, szz = h[2]
    n = [
        [sxx + syy + szz, syz - szy, szx - sxz, sxy - syx],
        [syz - szy, sxx - syy - szz, sxy + syx, szx + sxz],
        [szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy],
        [sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz],
    ]
    shift = 1.0 + sum(abs(x) for row in n for x in row)
    for i in range(4):
        n[i][i] += shift
    q = [1.0, 0.3, -0.2, 0.15]
    for _ in range(512):
        nxt = [sum(n[i][j] * q[j] for j in range(4)) for i in range(4)]
        length = math.sqrt(sum(x * x for x in nxt))
        if length < 1e-30:
            break
        nxt = [x / length for x in nxt]
        if max(abs(a - b) for a, b in zip(nxt, q)) < 1e-14:
            q = nxt
            break
        q = nxt
    w, x, y, z = q
    # Horn's quaternion carries the donor points onto San Andreas' as column vectors; everything
    # here is row-vector, so the transpose of his matrix is what goes back.
    col = [
        [w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (y * x + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)],
        [2 * (z * x - w * y), 2 * (z * y + w * x), w * w - x * x - y * y + z * z],
    ]
    return tuple(col[j][i] for i in range(3) for j in range(3))


def _transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def _inv3(m):
    det = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
           - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
           + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    if abs(det) < 1e-18:
        return None
    cof = [[(m[(i + 1) % 3][(j + 1) % 3] * m[(i + 2) % 3][(j + 2) % 3]
             - m[(i + 1) % 3][(j + 2) % 3] * m[(i + 2) % 3][(j + 1) % 3]) for j in range(3)]
           for i in range(3)]
    return [[cof[j][i] / det for j in range(3)] for i in range(3)]


def fit_basis(donor_pos, sa_pos):
    """The rotation, scale and offset that carry a donor skeleton into San Andreas' model space.

    `donor_pos` and `sa_pos` are {bone id: (x, y, z)} of the joints the two have in common.  Returns
    (rotation, scale, centroids, rms, count) where a point goes across as
    `(p - donor_centroid) * rotation * scale + sa_centroid`.

    The rotation is always proper - see `_best_rotation` - so the mesh is only ever turned, never
    reflected, and its triangle winding and its normals stay as the donor wrote them.  The **rms**
    is therefore the thing that decides between two candidate bone mappings: a mapping that has the
    model's left and right the wrong way round cannot be fixed by a rotation, so its best rotation
    leaves the arms and legs crossed over and the residual says so plainly.
    """
    shared = sorted(set(donor_pos) & set(sa_pos))
    n = len(shared)
    if n < 4:
        raise ValueError("only %d joint(s) in common; cannot fit a basis" % n)
    dc = tuple(sum(donor_pos[b][i] for b in shared) / n for i in range(3))
    sc = tuple(sum(sa_pos[b][i] for b in shared) / n for i in range(3))
    a = [[donor_pos[b][i] - dc[i] for i in range(3)] for b in shared]
    c = [[sa_pos[b][i] - sc[i] for i in range(3)] for b in shared]
    h = [[sum(a[k][i] * c[k][j] for k in range(n)) for j in range(3)] for i in range(3)]
    rot = _best_rotation(h)
    var = sum(x * x for row in a for x in row)
    num = sum(a[k][i] * rot[i * 3 + j] * c[k][j]
              for k in range(n) for i in range(3) for j in range(3))
    scale = num / var if var > 1e-12 else 1.0
    err = 0.0
    for k in range(n):
        for j in range(3):
            got = scale * sum(a[k][i] * rot[i * 3 + j] for i in range(3))
            err += (got - c[k][j]) ** 2
    return rot, scale, (dc, sc), math.sqrt(err / n), n


def basis_matrix(rot, scale, centroids):
    """`fit_basis`'s result as one matrix, for carrying a *point* across.

    It is a similarity, not a rotation - it carries the fitted scale - so it must never be
    multiplied into a frame's rotation, which has to stay orthonormal or RenderWare's own matrix
    optimisation flags become lies.  `place_frame` is what keeps those two apart.
    """
    dc, sc = centroids
    scaled = tuple(x * scale for x in rot)
    pos = tuple(sc[j] - sum(dc[i] * scaled[i * 3 + j] for i in range(3)) for j in range(3))
    return scaled, pos


def place_frame(world, rotation, basis):
    """A donor frame carried into San Andreas' model space.

    The rotation takes the fit's rotation alone; the position takes the whole similarity.  A uniform
    scale commutes with a rotation, so this is exactly "rotate the model, then scale it about the
    origin" - the mesh is scaled by the same factor by putting its vertices through `basis`, and
    the two stay in step: a vertex ends up at `scale` times its old position in its bone's space,
    which is a limb that has grown along with the skeleton rather than a joint that has drifted away
    from it.
    """
    rot, pos = world
    turned = tuple(sum(rot[i * 3 + k] * rotation[k * 3 + j] for k in range(3))
                   for i in range(3) for j in range(3))
    return turned, mat_apply(basis, pos)


# ---------------------------------------------------------------- placing the bones SA adds

def derive_missing(world, sa_reference, size_ratio):
    """Fill in the bones San Andreas has that the donor does not, in place.

    `world` is {bone id: matrix} with the donor's bones already in it.  Two rules, and neither needs
    a decision per model:

    * A bone with a mapped descendant is an **intermediate** - GTA III's missing Neck and clavicles
      are the whole of this case.  It is placed at the position of its nearest mapped descendant,
      carrying its nearest mapped ancestor's rotation, which makes it a zero-length bone sitting on
      the joint it shares.  Nothing is weighted to it, and its descendants' own world positions are
      unchanged, because every local matrix is derived from world positions afterwards - so a bone
      placed here can only move itself.

    * A bone with no mapped descendant is a **leaf** - the jaw, the brows, the belly, the breasts,
      the second finger joints, and GTA III's toes.  It takes San Andreas' own local matrix for that
      bone, its translation scaled by the donor's stature, so a short ped's jaw stays inside its
      head.  Nothing is weighted to these either; they exist because San Andreas' code reaches for
      them by id.
    """
    children = {}
    for bone, _name, parent in SA_BONES:
        if parent is not None:
            children.setdefault(parent, []).append(bone)

    def first_mapped_descendant(bone):
        stack = list(children.get(bone, []))
        while stack:
            b = stack.pop(0)
            if b in world:
                return b
            stack.extend(children.get(b, []))
        return None

    def nearest_mapped_ancestor(bone):
        p = SA_PARENT.get(bone)
        while p is not None:
            if p in world:
                return p
            p = SA_PARENT.get(p)
        return None

    derived = {}
    for bone, _name, parent in SA_BONES:
        if bone in world:
            continue
        kid = first_mapped_descendant(bone)
        anc = nearest_mapped_ancestor(bone)
        if kid is not None and anc is not None:
            world[bone] = (world[anc][0], world[kid][1])
            derived[bone] = "intermediate, on %s" % SA_NAME[kid]
        else:
            derived[bone] = "leaf, from San Andreas' own, scaled %.3f" % size_ratio
    # Leaves are resolved in frame order, so a parent is always in place before its child.
    for bone, _name, parent in SA_BONES:
        if bone in world or parent is None:
            continue
        rot, pos = sa_reference[bone]
        local = (rot, tuple(x * size_ratio for x in pos))
        world[bone] = mat_mul(local, world[parent])
    return derived
