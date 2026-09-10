"""Bring a donor game's pedestrians into San Andreas as San Andreas peds.

    python tools/convert_peds.py <source game dir> <SA dir> [--region=vicecity] [--out=DIR]
    python tools/convert_peds.py <source game dir> <SA dir> --check      (parse and report, write nothing)

Writes, into `<out>/`:

    peds.img        every converted ped .dff and its .txd, as a San Andreas VER2 archive
    peds.ide        a San Andreas `peds` section for them, one line per model
    peds.txt        what happened to each one, and why

`tools/ped_rig.py` holds the skeleton half of this and the evidence for it.  This file is the
mechanics: reading the donor clump, writing a San Andreas one, and the skin plugin in between.

## What a conversion actually changes

**Vice City.**  Almost nothing.  Its ped is already one skinned geometry against a 24-bone hierarchy
whose bone ids are San Andreas' own, so the model is passed through `import_city.convert_dff` - the
same path every map model takes, which restamps the RenderWare version, converts the materials and
textures and merges identical materials - and then two chunks are replaced:

  * the **frame list**, rebuilt with San Andreas' 33 frames, its names, its parenting and its
    32-node animation hierarchy.  Each bone keeps the donor's own world bind matrix; the local
    matrices are recomputed from those against San Andreas' parenting, which is what absorbs the one
    topological difference between the two rigs (Vice City hangs the thighs off the Spine, San
    Andreas off the Pelvis).
  * the **skin plugin**, rewritten from Vice City's RenderWare 3.3 layout into San Andreas' 3.6 one
    and with every per-vertex bone index remapped through the 24-to-32 node table.

  Not one vertex moves.  The bind pose difference between the two games - Vice City stores a T-pose,
  San Andreas an A-pose - cancels in the skinning arithmetic, for the reason set out in ped_rig.py.

**GTA III.**  Everything.  Its ped has no skin plugin and no hierarchy at all: it is 16 rigid
atomics on 16 frames.  So the skin is *built*.  Every part's vertices and normals are transformed by
its frame's world matrix into one common space, the 16 geometries are merged into one, and each
vertex is weighted 1.0 to the San Andreas bone its frame stood for.  The result deforms exactly as
GTA III's own ped did - rigidly, one part per bone - which is the honest conversion; smoothing the
joints would be inventing weights the donor never had.

The whole model is also rotated into San Andreas' ped model space, because GTA III's is Z-up and
mirrored in X.  That rotation is solved per model by `ped_rig.fit_basis`, not hardcoded.

## What is not done here, and where it goes

The `peds.ide` written here is a San Andreas ped line, but a region's ped models still have to be
*registered* - `CityLoader` skips `peds` sections today - and `pedgrp.dat`'s `DONOR/SAMODEL` pairs
still have to be pointed at the imported name instead of the San Andreas stand-in.  Both are one-line
changes on the loader side and neither is in this file's gift.  See docs/donor-peds.md.
"""

import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import import_city as ic
import ped_rig as rig

RW_SKIN = 0x116
RW_HANIM = 0x11E
RW_FRAMENAME = 0x253F2FE


class PedError(Exception):
    """A ped this cannot convert. The model is skipped and named in peds.txt."""


# ---------------------------------------------------------------- reading a clump

def clump_children(data):
    kind, size, library_id = struct.unpack_from("<III", data, 0)
    if kind != ic.RW_CLUMP:
        raise PedError("not a clump (chunk %#x)" % kind)
    return ic.rw_version(library_id), ic.rw_children(data, 12, 12 + size, "clump")


def read_frames(data, s, e):
    """[{local, parent, name, hanim_id, nodes}] for a frame list, plus its hierarchy header."""
    kids = ic.rw_children(data, s, e, "frame list")
    if not kids or kids[0][0] != ic.RW_STRUCT:
        raise PedError("frame list: no struct")
    _k, _v, ss, se = kids[0]
    count = struct.unpack_from("<i", data, ss)[0]
    if se - ss != 4 + 56 * count:
        raise PedError("frame list struct: %d bytes for %d frame(s)" % (se - ss, count))
    frames, at = [], ss + 4
    for i in range(count):
        rot = struct.unpack_from("<9f", data, at)
        pos = struct.unpack_from("<3f", data, at + 36)
        parent, flags = struct.unpack_from("<iI", data, at + 48)
        frames.append(dict(i=i, local=(rot, pos), parent=parent, flags=flags,
                           name=None, hanim_id=None))
        at += 56
    header, exts = None, [k for k in kids[1:] if k[0] == ic.RW_EXTENSION]
    for i, (_k, _v, cs, ce) in enumerate(exts):
        if i >= count:
            break
        for kind, _v2, c2, e2 in ic.rw_children(data, cs, ce, "frame extension"):
            if kind == RW_FRAMENAME:
                frames[i]["name"] = data[c2:e2].split(b"\0", 1)[0].decode("latin-1")
            elif kind == RW_HANIM:
                version, node_id, num = struct.unpack_from("<IiI", data, c2)
                frames[i]["hanim_id"] = node_id
                if num:
                    flags, key_size = struct.unpack_from("<II", data, c2 + 12)
                    nodes, a = [], c2 + 20
                    for _ in range(num):
                        nodes.append(struct.unpack_from("<Iii", data, a))
                        a += 12
                    header = dict(version=version, flags=flags, key_size=key_size, nodes=nodes)
    for f in frames:
        f["world"] = f["local"] if f["parent"] < 0 else rig.mat_mul(f["local"],
                                                                   frames[f["parent"]]["world"])
    return frames, header


def read_geometry(data, s, e):
    """One geometry, as far as this needs it: counts, the struct's blocks, and its skin."""
    kids = ic.rw_children(data, s, e, "geometry")
    if not kids or kids[0][0] != ic.RW_STRUCT:
        raise PedError("geometry: no struct")
    _k, _v, ss, se = kids[0]
    flags, num_tri, num_vert, num_morph = struct.unpack_from("<IiiI", data, ss)
    out = dict(s=s, e=e, flags=flags, num_tri=num_tri, num_vert=num_vert, num_morph=num_morph,
               struct_s=ss, struct_e=se, skin=None, matlist=None, extension=None, kids=kids)
    for kind, ver, cs, ce in kids[1:]:
        if kind == ic.RW_MATLIST:
            out["matlist"] = (cs, ce)
        elif kind == ic.RW_EXTENSION:
            out["extension"] = (cs, ce)
            for k2, v2, c2, e2 in ic.rw_children(data, cs, ce, "geometry extension"):
                if k2 == RW_SKIN:
                    out["skin"] = (c2, e2, v2)
    return out


def geometry_blocks(data, g, version):
    """The vertex, UV, triangle and morph blocks of a geometry, in a version-independent form.

    San Andreas and Vice City lay the struct out the same way from the prelit colours on; only the
    three surface-property floats a pre-3.4 geometry carries in its head are in the way, and this
    steps over them.  Returns a dict of lists, and asserts the walk accounts for every byte, which
    is what stops a geometry whose layout is not the one assumed from being written back as a guess.
    """
    flags, num_tri, num_vert = g["flags"], g["num_tri"], g["num_vert"]
    at = g["struct_s"] + 16
    if version < ic.RW_34:
        at += 12
    sets = (flags >> 16) & 0xFF
    if sets == 0:
        sets = 1 if flags & ic.GEOM_TEXTURED else (2 if flags & ic.GEOM_TEXTURED2 else 0)
    if flags & ic.GEOM_NATIVE:
        raise PedError("geometry is already instanced for a platform")
    prelit = []
    if flags & ic.GEOM_PRELIT:
        prelit = [data[at + 4 * i:at + 4 * i + 4] for i in range(num_vert)]
        at += 4 * num_vert
    uvs = []
    for _ in range(sets):
        uvs.append(list(struct.unpack_from("<%df" % (2 * num_vert), data, at)))
        at += 8 * num_vert
    tris = [struct.unpack_from("<4H", data, at + 8 * t) for t in range(num_tri)]
    at += 8 * num_tri
    morphs = []
    for _ in range(g["num_morph"]):
        sphere = struct.unpack_from("<4f", data, at)
        at += 16
        has_vertices, has_normals = struct.unpack_from("<ii", data, at)
        at += 8
        verts = normals = None
        if has_vertices:
            verts = [struct.unpack_from("<3f", data, at + 12 * i) for i in range(num_vert)]
            at += 12 * num_vert
        if has_normals:
            normals = [struct.unpack_from("<3f", data, at + 12 * i) for i in range(num_vert)]
            at += 12 * num_vert
        morphs.append(dict(sphere=sphere, verts=verts, normals=normals))
    if at != g["struct_e"]:
        raise PedError("geometry struct: %d bytes, the walk accounts for %d"
                       % (g["struct_e"] - g["struct_s"], at - g["struct_s"]))
    return dict(sets=sets, prelit=prelit, uvs=uvs, tris=tris, morphs=morphs)


# ---------------------------------------------------------------- the skin plugin

