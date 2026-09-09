"""Read a converted peds.img back and hold every model in it against San Andreas' own.

    python tools/check_peds.py <region dir> <SA dir>

This is deliberately a *separate* reader from the one that wrote the archive.  It re-parses each
clump from the bytes on disk, re-derives the skeleton from the frame list alone, and compares the
result against a real San Andreas ped read the same way.  A converter checking its own output with
its own in-memory objects proves very little; this proves the bytes.

What it cannot prove is in docs/donor-peds.md: everything that only shows up once an animation is
playing.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import import_city as ic
import ped_rig as rig
import convert_peds as cp


def read_img(path):
    out = []
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            raise SystemExit("%s is not a VER2 archive" % path)
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
        for at in range(0, 32 * count, 32):
            offset, sectors, _ = struct.unpack_from("<IHH", table, at)
            name = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
            f.seek(offset * ic.SECTOR)
            out.append((name, f.read(sectors * ic.SECTOR)))
    return out


def profile(blob):
    """The facts about a ped clump that a converted one and a stock one should share."""
    version, kids = cp.clump_children(blob)
    counts = struct.unpack_from("<III", blob, kids[0][2])
    frames = header = geometry = None
    atomics = []
    for k, _v, cs, ce in kids:
        if k == ic.RW_FRAMELIST:
            frames, header = cp.read_frames(blob, cs, ce)
        elif k == ic.RW_GEOMETRYLIST:
            gk = ic.rw_children(blob, cs, ce, "geometry list")
            geometry = cp.read_geometry(blob, gk[1][2], gk[1][3])
        elif k == ic.RW_ATOMIC:
            atomics.append(struct.unpack_from("<I", blob, ic.rw_children(
                blob, cs, ce, "atomic")[0][2])[0])
    skin = cp.read_skin(blob, geometry["skin"][0], geometry["skin"][1], geometry["skin"][2],
                        geometry["num_vert"])
    world = {}
    for bone, _n, parent in rig.SA_BONES:
        f = frames[rig.SA_FRAME_INDEX[bone]]
        world[bone] = f["local"] if parent is None else rig.mat_mul(f["local"], world[parent])
    return dict(version=version, counts=counts, frames=len(frames), atomics=atomics,
                names=[f["name"] for f in frames], parents=[f["parent"] for f in frames],
                ids=[f["hanim_id"] for f in frames],
                nodes=[n[0] for n in header["nodes"]] if header else None,
                key_size=header["key_size"] if header else None,
                num_bones=skin["num_bones"], stride=skin["stride"], used=skin["used"],
                max_weights=skin["max_weights"], num_vert=geometry["num_vert"],
                num_tri=geometry["num_tri"], world=world, skin=skin, geometry=geometry)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    region, sa = sys.argv[1], sys.argv[2]
    img = os.path.join(region, "peds.img")
    if not os.path.isfile(img):
        sys.exit("no %s" % img)

    reference = cp.read_from_img_v2(ic.find_file(sa, "models/gta3.img"), "bfori.dff")
    want = profile(reference)
    print("San Andreas' bfori.dff: %d frames, %d bones, stride %d, %d used, keyframe %d"
          % (want["frames"], want["num_bones"], want["stride"], len(want["used"]),
             want["key_size"]))

    checked = failed = 0
    complaints = []
    for name, blob in read_img(img):
        if not name.lower().endswith(".dff"):
            continue
        checked += 1
        try:
            got = profile(blob)
        except Exception as err:                                  # noqa: BLE001 - report anything
            failed += 1
            complaints.append("%s: will not parse: %s: %s" % (name, type(err).__name__, err))
            continue
        bad = []
        for field in ("version", "counts", "frames", "names", "parents", "ids", "nodes",
                      "key_size", "num_bones", "stride"):
            if got[field] != want[field]:
                bad.append("%s is %r, San Andreas' is %r"
                           % (field, got[field], want[field]))
        # San Andreas is not of one mind about which frame the atomic hangs on: 258 of its 265 peds
        # use frame 2, the Pelvis, and the other seven - `bfori` among them - use frame 1, the
        # hierarchy root.  Both ship and both work, which is itself the evidence that a skinned
        # atomic's own frame does not enter the skinning, so either is accepted here.
        if any(a not in (1, 2) for a in got["atomics"]) or len(got["atomics"]) != 1:
            bad.append("atomic frames %s; San Andreas' peds use frame 1 or 2, once"
                       % (got["atomics"],))
        if not got["used"]:
            bad.append("no used-bone list")
        if got["max_weights"] < 1 or got["max_weights"] > 4:
            bad.append("maxWeightsPerVertex %d" % got["max_weights"])
        # `skinToBone[i] * frameWorld[i]` is the change of basis between the space the vertices are
        # stored in and the space the frames are posed in.  It is a property of the model, so it has
        # to be the same matrix at every bone - though not necessarily the identity, since San
        # Andreas' own peds have an axis permutation there.  A node array in the wrong order shows
        # up here and almost nowhere else.
        changes = [(bone, rig.mat_mul(got["skin"]["matrices"][i], got["world"][bone]))
                   for i, (bone, _flags) in enumerate(rig.SA_NODES)]
        reference_change = changes[0][1]
        worst, where = 0.0, None
        for bone, change in changes:
            err = max(max(abs(a - b) for a, b in zip(change[0], reference_change[0])),
                      max(abs(a - b) for a, b in zip(change[1], reference_change[1])))
            if err > worst:
                worst, where = err, rig.SA_NAME[bone]
        if worst > 1e-3:
            bad.append("skin and frame list disagree about the basis by %.5f at %s" % (worst, where))
        for bone, _n, _p in rig.SA_BONES:
            err = rig.mat_orthonormality(got["world"][bone])
            if err > 1e-3:
                bad.append("%s world matrix %.5f off orthonormal" % (rig.SA_NAME[bone], err))
        # Every bone a vertex names must be one the hierarchy holds and one the used list names.
        named = set()
        for v in range(got["num_vert"]):
            for c in range(4):
                if got["skin"]["weights"][v][c] > 0.0:
                    named.add(got["skin"]["indices"][v][c])
        if named - set(got["used"]):
            bad.append("vertices name bones outside the used list: %s"
                       % sorted(named - set(got["used"])))
        if max(named, default=0) >= len(rig.SA_NODES):
            bad.append("a vertex names node %d of %d" % (max(named), len(rig.SA_NODES)))
        blocks = cp.geometry_blocks(blob, got["geometry"], got["version"])
        verts = blocks["morphs"][0]["verts"]
        span = tuple(max(v[i] for v in verts) - min(v[i] for v in verts) for i in range(3))
        if not 1.2 < span[1] < 2.6:
            bad.append("is %.2f tall" % span[1])
        # San Andreas' own peds put the foot bone 0.746 to 0.953 below the model origin; a ped
        # outside that band by much would stand off the ground.
        foot = (got["world"][43][1][1] + got["world"][53][1][1]) / 2.0
        if not -1.05 < foot < -0.65:
            bad.append("feet %.3f below the origin, outside San Andreas' 0.746..0.953" % -foot)
        if bad:
            failed += 1
            complaints.append("%s: %s" % (name, "; ".join(bad)))

    print("%d model(s) checked, %d with something to say" % (checked, failed))
    for c in complaints:
        print("  " + c)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
