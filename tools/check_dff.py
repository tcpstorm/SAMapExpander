#!/usr/bin/env python3
"""Check the models of a region archive against the ones they were converted from.

    check_dff.py <region .img> [<source game dir>]

Every model import_city.py packs has been read apart and written back out as San Andreas
RenderWare (see convert_dff), and this is what says that came out right. On the archive alone it
checks that

    * every entry that ought to be a clump is one, and its top chunk carries 0x1803FFFF
    * every chunk header below it carries 0x1803FFFF too
    * the whole tree parses exactly - no chunk overruns its parent, no parent has bytes left over
    * every geometry struct accounts for its own length under the 3.4-onwards layout, which is
      the layout San Andreas reads, and its texture-coordinate-set count is in the flags' third
      byte where San Andreas looks for it
    * every clump, atomic, geometry, material and texture ends in an extension chunk, and a frame
      list carries one per frame

and, given the source game directory as well, that the conversion changed nothing it should not:
the triangle, vertex and material counts of every geometry are held against the same model in the
source gta3.img, matched up through the region's names.txt.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from import_city import (RW_ATOMIC, RW_CLUMP, RW_EXTENSION, RW_FRAMELIST, RW_GEOMETRY,
                         RW_GEOMETRYLIST, RW_MATERIAL, RW_MATLIST, RW_STRUCT, RW_TEXTURE, RW_34,
                         GEOM_NATIVE, GEOM_PRELIT, GEOM_TEXTURED, GEOM_TEXTURED2, SA_LIB_ID,
                         RwError, dff_geometry_counts, find_file, read_img_v1, rw_children,
                         rw_version)

SECTOR = 2048
NEEDS_EXTENSION = (RW_CLUMP, RW_ATOMIC, RW_GEOMETRY, RW_MATERIAL, RW_TEXTURE)
CONTAINERS = (RW_CLUMP, RW_FRAMELIST, RW_GEOMETRYLIST, RW_GEOMETRY, RW_MATLIST, RW_MATERIAL,
              RW_TEXTURE, RW_ATOMIC, RW_EXTENSION)


def read_img_v2(path):
    """[(name, offset, size)] of a San Andreas VER2 archive."""
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            sys.exit("%s is not a VER2 archive" % path)
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
    out = []
    for at in range(0, 32 * count, 32):
        offset, sectors, alt = struct.unpack_from("<IHH", table, at)
        name = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
        out.append((name, offset * SECTOR, (sectors or alt) * SECTOR))
    return out


def check_clump(data, name, problems):
    """Walk one converted model, recording everything wrong with it in `problems`."""
    kind, size, library_id = struct.unpack_from("<III", data, 0)
    if kind != RW_CLUMP:
        problems.append("%s: top chunk is %#x, not a clump" % (name, kind))
        return
    if library_id != SA_LIB_ID:
        problems.append("%s: clump stamped %08X, not %08X" % (name, library_id, SA_LIB_ID))

    def walk(at, end, what, parent):
        for child, version, cs, ce in rw_children(data, at, end, what):
            stamp = struct.unpack_from("<I", data, cs - 4)[0]
            if stamp != SA_LIB_ID:
                problems.append("%s: %s holds a %#x chunk stamped %08X" % (name, what, child, stamp))
            if child == RW_STRUCT and parent == RW_GEOMETRY:
                check_geometry_struct(cs, ce, what)
            if child in CONTAINERS:
                walk(cs, ce, "%#x" % child, child)
        if parent in NEEDS_EXTENSION:
            kinds = [c for c, _v, _s, _e in rw_children(data, at, end, what)]
            if RW_EXTENSION not in kinds:
                problems.append("%s: a %#x chunk has no extension" % (name, parent))
        if parent == RW_FRAMELIST:
            kids = rw_children(data, at, end, what)
            frames = struct.unpack_from("<i", data, kids[0][2])[0]
            got = sum(1 for c, _v, _s, _e in kids if c == RW_EXTENSION)
            if got != frames:
                problems.append("%s: frame list declares %d frame(s) but carries %d extension(s)"
                                % (name, frames, got))

    def check_geometry_struct(s, e, what):
        """The same accounting convert_geometry_struct does, but only for the 3.4+ layout, which
        is the one San Andreas reads. A struct that does not add up here would not be instanced."""
        flags, num_tri, num_vert, num_morph = struct.unpack_from("<IiiI", data, s)
        sets = (flags >> 16) & 0xFF
        implied = 1 if flags & GEOM_TEXTURED else (2 if flags & GEOM_TEXTURED2 else 0)
        if sets != implied:
            problems.append("%s: geometry flags %08X carry %d texture set(s), the flags imply %d"
                            % (name, flags, sets, implied))
        at = s + 16
        if not flags & GEOM_NATIVE:
            at += (4 * num_vert if flags & GEOM_PRELIT else 0) + sets * 8 * num_vert + 8 * num_tri
        for _ in range(num_morph):
            at += 16
            has_vertices, has_normals = struct.unpack_from("<ii", data, at)
            at += 8
            if not flags & GEOM_NATIVE:
                at += (12 * num_vert if has_vertices else 0) + (12 * num_vert if has_normals else 0)
        if at != e:
            problems.append("%s: geometry struct is %d bytes, the 3.6 layout accounts for %d"
                            % (name, e - s, at - s))

    try:
        walk(12, 12 + size, "clump", RW_CLUMP)
    except RwError as err:
        problems.append("%s: %s" % (name, err))


def source_models(src):
    """{lowercase name: bytes} for every .dff of a source game's gta3.img."""
    img = find_file(src, "models/gta3.img")
    dirf = find_file(src, "models/gta3.dir")
    if not img or not dirf:
        sys.exit("models/gta3.img + gta3.dir not found under %s" % src)
    with open(img, "rb") as f:
        archive = f.read()
    out = {}
    for name, offset, size in read_img_v1(img, dirf):
        if name.lower().endswith(".dff") and size and name.lower() not in out:
            out[name.lower()] = archive[offset:offset + size]
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    img = sys.argv[1]
    src = sys.argv[2] if len(sys.argv) > 2 else None
    entries = read_img_v2(img)
    with open(img, "rb") as f:
        archive = f.read()

    originals = {}
    names = os.path.join(os.path.dirname(os.path.abspath(img)), "names.txt")
    if os.path.isfile(names):
        for line in open(names, encoding="latin-1"):
            if line.startswith("#"):
                continue
            field = line.rstrip("\n").split("\t")
            if len(field) >= 2:
                originals[field[0].lower()] = field[1].lower()

    problems, models, versions = [], 0, {}
    for name, offset, size in entries:
        if not name.lower().endswith(".dff"):
            continue
        models += 1
        data = archive[offset:offset + size]
        versions[struct.unpack_from("<I", data, 8)[0]] = \
            versions.get(struct.unpack_from("<I", data, 8)[0], 0) + 1
        check_clump(data, name, problems)

    print("%s: %d model(s)" % (os.path.basename(img), models))
    for library_id, count in sorted(versions.items(), key=lambda kv: -kv[1]):
        version = rw_version(library_id)
        print("   %08X x%-5d RW %X.%X.%X.%X%s"
              % (library_id, count, (version >> 16) & 0xF, (version >> 12) & 0xF,
                 (version >> 8) & 0xF, version & 0xF,
                 "" if library_id == SA_LIB_ID else "   <- NOT San Andreas'"))

    if src:
        pool = source_models(src)
        checked, unmatched = 0, 0
        for name, offset, size in entries:
            if not name.lower().endswith(".dff"):
                continue
            original = originals.get(name.lower(), name.lower())
            if original not in pool:
                unmatched += 1
                continue
            try:
                was = dff_geometry_counts(pool[original])
                now = dff_geometry_counts(archive[offset:offset + size])
            except (RwError, struct.error) as err:
                problems.append("%s: counting geometries: %s" % (name, err))
                continue
            if was != now:
                problems.append("%s (from %s): geometries were %s, are now %s"
                                % (name, original, was, now))
            checked += 1
        print("counts held against %s: %d model(s) matched by name, %d not found in the source"
              % (src, checked, unmatched))

    print("problems: %d" % len(problems))
    for p in problems[:40]:
        print("   %s" % p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