def read_skin(data, cs, ce, version, num_vert):
    """A skin plugin in either game's layout.

    San Andreas (RenderWare 3.6) writes, in order: four bytes of `numBones`, `numUsedBones`,
    `maxWeightsPerVertex` and a pad; the used-bone list, one byte each; four bone indices and four
    float weights per vertex; a 4x4 inverse-bind matrix per bone; and twelve trailing bytes of
    `boneLimit`, `numMeshes` and `numRLE`, which are (0, 0, 0) on all 265 of its peds.

    Vice City (3.3 and 3.4) writes `numBones` as a whole word with no used-bone list, and pads every
    bone matrix with **four leading bytes** - so the stride is 68, not 64.  That is not documented
    here on faith: it is what makes the inverse-bind matrices come out as the exact inverses of the
    frame world matrices, to 0.000000, on every Vice City ped, and it is what makes the chunk length
    add up (4 + 20*verts + 68*bones, exactly).  The filler is 0 on 802,120 of the 804,616 bones in
    the set and 0xDEADDEAD on the other 2,496, which is RenderWare's own uninitialised marker.

    This is the layout difference the existing importer would get wrong if a ped ever reached it -
    `convert_extension` passes an unrecognised plugin through byte for byte - which is why the skin
    is rebuilt here rather than restamped.
    """
    num_bones = data[cs]
    if num_bones <= 0:
        raise PedError("skin: %d bones" % num_bones)
    # Which of the three shapes this is, decided by which one accounts for every byte rather than by
    # the version in the header - `pga` and `pgb` are Vice City peds stamped 3.4.0.0 that are still
    # written the 3.3 way, and a version test alone would read them wrong.
    for num_used, max_weights, stride, tail in (
            (data[cs + 1], data[cs + 2], 64, 12),      # San Andreas 3.6
            (0, 0, 68, 0),                             # Vice City 3.3 / 3.4
            (data[cs + 1], data[cs + 2], 64, 0)):      # 3.4 with a used-bone list and no tail
        at = cs + 4 + num_used
        left = ce - at - 20 * num_vert - tail
        if left == stride * num_bones:
            break
    else:
        raise PedError("skin: %d bytes do not fit any known layout for %d bones and %d vertices"
                       % (ce - cs, num_bones, num_vert))
    used = list(data[cs + 4:cs + 4 + num_used])
    indices = [tuple(data[at + 4 * v:at + 4 * v + 4]) for v in range(num_vert)]
    at += 4 * num_vert
    weights = [struct.unpack_from("<4f", data, at + 16 * v) for v in range(num_vert)]
    at += 16 * num_vert
    lead = 4 if stride == 68 else 0
    matrices = []
    for b in range(num_bones):
        m = struct.unpack_from("<16f", data, at + b * stride + lead)
        matrices.append(((m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]),
                         (m[12], m[13], m[14])))
    return dict(num_bones=num_bones, num_used=num_used, max_weights=max_weights, used=used,
                indices=indices, weights=weights, matrices=matrices, stride=stride)


def write_skin(indices, weights, matrices):
    """The skin plugin in San Andreas' 3.6 layout, with the used-bone list computed.

    The used-bone list is not decoration.  RenderWare's D3D9 skin pipeline uploads only the bones it
    names, so a skin that declares none - which is what Vice City's layout amounts to once it is
    restamped as 3.6 - would have no bone matrices to skin with.
    """
    num_bones = len(matrices)
    used, most = set(), 0
    for v in range(len(indices)):
        n = 0
        for c in range(4):
            if weights[v][c] > 0.0:
                used.add(indices[v][c])
                n += 1
        most = max(most, n)
    for b in used:
        if b >= num_bones:
            raise PedError("skin: a vertex names bone %d of %d" % (b, num_bones))
    used = sorted(used)
    body = [struct.pack("<4B", num_bones, len(used), most, 0), bytes(used)]
    body.append(b"".join(bytes(i) for i in indices))
    body.append(b"".join(struct.pack("<4f", *w) for w in weights))
    body.append(b"".join(rig.pack_skin_matrix(m) for m in matrices))
    body.append(struct.pack("<iii", 0, 0, 0))
    return ic.rw_chunk(RW_SKIN, b"".join(body))


# ---------------------------------------------------------------- the frame list

def write_frame_list(world):
    """San Andreas' 33-frame list, with each bone's local matrix derived from `world`.

    Deriving the locals rather than copying the donor's is what makes the two rigs' different
    parenting a non-event: Vice City hangs both thighs off the Spine and San Andreas off the Pelvis,
    and a local recomputed as `world * inverse(parent world)` lands the thigh in exactly the same
    place either way.
    """
    frames = [rig.pack_frame(rig.IDENTITY, -1, 0)]
    for bone, _name, parent in rig.SA_BONES:
        if parent is None:
            local, parent_frame = world[bone], 0
        else:
            local = rig.mat_mul(world[bone], rig.mat_inv(world[parent]))
            parent_frame = rig.SA_FRAME_INDEX[parent]
        frames.append(rig.pack_frame(local, parent_frame, 0))
    body = [ic.rw_chunk(ic.RW_STRUCT, struct.pack("<i", len(frames)) + b"".join(frames))]
    # Frame 0 carries no extension of any kind, which is what all 265 San Andreas peds do.
    body.append(ic.rw_chunk(ic.RW_EXTENSION, b""))
    for bone, name, _parent in rig.SA_BONES:
        plugins = [ic.rw_chunk(RW_FRAMENAME, name.encode("latin-1"))]
        if bone == 0:
            nodes = b"".join(struct.pack("<Iii", b, i, f)
                             for i, (b, f) in enumerate(rig.SA_NODES))
            plugins.append(ic.rw_chunk(RW_HANIM,
                                       struct.pack("<IiIII", 0x100, 0, len(rig.SA_NODES), 0, 36)
                                       + nodes))
        else:
            plugins.append(ic.rw_chunk(RW_HANIM, struct.pack("<IiI", 0x100, bone, 0)))
        body.append(ic.rw_chunk(ic.RW_EXTENSION, b"".join(plugins)))
    return ic.rw_chunk(ic.RW_FRAMELIST, b"".join(body))


# ---------------------------------------------------------------- reference skeleton

def sa_reference_skeleton(sa_dir, model="bfori"):
    """San Andreas' own ped skeleton: {bone: local matrix}, {bone: world bind matrix}, stature.

    Read out of a real San Andreas ped rather than written down, so the bones this has to invent -
    the jaw, the brows, the belly, the breasts, the second finger joints - are placed from the
    game's own numbers.

    Both come from the **frame list**, not from the skin's inverse-bind matrices, and that is worth
    being explicit about because on a San Andreas ped the two are different poses.  Its frame list
    holds an A-pose with +Y up, +X the character's left and +Z forward; the pose its skin was bound
    in is the same A-pose with the axes cyclically permuted, +X up.  Both are self-consistent - the
    basis a mesh is stored in cancels against its own inverse-bind matrices and never reaches the
    engine - but a converted ped has to land in *some* basis, and the frame list's is the one Vice
    City's peds are already in, measured joint by joint.  Fitting GTA III to the same one means both
    donors come out in one space and a bounding box can be read as a person's height.
    """
    blob = None
    for archive in ("models/gta3.img", "models/player.img"):
        path = ic.find_file(sa_dir, archive)
        if not path:
            continue
        blob = read_from_img_v2(path, model + ".dff")
        if blob:
            break
    if blob is None:
        raise PedError("no %s.dff in San Andreas' archives to take a reference skeleton from" % model)
    version, kids = clump_children(blob)
    frames = geom = None
    for kind, _v, cs, ce in kids:
        if kind == ic.RW_FRAMELIST:
            frames, _header = read_frames(blob, cs, ce)
        elif kind == ic.RW_GEOMETRYLIST:
            gk = ic.rw_children(blob, cs, ce, "geometry list")
            geom = read_geometry(blob, gk[1][2], gk[1][3])
    if frames is None or geom is None or geom["skin"] is None:
        raise PedError("San Andreas' %s.dff is not the shape a reference needs" % model)
    by_id = {f["hanim_id"]: f for f in frames if f["hanim_id"] is not None}
    local, world = {}, {}
    for bone, name, parent in rig.SA_BONES:
        f = by_id.get(bone)
        if f is None:
            raise PedError("San Andreas' %s.dff has no bone %d (%s)" % (model, bone, name))
        local[bone] = f["local"]
        world[bone] = f["world"]
    stature = world[5][1][1] - world[43][1][1]    # head to foot, along +Y, this basis' up axis
    return local, world, abs(stature)


def read_from_img_v2(path, name):
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            return None
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
        for at in range(0, 32 * count, 32):
            offset, sectors, _ = struct.unpack_from("<IHH", table, at)
            entry = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
            if entry.lower() == name.lower():
                f.seek(offset * ic.SECTOR)
                return f.read(sectors * ic.SECTOR)
    return None


# ---------------------------------------------------------------- Vice City

def convert_vc_ped(data, sa_local, sa_bind, sa_stature):
    """A Vice City ped: the same mesh, re-hung on San Andreas' skeleton."""
    version, kids = clump_children(data)
    frames = header = None
    geometry = None
    for kind, _v, cs, ce in kids:
        if kind == ic.RW_FRAMELIST:
            frames, header = read_frames(data, cs, ce)
        elif kind == ic.RW_GEOMETRYLIST:
            gk = ic.rw_children(data, cs, ce, "geometry list")
            if len(gk) != 2:
                raise PedError("%d geometries; a Vice City ped has 1" % (len(gk) - 1))
            geometry = read_geometry(data, gk[1][2], gk[1][3])
    if frames is None or header is None:
        raise PedError("no animation hierarchy")
    if geometry is None or geometry["skin"] is None:
        raise PedError("no skinned geometry")

    # Which bone each frame is, by name first and by id only as a fallback.  Name first because
    # Vice City's `fireman` numbers its toes 54 and 55 where every other ped numbers them 2000 and
    # 2001, and 54 is San Andreas' *right* toe - an id-first map would put its left foot on its
    # right leg.
    skin = read_skin(data, geometry["skin"][0], geometry["skin"][1], geometry["skin"][2],
                     geometry["num_vert"])
    if skin["num_bones"] != len(header["nodes"]):
        raise PedError("skin has %d bones, the hierarchy %d"
                       % (skin["num_bones"], len(header["nodes"])))

    # The donor's bone index (into its own node array) -> its bone -> San Andreas' node index.
    # Only the frames the hierarchy actually names are bones; Vice City's outer root frame carries
    # a hierarchy id of -1 and is the clump's root, not a joint.
    by_id = {f["hanim_id"]: f for f in frames if f["hanim_id"] is not None and f["hanim_id"] >= 0}
    donor_node_bone, index_map, unmapped = [], {}, []
    for node, (node_id, _ix, _fl) in enumerate(header["nodes"]):
        f = by_id.get(node_id)
        if f is None:
            raise PedError("the hierarchy names node id %d, which no frame carries" % node_id)
        key = rig.norm_bone_name(f["name"] or "")
        bone = rig.VC_EXTRA_NAMES.get(key)
        if bone is None:
            for b, name, _p in rig.SA_BONES:
                if rig.norm_bone_name(name) == key:
                    bone = b
                    break
        if bone is None and node_id in rig.SA_PARENT:
            bone = node_id
        if bone is None:
            unmapped.append("%s (id %d)" % (f["name"], node_id))
            continue
        donor_node_bone.append(bone)
        index_map[node] = rig.SA_NODE_INDEX[bone]
    if unmapped:
        raise PedError("bones with no San Andreas equivalent: %s" % ", ".join(unmapped))
    if len(set(donor_node_bone)) != len(donor_node_bone):
        raise PedError("two hierarchy nodes map onto the same San Andreas bone")

    # A ped clump says where its bones are twice - once in the frame list, once as the skin's
    # inverse-bind matrices - and the two are not obliged to agree.  What they are obliged to do is
    # differ by the *same* matrix everywhere, because that matrix is the change of basis between the
    # space the mesh's vertices are stored in and the space the frames are posed in, and it cancels
    # in `v * skinToBone[i] * boneWorld[i]`.  Call it P, with `P = skinToBone[i] * frameWorld[i]`.
    #
    # San Andreas' own peds have a P that is a cyclic permutation of the axes: their frame list holds
    # a Y-up A-pose and their mesh is stored X-up.  103 of Vice City's 105 have P = the identity.
    # `fireman` and `pga`/`pgb`, the three written by a later RenderWare, are like San Andreas'.
    #
    # So the frame list is rebuilt from the donor's **frame list** and the skin from the donor's
    # **skin**, and P is carried across rather than flattened.  Deriving the frames from the skin
    # instead - which is what this did first, and it passed every structural check - quietly put
    # `fireman` alone among the region's peds in a different basis from the rest, lying on its side
    # relative to them until an animation keyed the root frame.  What caught it was not a structural
    # check but the measured fact that its feet had ended up 4 millimetres below its origin instead
    # of the 0.75 to 0.95 San Andreas' own peds use.
    by_frame = {}
    for node, (node_id, _ix, _fl) in enumerate(header["nodes"]):
        by_frame[donor_node_bone[node]] = (by_id[node_id]["world"], skin["matrices"][node])
    world = {bone: pair[0] for bone, pair in by_frame.items()}
    basis_changes = [rig.mat_mul(pair[1], pair[0]) for pair in by_frame.values()]
    reference_p = basis_changes[0]
    drift = max(max(abs(a - b) for a, b in zip(p[0] + p[1], reference_p[0] + reference_p[1]))
                for p in basis_changes)
    if drift > 1e-3:
        raise PedError("the skin and the frame list disagree about the model's basis by %.5f; "
                       "they should differ by one constant matrix, not by bone" % drift)

    stature = abs(world[5][1][1] - world[43][1][1])
    ratio = stature / sa_stature if sa_stature else 1.0
    derived = rig.derive_missing(world, sa_local, ratio)

    indices, weights = [], []
    for v in range(geometry["num_vert"]):
        row = skin["indices"][v]
        indices.append(tuple(index_map[row[c]] if weights_nonzero(skin["weights"][v], c) else 0
                             for c in range(4)))
        weights.append(skin["weights"][v])
    # A bone the donor had keeps its own matrix byte for byte; one San Andreas adds gets the same
    # basis change applied to its new frame, so P stays constant across all 32.
    matrices = [by_frame[bone][1] if bone in by_frame
                else rig.mat_mul(reference_p, rig.mat_inv(world[bone]))
                for bone, _flags in rig.SA_NODES]

    out = rebuild_clump(data, kids, version, write_frame_list(world),
                        write_skin(indices, weights, matrices))
    return out, dict(kind="vicecity", bones=len(donor_node_bone), derived=derived,
                     vertices=geometry["num_vert"], triangles=geometry["num_tri"],
                     stature=stature, ratio=ratio, geometries=1, basis="identity")


def weights_nonzero(row, c):
    return row[c] > 0.0


def rebuild_clump(data, kids, version, frame_list, skin_chunk):
    """The donor clump written as San Andreas', with our frame list and skin swapped in.

    The geometry, its materials and its textures all go through `import_city.convert_dff`'s own
    conversion - the same code every map model takes - so the version restamping, the material
    merging and the plugin filtering are not reimplemented here.
    """
    out = [ic.rw_chunk(ic.RW_STRUCT, struct.pack("<III", 1, 0, 0)), frame_list]
    for kind, ver, cs, ce in kids:
        if kind in (ic.RW_STRUCT, ic.RW_FRAMELIST):
            continue
        if kind == ic.RW_GEOMETRYLIST:
            gk = ic.rw_children(data, cs, ce, "geometry list")
            piece, _counts = ic.convert_geometry(data, gk[1][2], gk[1][3], version)
            out.append(ic.rw_chunk(ic.RW_GEOMETRYLIST,
                                   ic.rw_chunk(ic.RW_STRUCT, struct.pack("<i", 1))
                                   + replace_skin(piece, skin_chunk)))
        elif kind == ic.RW_ATOMIC:
            piece, _counts = ic.convert_atomic(data, cs, ce, version)
            out.append(retarget_atomic(piece))
        elif kind == ic.RW_EXTENSION:
            out.append(ic.convert_extension(data, cs, ce, "clump"))
        else:
            out.append(ic.rw_chunk(kind, data[cs:ce]))
    if not any(struct.unpack_from("<I", c, 0)[0] == ic.RW_EXTENSION for c in out[2:]):
        out.append(ic.rw_chunk(ic.RW_EXTENSION, b""))
    return ic.rw_chunk(ic.RW_CLUMP, b"".join(out))


def replace_skin(geometry_chunk, skin_chunk):
    """Swap the skin plugin inside an already-converted geometry, leaving its others alone."""
    kind, size, _lib = struct.unpack_from("<III", geometry_chunk, 0)
    kids = ic.rw_children(geometry_chunk, 12, 12 + size, "converted geometry")
    body, done = [], False
    for k, _v, cs, ce in kids:
        if k != ic.RW_EXTENSION:
            body.append(geometry_chunk[cs - 12:ce])
            continue
        inner = []
        for k2, _v2, c2, e2 in ic.rw_children(geometry_chunk, cs, ce, "geometry extension"):
            if k2 == RW_SKIN:
                inner.append(skin_chunk)
                done = True
            else:
                inner.append(geometry_chunk[c2 - 12:e2])
        if not done:
            inner.append(skin_chunk)
            done = True
        body.append(ic.rw_chunk(ic.RW_EXTENSION, b"".join(inner)))
    if not done:
        body.append(ic.rw_chunk(ic.RW_EXTENSION, skin_chunk))
    return ic.rw_chunk(ic.RW_GEOMETRY, b"".join(body))


def retarget_atomic(atomic_chunk):
    """Put the atomic on the frame San Andreas puts its own peds' atomics on."""
    kind, size, _lib = struct.unpack_from("<III", atomic_chunk, 0)
    kids = ic.rw_children(atomic_chunk, 12, 12 + size, "converted atomic")
    body = []
    for k, _v, cs, ce in kids:
        if k == ic.RW_STRUCT:
            fields = list(struct.unpack_from("<IIII", atomic_chunk, cs))
            fields[0] = rig.SA_ATOMIC_FRAME
            fields[1] = 0
            body.append(ic.rw_chunk(ic.RW_STRUCT, struct.pack("<IIII", *fields)))
        else:
            body.append(atomic_chunk[cs - 12:ce])
    return ic.rw_chunk(ic.RW_ATOMIC, b"".join(body))


# ---------------------------------------------------------------- GTA III

def convert_iii_ped(data, sa_local, sa_bind, sa_stature):
    """A GTA III ped: 16 rigid parts welded into one skinned San Andreas mesh.

    There is no skin plugin and no hierarchy to convert, so both are built.  Each atomic's vertices
    are carried into a single model space by its own frame's world matrix and weighted 1.0 to the
    bone that frame stood for, with that bone's inverse-bind matrix being the same world matrix
    inverted - so the product is the identity at bind and the mesh starts exactly where GTA III drew
    it.  The parts stay rigid, because rigid is what they were; giving them blended weights would be
    inventing data the donor never had.
    """
    version, kids = clump_children(data)
    frames = None
    geometries, atomics = [], []
    for kind, _v, cs, ce in kids:
        if kind == ic.RW_FRAMELIST:
            frames, _header = read_frames(data, cs, ce)
        elif kind == ic.RW_GEOMETRYLIST:
            for g in ic.rw_children(data, cs, ce, "geometry list")[1:]:
                geometries.append(read_geometry(data, g[2], g[3]))
        elif kind == ic.RW_ATOMIC:
            head = ic.rw_children(data, cs, ce, "atomic")[0]
            frame_index, geom_index = struct.unpack_from("<II", data, head[2])[:2]
            atomics.append((frame_index, geom_index))
    if frames is None:
        raise PedError("no frame list")
    if not geometries or not atomics:
        raise PedError("%d geometry(s), %d atomic(s)" % (len(geometries), len(atomics)))

    # Which way round GTA III's `l` and `r` are on *this* model, and the basis its ped model space
    # sits in.  They have to be solved together, and neither is assumed.
    #
    # GTA III does not name its limbs consistently, so both readings of `supperlegl` are fitted.
    # What decides between them is *not* the residual, and it is worth saying why, because the
    # residual is the obvious choice and it is useless here.  A ped skeleton is very nearly
    # symmetric front to back - a metre and a half tall, half a metre across, and barely fifteen
    # centimetres deep - so swapping left for right is almost exactly a half turn about the vertical
    # axis, and the fit obliges: over all 78 of GTA III's peds the two readings' residuals differ by
    # at most 1%, and on most of them by under 0.5%. Choosing on residual is choosing on noise, and
    # the ped it gets wrong is not subtly wrong - it is built facing backwards.
    #
    # So the model is asked which way its feet point.  A foot's mesh sticks out well in front of its
    # ankle joint, by a good part of the foot's length, and San Andreas' ped model space has +Z
    # forward - its own L Toe0 sits 0.15 in front of its L Foot.  Carrying each foot part's vertex
    # centroid and its joint through the fitted basis and taking the difference therefore gives a
    # forward vector that is around 0.1 long against joint positions good to a millimetre, which is
    # signal rather than noise.  The reading whose feet end up pointing forwards is the model's own.
    centroid_of_frame = {}
    for frame_index, geom_index in atomics:
        if geom_index >= len(geometries):
            continue
        g = geometries[geom_index]
        blocks = geometry_blocks(data, g, version)
        morph = blocks["morphs"][0] if blocks["morphs"] else None
        if not morph or not morph["verts"]:
            continue
        placed = [rig.mat_apply(frames[frame_index]["world"], v) for v in morph["verts"]]
        centroid_of_frame[frame_index] = tuple(sum(p[i] for p in placed) / len(placed)
                                               for i in range(3))

    sa_joint = {bone: m[1] for bone, m in sa_bind.items()}
    tried, failed = [], []
    for label, table in (("straight", rig.III_BONE_MAP), ("mirrored", rig.III_BONE_MAP_MIRROR)):
        mapping = {}
        for f in frames:
            bone = table.get(rig.norm_bone_name(f["name"] or ""))
            if bone is not None:
                mapping[f["i"]] = bone
        missing = set(table.values()) - set(mapping.values())
        if missing:
            failed.append("%s: no frame for bone(s) %s" % (label, sorted(missing)))
            continue
        pos = {mapping[i]: frames[i]["world"][1] for i in mapping}
        fit = rig.fit_basis(pos, sa_joint)
        candidate = (fit[0], (0.0, 0.0, 0.0))
        forward = 0.0
        for frame_index, bone in mapping.items():
            if bone not in (43, 53) or frame_index not in centroid_of_frame:
                continue
            here = rig.mat_apply(candidate, frames[frame_index]["world"][1])
            there = rig.mat_apply(candidate, centroid_of_frame[frame_index])
            forward += there[2] - here[2]
        tried.append((label, mapping, fit, forward))
    if not tried:
        raise PedError("; ".join(failed))
    tried.sort(key=lambda t: -t[3])
    reading, bone_of_frame, (rot, scale, centroids, rms, count), forward = tried[0]
    other = tried[1] if len(tried) > 1 else None
    if forward <= 0:
        raise PedError("neither left/right reading puts the feet in front of the ankles "
                       "(%s); the model does not read as a person facing forward"
                       % ", ".join("%s %+.3f" % (t[0], t[3]) for t in tried))
    if scale <= 0:
        raise PedError("the best fit has scale %.3f; the skeleton does not read as a person" % scale)
    # Rotation only, about the model's own origin.  The fit also offers a scale and a translation and
    # neither is taken, because measuring San Andreas' own peds says it should not be: across its 265
    # of them the foot bone sits anywhere from 0.746 to 0.953 below the origin, a spread of 0.207,
    # and GTA III's peds land inside that band on their own.  Scaling them to match one San Andreas
    # ped would only be picking which of its peds to be the same height as - and it showed: the
    # fitted scale moved from 1.06 to 0.88 purely by changing the reference model from a man to a
    # woman.  Taking the translation would be as arbitrary, and both donors already put their root
    # bone at the origin exactly where San Andreas does.
    basis = (rot, (0.0, 0.0, 0.0))

    world = {}
    for f in frames:
        if f["i"] in bone_of_frame:
            world[bone_of_frame[f["i"]]] = rig.mat_orthonormalise(
                rig.place_frame(f["world"], rot, basis))
    if 0 not in world:                      # GTA III's root frame is named after the ped itself
        root = next(f for f in frames if f["parent"] < 0)
        world[0] = rig.mat_orthonormalise(rig.place_frame(root["world"], rot, basis))
    stature = abs(world[5][1][1] - world[43][1][1])
    derived = rig.derive_missing(world, sa_local, stature / sa_stature if sa_stature else 1.0)

    verts, normals, uvs, prelit, tris, materials = [], [], [], [], [], []
    indices, weights, dropped = [], [], []
    sets = None
    for frame_index, geom_index in atomics:
        if geom_index >= len(geometries):
            raise PedError("an atomic names geometry %d of %d" % (geom_index, len(geometries)))
        bone = bone_of_frame.get(frame_index)
        if bone is None:
            dropped.append(frames[frame_index]["name"] or "frame %d" % frame_index)
            continue
        g = geometries[geom_index]
        blocks = geometry_blocks(data, g, version)
        if sets is None:
            sets = blocks["sets"]
            uvs = [[] for _ in range(sets)]
        elif sets != blocks["sets"]:
            raise PedError("parts disagree on texture-coordinate sets (%d and %d)"
                           % (sets, blocks["sets"]))
        morph = blocks["morphs"][0] if blocks["morphs"] else None
        if morph is None or morph["verts"] is None:
            raise PedError("a part has no vertices")
        # A point goes through the frame and then through the whole similarity; a normal goes
        # through the frame's rotation and then the fit's, and is renormalised.  Putting a normal
        # through the scale as well would leave it the wrong length on every vertex.
        place = frames[frame_index]["world"]
        base = len(verts)
        for v in morph["verts"]:
            verts.append(rig.mat_apply(basis, rig.mat_apply(place, v)))
        if morph["normals"] is not None:
            for n in morph["normals"]:
                normals.append(normalise(rig.mat_apply_dir((rot, (0.0, 0.0, 0.0)),
                                                           rig.mat_apply_dir(place, n))))
        for s in range(sets):
            uvs[s].extend(blocks["uvs"][s])
        prelit.extend(blocks["prelit"])
        node = rig.SA_NODE_INDEX[bone]
        for _ in morph["verts"]:
            indices.append((node, 0, 0, 0))
            weights.append((1.0, 0.0, 0.0, 0.0))
        surface = (data[g["struct_s"] + 16:g["struct_s"] + 28] if version < ic.RW_34
                   else struct.pack("<fff", 1.0, 1.0, 1.0))
        if g["matlist"] is None:
            raise PedError("a part has no material list")
        part_materials = ic.convert_material_list(data, g["matlist"][0], g["matlist"][1], surface)
        offset = len(materials)
        materials.extend(part_materials)
        for v2, v1, material, v3 in blocks["tris"]:
            if material >= len(part_materials):
                raise PedError("a triangle names material %d of %d"
                               % (material, len(part_materials)))
            tris.append((v2 + base, v1 + base, material + offset, v3 + base))

    if not verts:
        raise PedError("nothing left after welding")
    if normals and len(normals) != len(verts):
        raise PedError("%d normals for %d vertices" % (len(normals), len(verts)))

    unique, remap = ic.merge_materials(materials)
    tris = [(a, b, remap[m], c) for a, b, m, c in tris]
    matrices = [rig.mat_inv(world[bone]) for bone, _flags in rig.SA_NODES]
    geometry = build_geometry(verts, normals, uvs, prelit, tris, unique,
                              write_skin(indices, weights, matrices), sets or 0)
    clump = ic.rw_chunk(ic.RW_CLUMP, b"".join([
        ic.rw_chunk(ic.RW_STRUCT, struct.pack("<III", 1, 0, 0)),
        write_frame_list(world),
        ic.rw_chunk(ic.RW_GEOMETRYLIST,
                    ic.rw_chunk(ic.RW_STRUCT, struct.pack("<i", 1)) + geometry),
        ic.rw_chunk(ic.RW_ATOMIC,
                    ic.rw_chunk(ic.RW_STRUCT,
                                struct.pack("<IIII", rig.SA_ATOMIC_FRAME, 0, 5, 0))
                    + ic.rw_chunk(ic.RW_EXTENSION, b"")),
        ic.rw_chunk(ic.RW_EXTENSION, b""),
    ]))
    return clump, dict(kind="libertycity", bones=len(bone_of_frame), derived=derived,
                       vertices=len(verts), triangles=len(tris), stature=stature,
                       ratio=stature / sa_stature if sa_stature else 1.0,
                       geometries=len(geometries), materials=len(unique), dropped=dropped,
                       basis="%s l/r, %d joints, scale %.4f, rms %.4f m, feet %+.3f forward "
                             "(the other reading: rms %.4f, feet %+.3f)"
                             % (reading, count, scale, rms, forward,
                                other[2][3] if other else float("nan"),
                                other[3] if other else float("nan")))


def normalise(v):
    length = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) ** 0.5
    return (v[0] / length, v[1] / length, v[2] / length) if length > 1e-9 else v


def build_geometry(verts, normals, uvs, prelit, tris, materials, skin_chunk, sets):
    """One San Andreas geometry from welded parts, with a fresh bin mesh and bounding sphere.

    The meshes written are lists rather than strips - the triangle array is the geometry's own
    authority on what belongs to which material, and stitching strips across a weld would need
    degenerate triangles between them - so rpGEOMETRYTRISTRIP is left clear.
    """
    flags = 0x02 | 0x20                                     # positions, and lit by the engine
    flags |= ic.GEOM_TEXTURED if sets == 1 else (ic.GEOM_TEXTURED2 if sets >= 2 else 0)
    if normals:
        flags |= 0x10
    if prelit:
        flags |= ic.GEOM_PRELIT
    flags |= (sets & 0xFF) << 16
    body = [struct.pack("<IiiI", flags, len(tris), len(verts), 1)]
    if prelit:
        body.append(b"".join(prelit))
    for s in range(sets):
        body.append(struct.pack("<%df" % len(uvs[s]), *uvs[s]))
    body.append(b"".join(struct.pack("<4H", *t) for t in tris))
    lo = [min(v[i] for v in verts) for i in range(3)]
    hi = [max(v[i] for v in verts) for i in range(3)]
    centre = [(lo[i] + hi[i]) / 2.0 for i in range(3)]
    radius = max(sum((v[i] - centre[i]) ** 2 for i in range(3)) for v in verts) ** 0.5
    body.append(struct.pack("<4f", centre[0], centre[1], centre[2], radius))
    body.append(struct.pack("<ii", 1, 1 if normals else 0))
    body.append(b"".join(struct.pack("<3f", *v) for v in verts))
    if normals:
        body.append(b"".join(struct.pack("<3f", *n) for n in normals))

    groups = {}
    for v2, v1, material, v3 in tris:
        groups.setdefault(material, []).extend((v1, v2, v3))
    meshes, total = [], 0
    for material in sorted(groups):
        idx = groups[material]
        meshes.append(struct.pack("<II", len(idx), material)
                      + struct.pack("<%dI" % len(idx), *idx))
        total += len(idx)
    binmesh = struct.pack("<III", 0, len(meshes), total) + b"".join(meshes)

    return ic.rw_chunk(ic.RW_GEOMETRY, b"".join([
        ic.rw_chunk(ic.RW_STRUCT, b"".join(body)),
        ic.material_list_chunk(materials),
        ic.rw_chunk(ic.RW_EXTENSION, ic.rw_chunk(ic.RW_BINMESH, binmesh) + skin_chunk),
    ]))


# ---------------------------------------------------------------- reading it back

def verify(blob, name):
    """Everything about a converted ped that can be settled without running the game.

    Returns a list of complaints; an empty list means it passed.  This is deliberately paranoid,
    because the failure mode of a bad skin is not an error message - it is a ped rendered as a spike
    through the world origin, or a crash inside the D3D pipeline with no useful stack.
    """
    bad = []
    try:
        kind, size, library_id = struct.unpack_from("<III", blob, 0)
    except struct.error:
        return ["%s: too short to be a chunk" % name]
    if kind != ic.RW_CLUMP:
        return ["%s: root chunk is %#x, not a clump" % (name, kind)]
    if library_id != ic.SA_LIB_ID:
        bad.append("root library id %#x, not San Andreas' %#x" % (library_id, ic.SA_LIB_ID))
    if 12 + size != len(blob):
        bad.append("clump claims %d bytes, the blob is %d" % (size, len(blob) - 12))

    def walk(s, e, path):
        for k, v, cs, ce in ic.rw_children(blob, s, e, path):
            head = struct.unpack_from("<I", blob, cs - 4)[0]
            if head != ic.SA_LIB_ID:
                bad.append("%s/%#x still carries library id %#x" % (path, k, head))
            if k in ic.RW_CONTAINERS or k == ic.RW_GEOMETRY:
                try:
                    walk(cs, ce, "%s/%#x" % (path, k))
                except ic.RwError:
                    pass
    walk(12, 12 + size, "clump")

    version, kids = clump_children(blob)
    counts = struct.unpack_from("<III", blob, kids[0][2])
    if counts != (1, 0, 0):
        bad.append("clump struct says %s atomics/lights/cameras, expected (1, 0, 0)" % (counts,))
    frames = header = geometry = None
    atomic_frame = None
    for k, _v, cs, ce in kids:
        if k == ic.RW_FRAMELIST:
            frames, header = read_frames(blob, cs, ce)
        elif k == ic.RW_GEOMETRYLIST:
            gk = ic.rw_children(blob, cs, ce, "geometry list")
            if len(gk) != 2:
                bad.append("%d geometries, expected 1" % (len(gk) - 1))
            else:
                geometry = read_geometry(blob, gk[1][2], gk[1][3])
        elif k == ic.RW_ATOMIC:
            atomic_frame = struct.unpack_from("<I", blob, ic.rw_children(
                blob, cs, ce, "atomic")[0][2])[0]

    if frames is None:
        return [("%s: " % name) + c for c in bad + ["no frame list"]]
    if len(frames) != len(rig.SA_BONES) + 1:
        bad.append("%d frames, San Andreas' peds have %d" % (len(frames), len(rig.SA_BONES) + 1))
    for bone, bname, parent in rig.SA_BONES:
        i = rig.SA_FRAME_INDEX[bone]
        if i >= len(frames):
            bad.append("no frame for %s" % bname)
            continue
        f = frames[i]
        if f["name"] != bname:
            bad.append("frame %d is %r, expected %r" % (i, f["name"], bname))
        if f["hanim_id"] != bone:
            bad.append("frame %d (%s) has hierarchy id %s, expected %d"
                       % (i, bname, f["hanim_id"], bone))
        want_parent = 0 if parent is None else rig.SA_FRAME_INDEX[parent]
        if f["parent"] != want_parent:
            bad.append("frame %d (%s) hangs on %d, expected %d"
                       % (i, bname, f["parent"], want_parent))
        err = rig.mat_orthonormality(f["local"])
        if err > 1e-3:
            bad.append("frame %d (%s) local rotation is %.5f off orthonormal" % (i, bname, err))
    if header is None:
        bad.append("no animation hierarchy")
    else:
        got = [n[0] for n in header["nodes"]]
        want = [b for b, _f in rig.SA_NODES]
        if got != want:
            bad.append("hierarchy node order differs from San Andreas'")
        if header["key_size"] != 36:
            bad.append("hierarchy keyframe size %d, expected 36" % header["key_size"])
    if atomic_frame is not None and atomic_frame >= len(frames):
        bad.append("the atomic hangs on frame %d of %d" % (atomic_frame, len(frames)))

    if geometry is None:
        bad.append("no geometry")
        return [("%s: " % name) + c for c in bad]
    if geometry["skin"] is None:
        bad.append("the geometry has no skin plugin")
        return [("%s: " % name) + c for c in bad]
    skin = read_skin(blob, geometry["skin"][0], geometry["skin"][1], geometry["skin"][2],
                     geometry["num_vert"])
    if skin["stride"] != 64:
        bad.append("skin bone stride %d, San Andreas' is 64" % skin["stride"])
    if skin["num_bones"] != len(rig.SA_NODES):
        bad.append("skin has %d bones, expected %d" % (skin["num_bones"], len(rig.SA_NODES)))
    if not skin["used"]:
        bad.append("skin names no used bones; the pipeline would upload no matrices")
    for b in skin["used"]:
        if b >= len(rig.SA_NODES):
            bad.append("used-bone list names bone %d of %d" % (b, len(rig.SA_NODES)))
    seen = set()
    worst_sum = 0.0
    for v in range(geometry["num_vert"]):
        total = 0.0
        for c in range(4):
            w = skin["weights"][v][c]
            if w > 0.0:
                seen.add(skin["indices"][v][c])
                total += w
        worst_sum = max(worst_sum, abs(total - 1.0))
    if worst_sum > 1e-3:
        bad.append("a vertex's weights sum to %.5f away from 1" % worst_sum)
    if seen - set(skin["used"]):
        bad.append("vertices name bones the used-bone list leaves out: %s"
                   % sorted(seen - set(skin["used"])))
    for b in sorted(seen):
        bone = rig.SA_NODES[b][0]
        if bone not in rig.SA_PARENT:
            bad.append("a vertex is weighted to node %d, which is no San Andreas bone" % b)
    # `skinToBone[i] * frameWorld[i]` has to come to the same matrix for every bone: that product is
    # the change of basis between the space the vertices live in and the space the frames are posed
    # in, and it is a property of the model, not of the bone.  Requiring it to be the *identity*
    # would be wrong - San Andreas' own peds have a permutation there - but requiring it to be
    # constant is exactly right, and it is what catches a node array put in the wrong order, which
    # is the mistake that would otherwise show up only as a ped folded through itself.
    world = {}
    for bone, _bname, parent in rig.SA_BONES:
        f = frames[rig.SA_FRAME_INDEX[bone]]
        world[bone] = f["local"] if parent is None else rig.mat_mul(f["local"], world[parent])
    changes = [(bone, rig.mat_mul(skin["matrices"][i], world[bone]))
               for i, (bone, _flags) in enumerate(rig.SA_NODES)]
    reference = changes[0][1]
    worst_bind, worst_bone = 0.0, None
    for bone, change in changes:
        err = max(max(abs(a - b) for a, b in zip(change[0], reference[0])),
                  max(abs(a - b) for a, b in zip(change[1], reference[1])))
        if err > worst_bind:
            worst_bind, worst_bone = err, rig.SA_NAME[bone]
    if worst_bind > 1e-3:
        bad.append("the skin and the frame list disagree about the basis by %.5f at %s; it should "
                   "be one matrix for the whole model" % (worst_bind, worst_bone))

    blocks = geometry_blocks(blob, geometry, version)
    morph = blocks["morphs"][0]
    xs = [v[0] for v in morph["verts"]]
    ys = [v[1] for v in morph["verts"]]
    zs = [v[2] for v in morph["verts"]]
    span = (max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
    if max(span) > 8.0 or max(span) < 0.5:
        bad.append("bounding box %.2f x %.2f x %.2f is not a person" % span)
    # Where the feet sit relative to the model origin decides whether a ped stands on the ground,
    # floats above it or sinks into it.  San Andreas' own 265 peds put the foot bone between 0.746
    # and 0.953 below the origin - a spread of 0.207, which is how much variation the game is
    # demonstrably happy with - so that band, loosened a little, is the test.  Vice City's own 107
    # peds fall inside it unaided, from 0.753 to 0.933.
    foot = (world[43][1][1] + world[53][1][1]) / 2.0
    if not -1.05 < foot < -0.65:
        bad.append("the feet sit %.3f below the origin; San Andreas' own peds are between "
                   "0.746 and 0.953, so this would float or sink" % -foot)
    for v in morph["verts"]:
        if any(x != x or abs(x) > 1e6 for x in v):
            bad.append("a vertex is not a finite position")
            break
    for v2, v1, material, v3 in blocks["tris"]:
        if max(v1, v2, v3) >= geometry["num_vert"]:
            bad.append("a triangle names a vertex the geometry does not have")
            break
    return [("%s: " % name) + c for c in bad], dict(span=span, bones_used=len(skin["used"]),
                                                    max_weights=skin["max_weights"])


# ---------------------------------------------------------------- the donor's ped table

def read_donor_peds(ide_path):
    """[(id, model, txd, pedtype, stat, animgroup, rest)] from a donor's `peds` section."""
    rows, section = [], False
    with open(ide_path, "r", encoding="latin-1") as f:
        for raw in f:
            s = raw.strip()
            low = s.lower()
            if not section:
                if low == "peds":
                    section = True
                continue
            if low == "end":
                break
            if not s or s.startswith("#"):
                continue
            cols = [c.strip() for c in s.split(",")]
            if len(cols) < 6:
                continue
            rows.append(cols)
    return rows


def sa_anim_groups(sa_dir):
    """The animation-group names San Andreas declares, read from data/animgrp.dat.

    Not from peds.ide's own column, which only shows the sixteen groups its own peds happen to use;
    animgrp.dat declares twenty-one, and the five it adds - `shuffle`, `oldfatman`, `drunkman`,
    `blindman`, `fatwoman` - are exactly the ones a donor is most likely to ask for.  A group San
    Andreas has no animations for leaves the ped standing in its bind pose, so this is checked
    rather than hoped for.
    """
    path = ic.find_file(sa_dir, "data/animgrp.dat")
    names = set()
    if not path:
        return names
    # A group is declared by an unindented `name, file, type, count` line; the animation names that
    # follow it are indented and carry no commas, and taking those for group names would let a
    # donor's `walk_civi` through as if San Andreas had a group by that name.
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            if not raw[:1].strip():
                continue
            s = raw.split("#", 1)[0].strip()
            cols = [c.strip() for c in s.split(",")]
            if len(cols) >= 4 and re.match(r"^[A-Za-z][A-Za-z0-9_]*$", cols[0]):
                names.add(cols[0].lower())
    return names


# Where a donor group San Andreas does not declare should land.  Kept small deliberately: it is only
# consulted for a name animgrp.dat does not hold, and on the retail files that is `player` alone.
ANIM_GROUP_FALLBACK = {"player": "man", "casual": "man", "cop": "man", "shopping": "busywoman"}

# Animation groups whose animations live in a block San Andreas STREAMS on demand rather than the
# always-loaded "ped" block. animgrp.dat's second column names the block, and every group a donor
# ped uses sits in "ped" except these. A region ped can be spawned anywhere at any time without the
# game having decided to load that block, and CPed::SetModelIndex then faults building the ped -
# CAnimBlendAssocGroup::CopyAnimation walks a null association array (crash at 0x4CE15C). So these
# are never written out; the ped falls back to a "ped"-block group by gender instead.
STREAMED_ANIM_BLOCKS = {"skate"}


def sa_anim_group(donor_group, declared):
    g = (donor_group or "").strip().lower()
    if g in declared and g != "player" and g not in STREAMED_ANIM_BLOCKS:
        return g
    hit = ANIM_GROUP_FALLBACK.get(g)
    if hit and hit in declared and hit not in STREAMED_ANIM_BLOCKS:
        return hit
    return "woman" if "wom" in g else "man"


# GTA III, Vice City and San Andreas each number the "cars this ped may drive" bits differently, and
# the three headers are in the games' own .ide files.  GTA III has no NORMAL_CAR bit, so everything
# below its BIG_CAR sits one place lower than in the other two; it also has a SPECIAL_CAR bit at 16
# that neither successor has, which is dropped.  Copying a GTA III mask across unchanged would turn
# its POOR_FAMILY into San Andreas' NORMAL and its SPECIAL into San Andreas' WORKER.
III_TO_SA_CAR_BIT = {0: 1, 1: 2, 2: 3, 3: 4, 4: None, 5: 5, 6: 6}


def convert_car_mask(mask, kind):
    if kind != "libertycity":
        return mask                      # Vice City and San Andreas number these the same way
    out = 0
    for bit in range(7):
        if mask & (1 << bit):
            moved = III_TO_SA_CAR_BIT.get(bit)
            if moved is not None:
                out |= 1 << moved
    return out | (mask & ~0x7F)


def ide_line(model_id, model, txd, cols, stat_types, anim_groups, kind):
    """A San Andreas `peds` line for a donor ped.

    A donor line is shorter than San Andreas', and the columns it is short of are the ones this has
    to invent.  GTA III writes seven columns - id, model, txd, ped type, stat, animation group and
    the mask of cars the ped may drive - and Vice City ten, adding an animation file and two radio
    stations.  San Andreas writes fourteen: it splits a second flag word out behind the car mask,
    and it ends with a `PED_TYPE_` and two voice names.

      * **ped type** and **stat** come across unchanged where San Andreas declares them and fall
        back to CIVMALE / STAT_STREET_GUY where it does not.
      * **animation group** is mapped through `sa_anim_group`, because a group San Andreas has no
        .ifp for would leave the ped standing in its bind pose.
      * **animation file** is forced to `null`.  A donor's would name an .ifp block San Andreas does
        not have, and `CAnimManager::GetAnimationBlock` returning nothing there is a null dereference
        rather than a fallback.
      * **voice** is `VOICE_GEN_MALE01` or `VOICE_GEN_FEM01` by the ped type.  The donors' voices
        live in their own audio archives, which nothing here converts, and a `VOICE_` name San
        Andreas does not know is not resolvable.
    """
    ped_type = cols[3].strip().upper()
    ped_type = DONOR_PED_TYPE.get(ped_type, ped_type)
    if ped_type not in SA_PED_TYPES:
        ped_type = "CIVMALE"
    stat = cols[4].strip().upper()
    if stat not in stat_types:
        stat = "STAT_STREET_GUY"
    group = sa_anim_group(cols[5], anim_groups)
    try:
        cars = "%X" % convert_car_mask(int((cols[6] or "0").strip() or "0", 16), kind)
    except (ValueError, IndexError):
        cars = "0"
    female = ped_type in ("CIVFEMALE", "PROSTITUTE")
    voice = "VOICE_GEN_FEM01" if female else "VOICE_GEN_MALE01"
    # One of San Andreas' six audio ped types, never a name invented here: anything else is -1 from
    # CAEPedSpeechAudioEntity::GetAudioPedType and a ped that never speaks. Police, paramedics and
    # firemen are all PED_TYPE_EMG; there is no PED_TYPE_COP or PED_TYPE_MEDIC.
    kind = "PED_TYPE_GEN"
    if ped_type in ("COP", "MEDIC", "FIREMAN"):
        kind, voice = "PED_TYPE_EMG", "VOICE_GEN_MALE01"
    elif ped_type.startswith("GANG"):
        kind = "PED_TYPE_GANG"
    assert kind in SA_AUDIO_PED_TYPES, kind
    return "%d, %s, %s, %s, %s, %s, %s, 0, null, 9,9, %s, %s, %s" % (
        model_id, model, txd, ped_type, stat, group, cars, kind, voice, voice)


# CPedType::ms_apszPedTypeNames, read out of gta_sa.exe at 0x8D23B8 rather than recalled: thirty-two
# names, and CPedType::FindPedType (0x608790) answers 32 - one past the thirty-two-entry
# CPedType::ms_apPedType - for anything that is not one of them and is not PLAYER_NETWORK or
# PLAYER_UNUSED (0x608825, `add eax, 0x20`). CPedType::GetPedFlag guards that index; the
# acquaintance getters at 0x6089B0 and its neighbours do not.
#
# Written down because the list this had before was wrong in both directions: it offered PLAYER3,
# PLAYER4 and EMERGENCY, which San Andreas does not have, and lacked MISSION2..MISSION8. EMERGENCY
# is the one that mattered - it is what Vice City's and GTA III's `medic` declares, so one ped per
# region went out with a ped type of 32.
SA_PED_TYPES = {"PLAYER1", "PLAYER2", "PLAYER_NETWORK", "PLAYER_UNUSED",
                "CIVMALE", "CIVFEMALE", "COP",
                "GANG1", "GANG2", "GANG3", "GANG4", "GANG5", "GANG6", "GANG7", "GANG8", "GANG9",
                "GANG10", "DEALER", "MEDIC", "FIREMAN", "CRIMINAL", "BUM", "PROSTITUTE", "SPECIAL",
                "MISSION1", "MISSION2", "MISSION3", "MISSION4", "MISSION5", "MISSION6", "MISSION7",
                "MISSION8"}

# What a donor's ped type becomes when San Andreas has no such name but does have the same idea.
DONOR_PED_TYPE = {"EMERGENCY": "MEDIC", "PLAYER3": "PLAYER2", "PLAYER4": "PLAYER2"}

# CAEPedSpeechAudioEntity's six audio ped types, the pointers at 0x8C8108; GetAudioPedType
# (0x4E3C60) returns -1 for anything else and GetVoice then returns -1 as well, which is a ped that
# never speaks. The `PED_TYPE_COP` and `PED_TYPE_MEDIC` this used to write are not among them.
SA_AUDIO_PED_TYPES = {"PED_TYPE_GEN", "PED_TYPE_EMG", "PED_TYPE_PLAYER", "PED_TYPE_GANG",
                      "PED_TYPE_GFD", "PED_TYPE_SPC"}


def sa_stat_types(sa_dir):
    """The STAT_ names San Andreas declares, so a donor's can be checked against them."""
    path = ic.find_file(sa_dir, "data/pedstats.dat")
    names = set()
    if not path:
        return names
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            s = raw.strip()
            if s.startswith("STAT_"):
                names.add(s.split()[0].upper())
    return names


# ---------------------------------------------------------------- ped texture dictionaries

# San Andreas' own peds - the 265 in player.img - are stored one way and one way only: platform 9,
# an uncompressed 32-bit raster (rasterFormat 0x0600 X8R8G8B8, or 0x0500 A8R8G8B8 when it has an
# alpha channel), a single mip level, rasterType 4, and no mipmap or palette flags. Verified by
# parsing every raster in models/player.img: 394 of 394 are (platform 9, depth 32, mips 1, raw).
#
# The map path (import_city.convert_txd) instead restamps a donor raster's header in place and
# copies its levels through untouched, so a Vice City ped's DXT1/DXT3-compressed, nine-mip-level
# skin reaches San Andreas as a compressed, mipmapped platform-9 raster. That form is valid - San
# Andreas' own gta3.img ships thousands of identical ones for the map - but a ped is a skinned model
# uploaded through a different D3D9 path than the map, and the safe, evidence-backed thing to do is
# to hand a ped exactly the texture form San Andreas' own peds use rather than a form only its map
# textures are ever seen in. So a ped's dictionary is not restamped: every raster in it is decoded
# to pixels and re-emitted as a plain 32-bit BGRA texture, the player.img format.
#
# The decoders are import_city's own (decode_dxt, decode_raw) - the same ones that already turn a
# donor raster into the PNGs and radar tiles - so this reimplements no pixel handling.

def _decode_native_level0(body):
    """(width, height, has_alpha, rgba) for the base level of a III/VC native-texture struct.

    Handles every raster form the two donors actually carry: D3D8 DXT1/DXT3, D3D8 8-bit PAL8, and
    D3D8 uncompressed 16/24/32-bit. Returns None for anything else (PAL4, DXT2/4/5) so the caller
    can fall back rather than emit a guess.
    """
    if len(body) < 88:
        return None
    platform = struct.unpack_from("<I", body, 0)[0]
    raster_format, d3d_or_alpha = struct.unpack_from("<II", body, 72)
    width, height, depth, _mips, _rtype, flag = struct.unpack_from("<HHBBBB", body, 80)
    at = 88
    palette = None
    if raster_format & 0x2000:            # PAL8
        palette = [tuple(body[at + i * 4:at + i * 4 + 4]) for i in range(256)]
        at += 1024
    elif raster_format & 0x4000:          # PAL4 - neither donor has one; let the caller fall back
        return None
    at += 4                               # the level-0 <u32 size>, then its pixels
    if platform == 9:
        fourcc = struct.pack("<I", d3d_or_alpha)
        if flag & 0x08 and fourcc in (b"DXT1", b"DXT3"):
            return width, height, None, ic.decode_dxt(body, at, width, height, fourcc == b"DXT3")
        if flag & 0x08:
            return None
    elif platform == 8:
        if flag in (1, 2, 3, 4, 5):       # D3D8 dxtCompression; only DXT1/DXT3 are decodable here
            if flag not in (1, 3):
                return None
            return width, height, None, ic.decode_dxt(body, at, width, height, flag == 3)
    else:
        return None                       # PS2/Xbox rasters carry different pixel data entirely
    return width, height, None, ic.decode_raw(body, at, width, height, depth, raster_format, palette)


def ped_texture_native(body):
    """A III/VC native-texture struct re-emitted as San Andreas' own ped texture form.

    Platform 9, a single uncompressed 32-bit level in D3D's BGRA order, rasterType 4, alpha only
    when the decoded pixels actually carry it, and no mipmap or palette flags - the player.img
    format. Returns None if the raster cannot be decoded, so the caller falls back to the map path's
    header-restamp rather than dropping the texture.
    """
    decoded = _decode_native_level0(body)
    if decoded is None:
        return None
    width, height, _ha, rgba = decoded
    if len(rgba) < width * height * 4:
        return None
    has_alpha = any(rgba[i * 4 + 3] != 0xFF for i in range(width * height))
    bgra = bytearray(width * height * 4)
    for i in range(width * height):
        r, g, b, a = rgba[i * 4:i * 4 + 4]
        bgra[i * 4:i * 4 + 4] = bytes((b, g, r, a))
    raster_format = 0x0500 if has_alpha else 0x0600      # 8888 / 888, no mipmap or palette bit
    d3d_format = 21 if has_alpha else 22                 # A8R8G8B8 / X8R8G8B8
    out = bytearray(body[:88])
    struct.pack_into("<I", out, 0, 9)                    # platformId -> D3D9
    struct.pack_into("<II", out, 72, raster_format, d3d_format)
    struct.pack_into("<HH", out, 80, width, height)
    out[84] = 32                                         # depth
    out[85] = 1                                          # numLevels: one, no mip chain
    out[86] = 4                                          # rasterType rwRASTERTYPETEXTURE
    out[87] = 1 if has_alpha else 0                      # flags: bit0 alpha; no cube/automip/compress
    return bytes(out) + struct.pack("<I", len(bgra)) + bytes(bgra)


def convert_ped_txd(data):
    """A III/VC ped texture dictionary rebuilt in San Andreas' player.img texture form.

    Returns (bytes, converted, failed) like import_city.convert_txd, but decodes each raster to a
    plain 32-bit texture instead of restamping its header - see the note above. A raster that cannot
    be decoded falls back to the map path's restamp so it is never lost.
    """
    dictionary = None
    for kind, s, e in ic.rw_sections(data, 0, len(data)):
        if kind == ic.RW_TEX_DICTIONARY:
            dictionary = (s, e)
            break
    if dictionary is None:
        return data, 0, 0
    chunks, converted, failed = [], 0, 0
    for kind, s, e in ic.rw_sections(data, *dictionary):
        if kind != ic.RW_TEXTURE_NATIVE:
            continue
        for k2, s2, e2 in ic.rw_sections(data, s, e):
            if k2 != ic.RW_STRUCT:
                continue
            body = ped_texture_native(data[s2:e2])
            if body is None:                             # undecodable: keep it via the map restamp
                body = ic.convert_texture_native(data[s2:e2])
            if body is None:
                failed += 1
            else:
                converted += 1
                name = body[8:40].split(b"\0")[0].decode("ascii", "replace")
                chunks.append((name, ic.rw_chunk(ic.RW_TEXTURE_NATIVE,
                                                 ic.rw_chunk(ic.RW_STRUCT, body)
                                                 + ic.rw_chunk(ic.RW_EXTENSION, b""))))
            break
    if not chunks:
        return data, converted, failed
    return ic.build_txd(chunks), converted, failed


# ---------------------------------------------------------------- driving it

REGIONS = {"vicecity": ("v_", convert_vc_ped), "libertycity": ("l_", convert_iii_ped)}


def region_name(prefix, stem):
    """The imported name, under docs/asset-namespacing.md's scheme.

    Ped stems are short - the longest in either donor is 11 characters - so the two-character tag
    always fits inside the 19-character budget and none of them ever needs the hash fallback.
    """
    name = prefix + stem.lower()
    if len(name) > 19:
        raise PedError("%s is %d characters once tagged; the archive allows 19" % (name, len(name)))
    return name


def merge_into_archive(path, files):
    """Put these entries into an existing VER2 archive, replacing any of the same name.

    The peds go into the region's own archive rather than one of their own because San Andreas has
    room for eight archives in all - CStreaming::ms_files is 8 x 0x30 at 0x8E48D8 - and it has
    already used five of them (gta3, gta_int, carrec, script, cutscene) with player.img still to
    come in CGame::Init2. Two imported cities take it to seven; there is no eighth for a ped
    archive, and a third city could not register a map archive either. Merging costs a rewrite of
    the archive and no slot at all.

    Replacing by name is what lets this fix a region that was imported before the map path learned
    to leave skinned models out: the old, wrongly-stamped `v_wmyst.dff` is dropped here rather than
    sitting in front of the converted one, since CStreaming::LoadCdDirectory keeps the first entry
    it binds to a name (0x5B6449). Returns (added, replaced).
    """
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            raise PedError("%s is not a San Andreas VER2 archive" % path)
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
        existing = []
        for at in range(0, 32 * count, 32):
            off, sectors, _pad = struct.unpack_from("<IHH", table, at)
            name = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
            if name:
                existing.append((name, off * ic.SECTOR, sectors * ic.SECTOR))
        fresh = {name.lower() for name, _blob in files}
        kept, replaced = [], 0
        for name, off, size in existing:
            if name.lower() in fresh:
                replaced += 1
                continue
            f.seek(off)
            kept.append((name, f.read(size)))
    ic.write_img_v2(path, kept + list(files))
    return len(files) - replaced, replaced


def repoint_ped_groups(path, prefix, converted):
    """Point an existing pedgrp.dat's `DONOR/SAMODEL` pairs at the models just converted.

    This is the whole of "use the imported peds", and it is deliberately a data change. The plugin
    reads only the right-hand side of each pair (`src/peds/RegionPeds.cpp`, which splits on the `/`
    and looks the remainder up by name through CModelInfo::GetModelInfo), so swapping the San
    Andreas stand-in for the imported model is all there is to it - no code in `src/peds/` changes,
    which is what the pairing was for.

    A pair whose donor model was not converted keeps its stand-in, so a partial conversion leaves a
    working crowd rather than gaps. Returns (repointed, left as stand-ins), or None if there is no
    pedgrp.dat to rewrite - the map import writes that file, and it may not have been run.
    """
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="latin-1") as f:
        lines = f.read().splitlines()

    repointed = left = 0
    out = []
    for line in lines:
        if not line.lower().startswith("group "):
            out.append(line)
            continue
        parts = line.split()
        for i in range(3, len(parts)):
            donor, slash, _stand_in = parts[i].partition("/")
            if not slash:
                continue
            if donor.lower() in converted:
                parts[i] = "%s/%s" % (donor, region_name(prefix, donor))
                repointed += 1
            else:
                left += 1
        out.append("group %-3s %-18s %s" % (parts[1], parts[2], " ".join(parts[3:])))

    with open(path, "w", encoding="latin-1") as f:
        f.write("\n".join(out) + "\n")
    return repointed, left


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 2:
        sys.exit(__doc__)
    src, sa = args[0], args[1]
    region = "vicecity"
    out = None
    for o in opts:
        if o.startswith("--region="):
            region = o.split("=", 1)[1]
        elif o.startswith("--out="):
            out = o.split("=", 1)[1]
    check_only = "--check" in opts
    if region not in REGIONS:
        sys.exit("unknown region %r; try %s" % (region, " or ".join(sorted(REGIONS))))
    prefix, convert = REGIONS[region]
    if out is None:
        out = os.path.join(sa, "MapExpander", region.capitalize())

    img = ic.find_file(src, "models/gta3.img")
    directory = ic.find_file(src, "models/gta3.dir")
    ide = ic.find_file(src, "data/default.ide")
    if not (img and directory and ide):
        sys.exit("%s does not look like a GTA III or Vice City install" % src)

    sa_local, sa_bind, sa_stature = sa_reference_skeleton(sa)
    print("San Andreas' reference skeleton: %d bones, stature %.3f" % (len(sa_local), sa_stature))
    stat_types = sa_stat_types(sa)
    anim_groups = sa_anim_groups(sa)
    print("San Andreas declares %d stat type(s) and %d animation group(s)"
          % (len(stat_types), len(anim_groups)))

    rows = read_donor_peds(ide)
    wanted = {r[1].lower(): r for r in rows if r[1].lower() != "null"}
    entries = ic.read_img_v1(img, directory)
    blobs = {}
    with open(img, "rb") as f:
        for name, offset, size in entries:
            stem, _dot, ext = name.lower().rpartition(".")
            if ext not in ("dff", "txd"):
                continue
            if stem not in wanted:
                continue
            f.seek(offset)
            blobs.setdefault(ext, {})[stem] = f.read(size)
    # A ped's texture dictionary is named by its own column, which is usually but not always its
    # model name - so the txd set is gathered separately from the whole archive.
    txd_wanted = {r[2].lower() for r in wanted.values()}
    with open(img, "rb") as f:
        for name, offset, size in entries:
            stem, _dot, ext = name.lower().rpartition(".")
            if ext == "txd" and stem in txd_wanted:
                f.seek(offset)
                blobs.setdefault("txd", {})[stem] = f.read(size)

    files, lines, notes = [], [], []
    converted_stems = set()
    done = failed = 0
    for stem in sorted(blobs.get("dff", {})):
        cols = wanted[stem]
        try:
            blob, report = convert(blobs["dff"][stem], sa_local, sa_bind, sa_stature)
            complaints, measured = verify(blob, stem)
            if complaints:
                raise PedError("; ".join(c.split(": ", 1)[1] for c in complaints))
        except (PedError, ic.RwError, struct.error, ValueError, KeyError, IndexError) as err:
            failed += 1
            notes.append("%-14s SKIPPED  %s: %s" % (stem, type(err).__name__, err))
            continue
        done += 1
        converted_stems.add(stem.lower())
        model = region_name(prefix, stem)
        txd_stem = cols[2].lower()
        txd = region_name(prefix, txd_stem)
        files.append((model + ".dff", blob))
        notes.append("%-14s -> %-16s %5d verts %5d tris  %d bone(s) mapped, %d derived  "
                     "bbox %.2f x %.2f x %.2f  stature %.3f (%.0f%% of SA)  %s"
                     % (stem, model, report["vertices"], report["triangles"],
                        report["bones"], len(report["derived"]),
                        measured["span"][0], measured["span"][1], measured["span"][2],
                        report["stature"], 100.0 * report["ratio"], report["basis"]))
        lines.append(ide_line(int(cols[0]) if cols[0].strip().isdigit() else 0,
                              model, txd, cols, stat_types, anim_groups, region))

    seen_txd = set()
    for stem in sorted(blobs.get("txd", {})):
        name = region_name(prefix, stem)
        if name in seen_txd:
            continue
        seen_txd.add(name)
        try:
            blob, converted, failed = convert_ped_txd(blobs["txd"][stem])
            files.append((name + ".txd", blob))
            if failed:
                notes.append("%-14s -> %-16s %d texture(s), %d that would not convert"
                             % (stem + ".txd", name + ".txd", converted, failed))
        except (ic.RwError, struct.error, ValueError) as err:
            notes.append("%-14s TXD SKIPPED  %s" % (stem, err))

    print("converted %d ped(s), skipped %d, %d texture dictionary(s)"
          % (done, failed, len(seen_txd)))
    if check_only:
        for n in notes:
            print("  " + n)
        return
    os.makedirs(out, exist_ok=True)
    # Into the region's own archive when there is one, because there is no eighth streaming slot
    # for an archive of our own - see merge_into_archive. Falling back to peds.img keeps this
    # runnable on its own, and then the region needs a PedImg key.
    region_img = os.path.join(out, region + ".img")
    if os.path.isfile(region_img):
        added, replaced = merge_into_archive(region_img, files)
        print("%s.img: %d entry/entries added, %d replaced (an entry of the same name that was "
              "already there - the map import used to pack the donor's own ped models with a "
              "RenderWare 3.3 skin restamped 3.6)" % (region, added, replaced))
        ped_img = None
    else:
        ic.write_img_v2(os.path.join(out, "peds.img"), files)
        print("no %s.img here, so the peds went into peds.img of their own - the region needs a "
              "PedImg key, and San Andreas has room for eight archives in all" % region)
        ped_img = "peds.img"
    with open(os.path.join(out, "peds.ide"), "w", encoding="latin-1") as f:
        f.write("# Pedestrians imported from %s by tools/convert_peds.py.\n"
                "#\n"
                "# San Andreas' own peds.ide columns: modelId, model, txd, ped type, stat,\n"
                "# animation group, cars-can-drive mask, second flag word, animation file,\n"
                "# radio 1, radio 2, PED_TYPE_, voice, voice.\n"
                "#\n"
                "# The model ids here are the donor's own. The loader assigns real ones.\n"
                "peds\n" % os.path.basename(os.path.abspath(src)))
        f.write("\n".join(lines) + "\nend\n")
    with open(os.path.join(out, "peds.txt"), "w", encoding="latin-1") as f:
        f.write("# What became of each of %s's pedestrians.\n#\n" % region)
        f.write("\n".join(notes) + "\n")

    groups = repoint_ped_groups(os.path.join(out, "pedgrp.dat"), prefix, converted_stems)
    if groups is None:
        print("no pedgrp.dat in %s - run tools/import_city.py first and this again, or the "
              "region's crowd stays San Andreas'" % out)
    else:
        print("pedgrp.dat: %d pairing(s) now name the imported model, %d still name a San Andreas "
              "stand-in because the donor model was not converted" % groups)
    print("wrote %s" % out)
    folder = os.path.basename(os.path.normpath(out))
    print("this region's region.ini needs:\n    PedIde       = MapExpander\\%s\\peds.ide" % folder)
    if ped_img:
        print("    PedImg       = MapExpander\\%s\\%s" % (folder, ped_img))


if __name__ == "__main__":
    main()
