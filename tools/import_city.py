#!/usr/bin/env python3
"""Turn a GTA III / Vice City install into a MapExpander region folder.

    import_city.py <source game dir> <SA game dir> [--region=vicecity] [--lite] [--no-rename]

Writes <SA game dir>/MapExpander/<region>/ with

    <region>.img   every .dff/.txd from the source gta3.img, repacked as an SA "VER2" archive
    col/           every .col from the archive plus the COLFILE entries of the source .dat
    ide/, ipl/     the IDE and IPL files the source .dat lists, in its order
    tiles/         radar00.png .. radarNN.png decoded from the radar TXDs
    american.gxt, waterpro.dat, and the source's zone file (navig.zon/info.zon, or III's gta3.zon)
    region.ini     the [region] keys to paste into MapExpander.ini, lists in .dat order, with the
                   origin the conversion this project follows places that city at

The dictionaries a source .dat loads from disk rather than out of the archive - III's
models/generic.txd and models/misc.txd, named by TEXDICTION in data/default.dat - travel with the
region too: 608 of III's models are bound to `generic`, which is also the name of a San Andreas
dictionary, so it has to be packed and renamed like everything else.

--lite writes only the last three groups (tiles, text/water/zones, region.ini): what a region
needs when something else - a server, a map mod - already places the buildings.

Every model and texture dictionary is renamed under a two-character tag for the source game
("v_" for Vice City, "l_" for III) - in the archive, the IDE model/txd columns, the IPL inst
lines and the collision entry names, all together - and names.txt records what each was called.
A name is kept readable when it fits the game's 19-character field and takes a short hashed
tail when it does not; every name is checked to be unique, and to share no CKeyGen key with
anything San Andreas has, before the archive is written. See docs/asset-namespacing.md.
--no-rename skips all of it, which is only safe for a source that clashes with nothing.
Runs on plain Python; Pillow is used for the PNGs if it is installed, else a built-in writer.
"""
import os
import re
import struct
import sys
import zlib

SECTOR = 2048


# ---------------------------------------------------------------- IMG archives

def read_img_v1(img_path, dir_path):
    """Returns [(name, offset_bytes, size_bytes)] from a III/VC gta3.dir."""
    entries = []
    with open(dir_path, "rb") as f:
        data = f.read()
    for at in range(0, len(data) - 31, 32):
        off, size = struct.unpack_from("<II", data, at)
        name = data[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
        if name:
            entries.append((name, off * SECTOR, size * SECTOR))
    return entries


def read_img_v2_names(path):
    """Names in an SA archive (VER2), or an empty set if it is not there."""
    names = set()
    if not os.path.isfile(path):
        return names
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            return names
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
    for at in range(0, 32 * count, 32):
        name = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1")
        if name:
            names.add(name.lower())
    return names


def write_img_v2(path, files):
    """files: [(name, bytes)]. Writes a VER2 archive, every entry sector aligned."""
    count = len(files)
    header = 8 + 32 * count
    data_start = (header + SECTOR - 1) // SECTOR * SECTOR
    with open(path, "wb") as f:
        f.write(b"VER2" + struct.pack("<I", count))
        pos = data_start
        table = bytearray()
        placed = []
        for name, blob in files:
            sectors = (len(blob) + SECTOR - 1) // SECTOR
            table += struct.pack("<IHH", pos // SECTOR, sectors, 0)
            table += name.encode("latin-1")[:23].ljust(24, b"\0")
            placed.append((pos, blob, sectors))
            pos += sectors * SECTOR
        f.write(table)
        f.write(b"\0" * (data_start - header))
        for pos, blob, sectors in placed:
            f.write(blob)
            f.write(b"\0" * (sectors * SECTOR - len(blob)))
    return pos


# ---------------------------------------------------------------- RenderWare TXD -> PNG

def rw_sections(data, at, end):
    """Yields (type, body_start, body_end) for the RW sections between at and end."""
    while at + 12 <= end:
        kind, size, _ver = struct.unpack_from("<III", data, at)
        yield kind, at + 12, min(at + 12 + size, end)
        at += 12 + size


def rgb565(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31


def decode_dxt(data, at, width, height, dxt3):
    """DXT1 (dxt3=False) or DXT3 into a flat RGBA bytearray."""
    out = bytearray(width * height * 4)
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    for by in range(bh):
        for bx in range(bw):
            alphas = None
            if dxt3:
                bits = struct.unpack_from("<Q", data, at)[0]
                at += 8
                alphas = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
            c0, c1, idx = struct.unpack_from("<HHI", data, at)
            at += 8
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1 or dxt3:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x >= width or y >= height:
                    continue
                r, g, b, a = pal[(idx >> (2 * i)) & 3]
                if alphas is not None:
                    a = alphas[i]
                o = (y * width + x) * 4
                out[o:o + 4] = bytes((r, g, b, a))
    return out


def decode_raw(data, at, width, height, depth, fmt, palette):
    out = bytearray(width * height * 4)
    n = width * height
    if palette is not None:
        for i in range(n):
            r, g, b, a = palette[data[at + i]]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return out
    base = fmt & 0x0F00
    for i in range(n):
        o = i * 4
        if depth == 32:
            b, g, r, a = data[at + i * 4:at + i * 4 + 4]
            if base == 0x0600:   # 888: alpha byte is padding
                a = 255
        elif depth == 24:
            b, g, r = data[at + i * 3:at + i * 3 + 3]
            a = 255
        elif depth == 16:
            v = struct.unpack_from("<H", data, at + i * 2)[0]
            if base == 0x0200:      # 565
                r, g, b = rgb565(v)
                a = 255
            elif base == 0x0300:    # 4444
                r, g, b, a = [((v >> s) & 15) * 17 for s in (8, 4, 0, 12)]
            else:                   # 1555
                r = ((v >> 10) & 31) * 255 // 31
                g = ((v >> 5) & 31) * 255 // 31
                b = (v & 31) * 255 // 31
                a = 255 if v & 0x8000 else 0
        else:
            raise ValueError("unsupported raster depth %d" % depth)
        out[o:o + 4] = bytes((r, g, b, a))
    return out


def txd_first_texture(data):
    """(width, height, rgba bytes) of the first texture in a TXD, or None."""
    for kind, s, e in rw_sections(data, 0, len(data)):
        if kind != 0x16:
            continue
        for k2, s2, e2 in rw_sections(data, s, e):
            if k2 != 0x15:
                continue
            for k3, s3, e3 in rw_sections(data, s2, e2):
                if k3 != 0x01:
                    continue
                platform = struct.unpack_from("<I", data, s3)[0]
                fmt, d3d_or_alpha, width, height, depth, mips, rtype, flag = \
                    struct.unpack_from("<IIHHBBBB", data, s3 + 72)
                at = s3 + 88
                palette = None
                if fmt & 0x2000:    # PAL8
                    palette = [tuple(data[at + i * 4:at + i * 4 + 4]) for i in range(256)]
                    at += 1024
                elif fmt & 0x4000:  # PAL4
                    palette = [tuple(data[at + i * 4:at + i * 4 + 4]) for i in range(16)]
                    at += 64
                size = struct.unpack_from("<I", data, at)[0]
                at += 4
                if platform == 9:   # D3D9: d3d_or_alpha is a D3DFORMAT, a FOURCC when flag & 8
                    fourcc = struct.pack("<I", d3d_or_alpha)
                    if flag & 0x08 and fourcc in (b"DXT1", b"DXT3"):
                        return width, height, decode_dxt(data, at, width, height, fourcc == b"DXT3")
                elif flag in (1, 3):   # D3D8: dxtCompression
                    return width, height, decode_dxt(data, at, width, height, flag == 3)
                if palette is not None and fmt & 0x4000:
                    raise ValueError("PAL4 rasters are not supported")
                return width, height, decode_raw(data, at, width, height, depth, fmt, palette)
    return None


# ---------------------------------------------------------------- TXD -> San Andreas

# San Andreas' raster reader is _rwD3D9NativeTextureRead and it only accepts platform 9. Vice City
# and GTA III write platform 8 (D3D8), so their textures are silently rejected and every model that
# uses them renders white - the geometry loads perfectly, which makes it look like a texture path
# problem rather than a format one. The two native-texture structs are the same 88-byte layout and
# the pixel data is identical; only three fields mean anything different:
#
#     offset 0    platformId          8 -> 9
#     offset 76   hasAlpha            -> d3dFormat (a D3DFORMAT, or a FOURCC when compressed)
#     offset 87   dxtCompression      -> flags: bit0 alpha, bit1 cube, bit2 automipmaps, bit3 compressed
#
# so this rewrites the headers and copies the levels through untouched, rather than decoding and
# re-encoding every texture. Palettised rasters are the exception - see expand_palette.
SA_LIB_ID = 0x1803FFFF          # RenderWare 3.6.0.3, the version San Andreas writes
RW_STRUCT, RW_EXTENSION, RW_TEXTURE_NATIVE, RW_TEX_DICTIONARY = 0x01, 0x03, 0x15, 0x16

# rasterFormat (masked with 0x0F00) -> D3DFORMAT, for the uncompressed cases.
D3D_FORMATS = {
    0x0100: 25,   # 1555 -> A1R5G5B5
    0x0200: 23,   # 565  -> R5G6B5
    0x0300: 26,   # 4444 -> A4R4G4B4
    0x0400: 50,   # LUM8 -> L8
    0x0500: 21,   # 8888 -> A8R8G8B8
    0x0600: 22,   # 888  -> X8R8G8B8
    0x0A00: 24,   # 555  -> X1R5G5B5
}


RW_STRING = 0x02
RW_TEXTURE = 0x06
# Clump, GeometryList, Geometry, MaterialList, Material, Atomic, Extension, TextureDictionary:
# the chunks that can hold a material tree, so the walker knows what to descend into.
RW_CONTAINERS = (0x10, 0x1A, 0x0F, 0x08, 0x07, 0x14, 0x03, 0x16)


def rw_chunk(kind, body):
    return struct.pack("<III", kind, len(body), SA_LIB_ID) + body


def expand_palette(body, levels):
    """The levels of a PAL8 native texture, as <u32 size><data> blocks of 32-bit pixels."""
    at = 88
    lut = [bytes((body[at + i * 4 + 2], body[at + i * 4 + 1], body[at + i * 4], body[at + i * 4 + 3]))
           for i in range(256)]         # RenderWare keeps palettes in RGBA, Direct3D wants BGRA
    at += 1024
    out = []
    for _ in range(levels):
        size = struct.unpack_from("<I", body, at)[0]
        at += 4
        pixels = b"".join(map(lut.__getitem__, body[at:at + size]))
        at += size
        out.append(struct.pack("<I", len(pixels)) + pixels)
    return b"".join(out)


def convert_texture_native(body):
    """A D3D8 native-texture struct body rewritten as D3D9, or None if it cannot be."""
    platform = struct.unpack_from("<I", body, 0)[0]
    if platform == 9:
        return body                      # already San Andreas' format
    if platform != 8 or len(body) < 88:
        return None                      # PS2/Xbox rasters carry different pixel data entirely

    raster_format, has_alpha = struct.unpack_from("<II", body, 72)
    depth, levels = body[84], body[85]
    tail, dxt = body[88:], body[87]
    if dxt > 5:
        return None                      # nothing else is a D3D compression scheme
    if dxt:
        d3d_format = struct.unpack("<I", b"DXT%d" % dxt)[0]
        flags = 0x08 | (1 if (has_alpha or dxt != 1) else 0)
    elif raster_format & 0x4000:
        return None                      # PAL4; neither III nor Vice City has one
    elif raster_format & 0x2000:
        # _rwD3D9NativeTextureRead re-reads the raster format off the raster RwRasterCreate handed
        # it back and drops the texture when the two disagree - and a dropped texture takes the
        # whole dictionary down with it, not just itself. Direct3D 9 has no palettised texture for
        # the two to agree on, which is why San Andreas ships none at all, so unpack ours here.
        tail = expand_palette(body, levels)
        raster_format = (raster_format & 0x8000) | (0x0500 if has_alpha else 0x0600)
        d3d_format = 21 if has_alpha else 22
        depth, flags = 32, (1 if has_alpha else 0)
    else:
        d3d_format = D3D_FORMATS.get(raster_format & 0x0F00, 21 if depth == 32 else 22)
        flags = 1 if has_alpha else 0

    out = bytearray(body[:88])
    struct.pack_into("<I", out, 0, 9)
    struct.pack_into("<II", out, 72, raster_format, d3d_format)
    out[84] = depth
    out[87] = flags
    return bytes(out) + tail


def txd_texture_chunks(data):
    """Every texture in a III/VC TXD, converted to San Andreas' D3D9 form.

    Returns (chunks, converted, failed), where chunks is a list of (texture name, chunk bytes) in
    the order the dictionary held them, or (None, 0, 0) if the blob is not a texture dictionary.
    Kept separate from building the dictionary back up so that two files carrying the same name can
    be merged into one - see the packing loop in main().
    """
    dictionary = None
    for kind, s, e in rw_sections(data, 0, len(data)):
        if kind == RW_TEX_DICTIONARY:
            dictionary = (s, e)
            break
    if dictionary is None:
        return None, 0, 0

    textures, converted, failed = [], 0, 0
    for kind, s, e in rw_sections(data, *dictionary):
        if kind != RW_TEXTURE_NATIVE:
            continue
        for k2, s2, e2 in rw_sections(data, s, e):
            if k2 != RW_STRUCT:
                continue
            body = convert_texture_native(data[s2:e2])
            if body is None:
                failed += 1
            else:
                converted += 1
                name = body[8:40].split(b"\0")[0].decode("ascii", "replace")
                # An empty extension: whatever III/VC hung off the texture means nothing here.
                textures.append((name, rw_chunk(RW_TEXTURE_NATIVE,
                                                rw_chunk(RW_STRUCT, body) + rw_chunk(RW_EXTENSION, b""))))
            break

    return textures, converted, failed


def dff_texture_names(data):
    """Texture names a model's materials reference.

    Walks the clump for Texture chunks and takes the first String child of each, which is the
    texture name (the second is the alpha mask). Used to work out which textures a dictionary is
    actually asked for, so the ones it is missing can be copied in.
    """
    found = []

    def sections(off, end):
        while off + 12 <= end:
            cid, csz, _ver = struct.unpack_from("<III", data, off)
            if csz < 0 or off + 12 + csz > end:
                return
            yield cid, off + 12, off + 12 + csz
            off = off + 12 + csz

    def walk(s, e, in_texture):
        for cid, cs, ce in sections(s, e):
            if cid == RW_TEXTURE:
                walk(cs, ce, True)
            elif cid == RW_STRING and in_texture:
                name = data[cs:ce].split(bytes([0]))[0].decode("ascii", "replace")
                if name:
                    found.append(name)
                return                       # the first string is the name, the second the mask
            elif cid in RW_CONTAINERS:
                walk(cs, ce, in_texture)

    walk(0, len(data), False)
    return found


def build_txd(chunks):
    """A San Andreas texture dictionary holding the given (name, chunk bytes) textures."""
    header = rw_chunk(RW_STRUCT, struct.pack("<HH", len(chunks), 2))   # deviceId 2 = D3D9
    body = header + b"".join(c for _name, c in chunks) + rw_chunk(RW_EXTENSION, b"")
    return rw_chunk(RW_TEX_DICTIONARY, body)


def convert_txd(data):
    """A III/VC TXD rebuilt for San Andreas. Returns (bytes, converted, failed)."""
    chunks, converted, failed = txd_texture_chunks(data)
    if not chunks:
        return data, converted, failed
    return build_txd(chunks), converted, failed


# ---------------------------------------------------------------- DFF -> San Andreas
#
# Converting the textures is only half of it: San Andreas cannot read a Liberty City model either.
# Every chunk of a RenderWare stream carries the library id of the version that wrote it, and
# RenderWare's readers branch on that id, so the same bytes mean different things to GTA III's
# 3.1/3.2 and to San Andreas' 3.6.0.3. Three of those branches decide whether a map model draws:
#
#   * Below 3.4.0.0 an RpGeometry's struct carries twelve bytes of surface properties - ambient,
#     specular, diffuse - directly after its four counts. From 3.4 those live on the material
#     instead and the geometry no longer has them, so San Andreas does not look for them and every
#     field behind them is read twelve bytes early: the prelit colours come back as vertex
#     positions and the triangle list runs off the end. That is the whole of Liberty City - all
#     3134 of its clumps and all 6050 of its geometries are older than 3.4 - and 1919 of Vice
#     City's geometries besides. A version stamp on its own cannot fix this one; the struct has to
#     be re-laid, which is what convert_geometry_struct does and why this is not a header patch.
#   * From 3.4 the number of texture-coordinate sets is carried in the geometry flags' third byte,
#     (flags >> 16) & 0xFF. Before that it was implied by rpGEOMETRYTEXTURED (one set) and
#     rpGEOMETRYTEXTURED2 (two). Both readers fall back to the flags when the byte is zero, so this
#     one is survivable, but 678 of Liberty City's geometries lean on that fallback and the byte is
#     what San Andreas writes for every one of its own.
#   * Above 3.3.0.0 a clump's struct is numAtomics, numLights and numCameras rather than just
#     numAtomics. All 3134 of GTA III's are the four-byte form, and five of Vice City's.
#
# The rebuild fixes something else on the way past. 570 of GTA III's clumps carry a size in their
# header that stops short of their own contents - 538 just before the extension chunk and 32 by up
# to 180 bytes, in the middle of the last atomic. RenderWare never notices, because it reads a
# clump by the counts in its struct rather than by that size, so this reads them the same way and
# writes the length back out correctly. It also gives every clump, atomic, geometry, material,
# texture and frame the extension chunk RenderWare closes an object by reading, empty where the
# object has nothing to hang off it.
#
# So this reads the clump tree apart and writes it back out in San Andreas' form, stamping
# 0x1803FFFF on every header on the way through, exactly as convert_txd does for a dictionary.
# Everything that is version-independent - the frame matrices, the vertices, normals, UVs, prelit
# colours and triangles, the bin mesh, the material colours and the texture names - travels
# through byte for byte, and the counts are checked against the source afterwards.

RW_CLUMP, RW_FRAMELIST, RW_GEOMETRY, RW_GEOMETRYLIST = 0x10, 0x0E, 0x0F, 0x1A
RW_MATLIST, RW_MATERIAL, RW_ATOMIC, RW_MATFX = 0x08, 0x07, 0x14, 0x120
RW_LIGHT, RW_CAMERA = 0x12, 0x05

RW_34 = 0x34000           # the version at which the geometry stream format changed
RW_304 = 0x30400          # ... and at which a material grew surface properties of its own

# rpGeometryFlag: the bits of the geometry struct's first word that change how the rest is read.
GEOM_TEXTURED, GEOM_PRELIT, GEOM_TEXTURED2, GEOM_NATIVE = 0x04, 0x08, 0x80, 0x01000000
GEOM_TRISTRIP = 0x01      # the geometry's own "my meshes are strips" flag
RW_BINMESH = 0x50E        # the mesh split the D3D pipeline instances from, one mesh per material

# How far to lift a vertex's baked lighting towards white: out = in + (255 - in) * PRELIT_LIFT.
#
# III and Vice City light the map twice. The colours baked into a geometry are only part of it -
# ambient light from the time cycle is added on top at run time - so a surface that reads almost
# black in the file still comes out lit in the game. San Andreas does not do that: for map models
# the baked colour is the whole of the vertex lighting, modulating the texture directly. Imported
# straight across, Vice City's roads therefore render at a fraction of their brightness, and the
# black vertices among them - up to 47 per cent on the mainland - drag dark patches across the
# tarmac. That is the blotchy, unlit look the streets have.
#
# Measured, the gap is plain. San Andreas' own road models average a vertex luma of 92 to 115;
# Vice City's mainland roads average 20 to 45. One model the two games share, nt_roadblockci,
# comes out at 112.0 in San Andreas and 112.1 here, which says the conversion carries the colours
# across exactly and that the difference is in what each engine does with them afterwards.
#
# Lifting towards white rather than adding a constant keeps the ordering intact and moves the dark
# end furthest, which is where the missing ambient went: at 0.30 a road at 25 comes out at 94 and
# one already at 100 only reaches 146, so the baked shading survives instead of being flattened.
PRELIT_LIFT = 0.30

# Plugin chunks GTA III and Vice City hang off their models that no San Andreas model carries:
# 0x105, the morph plugin, on 2950 of III's geometries and 6118 of Vice City's, and 0x118 on 602
# of III's atomics. Neither id appears once across the 16264 geometries and 12953 clumps of San
# Andreas' own gta3.img, so San Andreas has nothing registered to read them and they could only
# ever be skipped. Everything else is kept: the bin mesh (0x50E) San Andreas instances from, the
# material effects (0x120), the skin (0x116) and animation hierarchy (0x11E), the frame names
# (0x253F2FE), the right to render (0x1F) and the sky mipmap value (0x110) - San Andreas ships
# models carrying every one of those. The one that would have to go, 2dfx (0x253F2F8), whose
# entries are laid out per game, appears in neither source's models at all.
DROP_PLUGINS = {0x105, 0x118}

MATFX_BUMPMAP = 1


class RwError(Exception):
    """A clump this cannot make sense of. The importer passes the model through untouched."""


def rw_version(library_id):
    """The RenderWare version a chunk header's library id encodes.

    0x1803FFFF -> 0x36003 (3.6.0.3) and 0x0800FFFF -> 0x32000 (3.2.0.0) in the packed form used
    from 3.2 on, and 0x00000310 -> 0x31000 (3.1.0.0) in the shifted form that came before it.
    """
    if library_id & 0xFFFF0000:
        return (((library_id >> 14) & 0x3FF00) + 0x30000) | ((library_id >> 16) & 0x3F)
    return library_id << 8


def rw_children(data, at, end, what):
    """[(kind, version, body start, body end)] for the chunks that exactly fill at..end.

    Exactly is the point. A chunk that overruns its parent, or a parent with bytes left over after
    its last child, means the layout being assumed is the wrong one, and it is far better to leave
    such a model alone than to write out a guess at it.
    """
    out = []
    while at + 12 <= end:
        kind, size, library_id = struct.unpack_from("<III", data, at)
        if size > end - at - 12:
            raise RwError("%s: a %#x chunk claims %d bytes with %d left"
                          % (what, kind, size, end - at - 12))
        out.append((kind, rw_version(library_id), at + 12, at + 12 + size))
        at += 12 + size
    if at != end:
        raise RwError("%s: %d byte(s) after the last chunk" % (what, end - at))
    return out


def rw_chunk_at(data, at, limit, what, want=None):
    """The one chunk starting at `at`, as (kind, version, body start, body end).

    The counterpart to rw_children for the places RenderWare reads a stream by what it is expecting
    rather than by a length - see convert_clump.
    """
    if at + 12 > limit:
        raise RwError("%s: nothing left to read it from" % what)
    kind, size, library_id = struct.unpack_from("<III", data, at)
    if size > limit - at - 12:
        raise RwError("%s: a %#x chunk claims %d bytes with %d left"
                      % (what, kind, size, limit - at - 12))
    if want is not None and kind != want:
        raise RwError("%s: found a %#x chunk, expected %#x" % (what, kind, want))
    return kind, rw_version(library_id), at + 12, at + 12 + size


def rebuild(kind, children, extension):
    """A container chunk from its already-converted children, with `extension` last.

    RenderWare finishes reading every object by reading an extension chunk, so one is written even
    where the source had none - see the note above.
    """
    body = b"".join(children) + (extension if extension is not None else rw_chunk(RW_EXTENSION, b""))
    return rw_chunk(kind, body)


def convert_matfx(data, s, e):
    """A material effects plugin, with the texture chunks inside it re-stamped.

    The payload is the enabled-effects word and then two effect slots, each a type word followed by
    that type's own fields: a coefficient for a bump map and then a flag and a texture chunk for
    each of the bump and height maps; a coefficient and a framebuffer-alpha flag for an environment
    map, or the two blend modes for a dual pass, either followed by one flag and texture chunk.
    Vice City writes 2960 of these, every one an environment map, and GTA III ten - seven
    environment maps and three bump maps. The walk is checked against the chunk's length, and
    anything that does not add up is refused rather than rewritten. The textures nested in here are
    the only chunk headers in a clump that walking children does not reach, which is the whole
    reason this function exists: without it every one of them would go out still carrying the
    source game's library id.
    """
    out, at = [data[s:s + 4]], s + 4
    for _slot in range(2):
        kind = struct.unpack_from("<I", data, at)[0]
        out.append(data[at:at + 4])
        at += 4
        if kind == 0:
            continue
        fields = 4 if kind == MATFX_BUMPMAP else 8
        out.append(data[at:at + fields])
        at += fields
        for _ in range(2 if kind == MATFX_BUMPMAP else 1):
            present = struct.unpack_from("<I", data, at)[0]
            out.append(data[at:at + 4])
            at += 4
            if not present:
                continue
            tkind, tsize, _lib = struct.unpack_from("<III", data, at)
            if tkind != RW_TEXTURE or at + 12 + tsize > e:
                raise RwError("material effects: no texture where one is flagged")
            out.append(convert_texture(data, at + 12, at + 12 + tsize))
            at += 12 + tsize
    if at != e:
        raise RwError("material effects: %d bytes, the layout accounts for %d" % (e - s, at - s))
    return b"".join(out)


def convert_extension(data, s, e, what):
    """An extension chunk with its plugins re-stamped, less the ones San Andreas cannot read."""
    body = []
    for kind, _ver, cs, ce in rw_children(data, s, e, what + " extension"):
        if kind in DROP_PLUGINS:
            continue
        if kind == RW_MATFX and ce - cs > 4:
            body.append(rw_chunk(kind, convert_matfx(data, cs, ce)))
        else:
            body.append(rw_chunk(kind, data[cs:ce]))
    return rw_chunk(RW_EXTENSION, b"".join(body))


def convert_texture(data, s, e):
    """A material's texture reference: the filter/addressing word, the name, the mask, extension."""
    out, extension = [], None
    for kind, _ver, cs, ce in rw_children(data, s, e, "texture"):
        if kind == RW_EXTENSION:
            extension = convert_extension(data, cs, ce, "texture")
        else:
            out.append(rw_chunk(kind, data[cs:ce]))
    return rebuild(RW_TEXTURE, out, extension)


def convert_material(data, s, e, surface_props):
    """One material.

    Its struct is a flags word, the colour, an unused word and a textured flag, and from 3.0.4.0
    three floats of surface properties behind them. Every material GTA III, Vice City and San
    Andreas write has the 28-byte form, so this is a pass-through in practice; the shorter one is
    filled in from the geometry, which is where a reader older than 3.0.4.0 took it from.
    """
    kids = rw_children(data, s, e, "material")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("material: no struct")
    _kind, _ver, ss, se = kids[0]
    if se - ss == 16:
        body = data[ss:se] + surface_props
    elif se - ss == 28:
        body = data[ss:se]
    else:
        raise RwError("material struct: %d bytes, expected 16 or 28" % (se - ss))
    out, extension = [rw_chunk(RW_STRUCT, body)], None
    for kind, _ver, cs, ce in kids[1:]:
        if kind == RW_EXTENSION:
            extension = convert_extension(data, cs, ce, "material")
        elif kind == RW_TEXTURE:
            out.append(convert_texture(data, cs, ce))
        else:
            out.append(rw_chunk(kind, data[cs:ce]))
    return rebuild(RW_MATERIAL, out, extension)


def convert_material_list(data, s, e, surface_props):
    """The material list, expanded to one converted material per slot.

    On the wire it is a count, then one index per material - negative where the material follows
    inline, otherwise the earlier material it repeats - and then the materials themselves. The
    repeats are expanded here so what comes back is simply the list of materials the geometry uses,
    in slot order, which is what merge_materials compares and what the triangles index into.
    """
    kids = rw_children(data, s, e, "material list")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("material list: no struct")
    _kind, _ver, ss, se = kids[0]
    count = struct.unpack_from("<i", data, ss)[0]
    if se - ss != 4 + 4 * count:
        raise RwError("material list struct: %d bytes for %d material(s)" % (se - ss, count))
    indices = struct.unpack_from("<%di" % count, data, ss + 4) if count else ()
    written = []
    for kind, _ver, cs, ce in kids[1:]:
        if kind != RW_MATERIAL:
            raise RwError("material list holds a %#x chunk" % kind)
        written.append(convert_material(data, cs, ce, surface_props))
    materials, taken = [], 0
    for i in indices:
        if i < 0:
            if taken >= len(written):
                raise RwError("material list: fewer materials written than indexed")
            materials.append(written[taken])
            taken += 1
        elif i < len(materials):
            materials.append(materials[i])
        else:
            raise RwError("material list: a material repeats one not yet written")
    if taken != len(written):
        raise RwError("material list: %d written, %d taken" % (len(written), taken))
    return materials


def material_list_chunk(materials):
    """A material list written back out, every material inline."""
    head = struct.pack("<i", len(materials)) + struct.pack("<%di" % len(materials),
                                                           *([-1] * len(materials)))
    return rw_chunk(RW_MATLIST, rw_chunk(RW_STRUCT, head) + b"".join(materials))


def merge_materials(materials):
    """Fold materials that are identical down to the byte into one, and say where each slot went.

    Vice City's Little Haiti models are built from one material repeated per face: v_lithitsjm
    carries 165 of them, every one the same colour, flags and texture (huspnk256hi), across 1592
    triangles. RenderWare gives each material its own mesh, so the model reaches San Andreas' D3D
    pipeline as 165 meshes - where San Andreas' own 16264 geometries never once exceed 57 - and the
    instance it builds for them is what the render then calls through as garbage. Nothing in the
    data is malformed, so it survives every structural check; it is simply far past the shape the
    engine is built for.

    Identical materials render identically, so they can be one, and 165 meshes become one. That is
    the whole fix: it takes 16 Vice City models and 4 Liberty City ones back under San Andreas' own
    ceiling without touching a vertex.
    """
    unique, remap, seen = [], [], {}
    for material in materials:
        at = seen.get(material)
        if at is None:
            at = seen[material] = len(unique)
            unique.append(material)
        remap.append(at)
    return unique, remap


def convert_geometry_struct(data, s, e, version):
    """An RpGeometry struct read in its own version's layout and written in 3.4 onwards'.

    The layout is: the flags word, the triangle, vertex and morph-target counts, then - below
    3.4.0.0 only - three floats of surface properties, then, unless the geometry is already
    instanced for a platform (rpGEOMETRYNATIVE, which nothing in either source is), a prelit RGBA
    per vertex if rpGEOMETRYPRELIT is set, two floats per vertex for each texture-coordinate set,
    and two words per triangle holding its three vertex indices and its material. Each morph
    target then adds a bounding sphere, a flag for vertices and one for normals, and three floats
    per vertex for whichever of those two it says it has.

    Only two things change on the way out: the surface properties go, and the number of texture-
    coordinate sets is written into the flags' third byte the way 3.4 onwards expects to read it.
    Everything from the prelit colours onwards is laid out identically in both versions and is
    copied across as one block, which is what keeps the vertices, normals, UVs and triangles exact.

    The parse is checked against the chunk's own length before anything is written, so a geometry
    whose layout is not the one being assumed is refused rather than mangled. Across all 6050 of
    GTA III's geometries, 6510 of Vice City's and 16264 of San Andreas' own, it accounts for every
    byte of every struct.
    """
    flags, num_tri, num_vert, num_morph = struct.unpack_from("<IiiI", data, s)
    at = s + 16
    if version < RW_34:
        at += 12
    sets = (flags >> 16) & 0xFF
    if sets == 0:
        sets = 1 if flags & GEOM_TEXTURED else (2 if flags & GEOM_TEXTURED2 else 0)
    tail = at
    if not flags & GEOM_NATIVE:
        at += (4 * num_vert if flags & GEOM_PRELIT else 0) + sets * 8 * num_vert + 8 * num_tri
    for _ in range(num_morph):
        at += 16
        has_vertices, has_normals = struct.unpack_from("<ii", data, at)
        at += 8
        if not flags & GEOM_NATIVE:
            at += (12 * num_vert if has_vertices else 0) + (12 * num_vert if has_normals else 0)
    if at != e:
        raise RwError("geometry struct: %d bytes, its %s layout accounts for %d"
                      % (e - s, "3.4+" if version >= RW_34 else "pre-3.4", at - s))
    out_flags = (flags & 0xFF00FFFF) | (sets << 16)
    # Where the triangles sit inside the block that follows the head, so their material indices can
    # be remapped when identical materials are merged.
    triangles_at = (4 * num_vert if flags & GEOM_PRELIT else 0) + sets * 8 * num_vert
    body = data[tail:e]
    # Put back the ambient light San Andreas will not add for itself - see PRELIT_LIFT. Only the
    # colour is touched; the alpha a vertex carries is what fades and blends it and stays as it was.
    if PRELIT_LIFT and flags & GEOM_PRELIT and not flags & GEOM_NATIVE and num_vert:
        body = bytearray(body)
        for v in range(num_vert):
            at = 4 * v
            for c in range(3):
                body[at + c] += int((255 - body[at + c]) * PRELIT_LIFT)
        body = bytes(body)
    return out_flags, body, triangles_at, (num_tri, num_vert)


def triangles_readable(body, triangles_at, num_tri, materials, num_vert):
    """True when the triangle block really is at `triangles_at` and can be rewritten.

    Every triangle has to name a material the list holds and three vertices the geometry has. Both
    are cheap to check and neither holds for data that is not a triangle array, so this is what
    decides whether a geometry's materials may be merged or are left exactly as they came.
    """
    if triangles_at + 8 * num_tri > len(body):
        return False
    for t in range(num_tri):
        v2, v1, material, v3 = struct.unpack_from("<HHHH", body, triangles_at + 8 * t)
        if material >= materials or v1 >= num_vert or v2 >= num_vert or v3 >= num_vert:
            return False
    return True


def rebuild_meshes(body, triangles_at, num_tri, remap, extension):
    """Point the triangles at the merged materials and rebuild the mesh split to match.

    A triangle is two words: its second and first vertex, then its material and its third - the
    material sits at byte four of the eight, not at the end. Only the material is rewritten.

    The mesh split is then regenerated from those triangles rather than edited, because merging
    strips is not a matter of concatenating them - joining two would need degenerate triangles
    stitched between them - whereas the triangle list is the geometry's own
    authority on what belongs to which material, and a list-form mesh is what the pipeline reads
    when the strip flag is clear. One mesh per surviving material, in material order.
    """
    body = bytearray(body)
    groups = {}
    for t in range(num_tri):
        at = triangles_at + 8 * t
        v2, v1, material, v3 = struct.unpack_from("<HHHH", body, at)
        material = remap[material]
        struct.pack_into("<H", body, at + 4, material)
        groups.setdefault(material, []).extend((v1, v2, v3))
    meshes, total = [], 0
    for material in sorted(groups):
        indices = groups[material]
        meshes.append(struct.pack("<II", len(indices), material)
                      + struct.pack("<%dI" % len(indices), *indices))
        total += len(indices)
    binmesh = struct.pack("<III", 0, len(meshes), total) + b"".join(meshes)
    # Swap it into the extension that was already converted, leaving its other plugins alone.
    inner, at, replaced = [], 12, False
    while at + 12 <= len(extension):
        kind, size, _lib = struct.unpack_from("<III", extension, at)
        if kind == RW_BINMESH:
            inner.append(rw_chunk(RW_BINMESH, binmesh))
            replaced = True
        else:
            inner.append(extension[at:at + 12 + size])
        at += 12 + size
    if not replaced:
        inner.append(rw_chunk(RW_BINMESH, binmesh))
    return bytes(body), rw_chunk(RW_EXTENSION, b"".join(inner))


def convert_geometry(data, s, e, version):
    """One geometry: its struct, its material list, and its extension.

    Materials identical to the byte are merged here, and where any were the triangles and the mesh
    split are rewritten to match - see merge_materials for why that is worth doing.
    """
    kids = rw_children(data, s, e, "geometry")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("geometry: no struct")
    _kind, _ver, ss, se = kids[0]
    # A material older than 3.0.4.0 has no surface properties of its own and takes the geometry's,
    # which is the only thing the twelve bytes being dropped here are ever read for.
    surface_props = data[ss + 16:ss + 28] if version < RW_34 else struct.pack("<fff", 1.0, 1.0, 1.0)
    flags, body, triangles_at, counts = convert_geometry_struct(data, ss, se, version)
    num_tri, num_vert = counts
    materials, extension, others = None, None, []
    for kind, _ver, cs, ce in kids[1:]:
        if kind == RW_MATLIST:
            materials = convert_material_list(data, cs, ce, surface_props)
        elif kind == RW_EXTENSION:
            extension = convert_extension(data, cs, ce, "geometry")
        else:
            others.append(rw_chunk(kind, data[cs:ce]))
    if materials is None:
        raise RwError("geometry: no material list")
    unique, remap = merge_materials(materials)
    # Merging rewrites the triangles in place, so it is only safe where they are exactly where this
    # expects them. 224 of Vice City's models - its LOD set, whose geometry is laid out differently
    # enough that the triangles are not found at the offset computed above - fail that test, and
    # they are simply left with their materials as they came. Merging is an optimisation, not part
    # of the conversion, so a geometry that cannot be read this way still converts; it just keeps
    # every material it had. None of them are near the mesh ceiling anyway.
    merged = len(unique) < len(materials) and triangles_readable(body, triangles_at, num_tri,
                                                                 len(materials), num_vert)
    if merged and not flags & GEOM_NATIVE:
        if extension is None:
            extension = rw_chunk(RW_EXTENSION, b"")
        body, extension = rebuild_meshes(body, triangles_at, num_tri, remap, extension)
        flags &= ~GEOM_TRISTRIP     # the meshes written above are lists, not strips
    else:
        unique, merged = materials, False
    head = struct.pack("<IiiI", flags, num_tri, num_vert,
                       struct.unpack_from("<I", data, ss + 12)[0])
    out = [rw_chunk(RW_STRUCT, head + body), material_list_chunk(unique)] + others
    return rebuild(RW_GEOMETRY, out, extension), (num_tri, num_vert, len(unique), merged)


def convert_geometry_list(data, s, e, version):
    """The clump's geometry list: a count, and then that many geometries."""
    kids = rw_children(data, s, e, "geometry list")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("geometry list: no struct")
    _kind, _ver, ss, se = kids[0]
    out, counts = [rw_chunk(RW_STRUCT, data[ss:se])], []
    for kind, _ver, cs, ce in kids[1:]:
        if kind != RW_GEOMETRY:
            raise RwError("geometry list holds a %#x chunk" % kind)
        piece, count = convert_geometry(data, cs, ce, version)
        out.append(piece)
        counts.append(count)
    return rw_chunk(RW_GEOMETRYLIST, b"".join(out)), counts


def convert_atomic(data, s, e, version):
    """One atomic: which frame it hangs on, which geometry it draws, its flags, and its extension.

    Below 3.0.4.0 the geometry sits inside the atomic rather than in the clump's geometry list.
    Nothing in either source is that old - GTA III's oldest clump is exactly 3.0.4.0 - but the
    case costs one branch to carry.
    """
    kids = rw_children(data, s, e, "atomic")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("atomic: no struct")
    _kind, _ver, ss, se = kids[0]
    if se - ss != 16:
        raise RwError("atomic struct: %d bytes, expected 16" % (se - ss))
    out, extension, counts = [rw_chunk(RW_STRUCT, data[ss:se])], None, []
    for kind, _ver, cs, ce in kids[1:]:
        if kind == RW_EXTENSION:
            extension = convert_extension(data, cs, ce, "atomic")
        elif kind == RW_GEOMETRY:
            piece, count = convert_geometry(data, cs, ce, version)
            out.append(piece)
            counts.append(count)
        else:
            out.append(rw_chunk(kind, data[cs:ce]))
    return rebuild(RW_ATOMIC, out, extension), counts


def convert_frame_list(data, s, e):
    """The frame list.

    A count, then a 3x3 rotation, a position, a parent index and a flags word for each frame, and
    then one extension chunk per frame carrying its name and its animation hierarchy. The frame
    struct is the same in every version this deals with; what is rebuilt is the run of extensions,
    so that there is exactly one for every frame the struct declares - RenderWare reads them by
    count, so a missing one would be taken out of whatever chunk came next.
    """
    kids = rw_children(data, s, e, "frame list")
    if not kids or kids[0][0] != RW_STRUCT:
        raise RwError("frame list: no struct")
    _kind, _ver, ss, se = kids[0]
    count = struct.unpack_from("<i", data, ss)[0]
    if se - ss != 4 + 56 * count:
        raise RwError("frame list struct: %d bytes for %d frame(s)" % (se - ss, count))
    out = [rw_chunk(RW_STRUCT, data[ss:se])]
    frames = [k for k in kids[1:] if k[0] == RW_EXTENSION]
    for i in range(count):
        if i < len(frames):
            out.append(convert_extension(data, frames[i][2], frames[i][3], "frame"))
        else:
            out.append(rw_chunk(RW_EXTENSION, b""))
    return rw_chunk(RW_FRAMELIST, b"".join(out))


def convert_clump(data, s, limit, version):
    """The clump itself: its counts, frame list, geometry list, atomics, any lights, and extension.

    Read the way RpClumpStreamRead reads it - by the counts in the struct rather than by the size
    in the header - because 32 of GTA III's models need that: their clump header stops short of
    their own last atomic, by 180 bytes on chunky.dff, and RenderWare never notices because it takes
    each child in turn and stops when it has the number the struct asked for. The header is written
    back out with the length it should have had. `limit` is the end of the blob, not of the clump.

    A light or a camera is streamed as a bare struct naming the frame it hangs on followed by the
    object itself, and 130 of Vice City's clumps carry 759 lights between them. Both pass through
    untouched.
    """
    _kind, _ver, ss, se = rw_chunk_at(data, s, limit, "clump struct", RW_STRUCT)
    if se - ss == 4:
        counts = struct.unpack_from("<I", data, ss) + (0, 0)
    elif se - ss >= 12:
        counts = struct.unpack_from("<III", data, ss)
    else:
        raise RwError("clump struct: %d bytes, expected 4 or 12" % (se - ss))
    atomics, lights, cameras = counts
    out, geometries, at = [rw_chunk(RW_STRUCT, struct.pack("<III", *counts))], [], se

    _kind, _ver, cs, ce = rw_chunk_at(data, at, limit, "frame list", RW_FRAMELIST)
    out.append(convert_frame_list(data, cs, ce))
    at = ce

    if version >= RW_304:       # older than that and each atomic carries its own geometry instead
        _kind, _ver, cs, ce = rw_chunk_at(data, at, limit, "geometry list", RW_GEOMETRYLIST)
        piece, found = convert_geometry_list(data, cs, ce, version)
        out.append(piece)
        geometries += found
        at = ce

    for i in range(atomics):
        _kind, _ver, cs, ce = rw_chunk_at(data, at, limit, "atomic %d" % i, RW_ATOMIC)
        piece, found = convert_atomic(data, cs, ce, version)
        out.append(piece)
        geometries += found
        at = ce

    for kind, count in ((RW_LIGHT, lights), (RW_CAMERA, cameras)):
        for i in range(count):
            _k, _ver, cs, ce = rw_chunk_at(data, at, limit, "frame of %#x %d" % (kind, i), RW_STRUCT)
            out.append(rw_chunk(RW_STRUCT, data[cs:ce]))
            _k, _ver, cs, ce = rw_chunk_at(data, ce, limit, "%#x %d" % (kind, i), kind)
            out.append(rw_chunk(kind, data[cs:ce]))
            at = ce

    extension = None
    if at + 12 <= limit:
        kind, _ver, cs, ce = rw_chunk_at(data, at, limit, "clump extension")
        if kind == RW_EXTENSION:
            extension = convert_extension(data, cs, ce, "clump")
    return rebuild(RW_CLUMP, out, extension), geometries


def dff_geometry_counts(data):
    """[(triangles, vertices, materials)] for every geometry of a clump.

    Reads without rewriting anything and does not care which version wrote it, so a converted model
    can be held against its source - see tools/check_dff.py.
    """
    kind, _size, library_id = struct.unpack_from("<III", data, 0)
    if kind != RW_CLUMP:
        raise RwError("not a clump")
    version, limit, counts = rw_version(library_id), len(data), []
    _k, _v, _ss, se = rw_chunk_at(data, 12, limit, "clump struct", RW_STRUCT)
    _k, _v, _cs, ce = rw_chunk_at(data, se, limit, "frame list", RW_FRAMELIST)
    if version < RW_304:
        return counts                # older than that and the geometries hang off the atomics
    _k, _v, cs, ce = rw_chunk_at(data, ce, limit, "geometry list", RW_GEOMETRYLIST)
    for k2, _v2, s2, e2 in rw_children(data, cs, ce, "geometry list"):
        if k2 != RW_GEOMETRY:
            continue
        kids = rw_children(data, s2, e2, "geometry")
        _flags, num_tri, num_vert, _morph = struct.unpack_from("<IiiI", data, kids[0][2])
        materials = 0
        for k3, _v3, s3, e3 in kids[1:]:
            if k3 == RW_MATLIST:
                head = rw_children(data, s3, e3, "material list")[0]
                materials = struct.unpack_from("<i", data, head[2])[0]
        counts.append((num_tri, num_vert, materials))
    return counts


def convert_dff(data):
    """A GTA III / Vice City model rebuilt as a San Andreas one. Returns (bytes, converted).

    (data, False) comes back when the blob is not a clump at all - the source archives hold a
    handful of those - and an RwError is raised when it is one but cannot be read, so that the
    importer can name the model it left alone rather than quietly writing out a guess.
    """
    if len(data) < 12:
        return data, False, 0
    kind, _size, library_id = struct.unpack_from("<III", data, 0)
    if kind != RW_CLUMP:
        return data, False, 0
    try:
        out, counts = convert_clump(data, 12, len(data), rw_version(library_id))
    except (struct.error, IndexError, ValueError) as err:
        raise RwError("%s: %s" % (type(err).__name__, err))
    return out, True, sum(1 for c in counts if len(c) > 3 and c[3])


def write_png(path, width, height, rgba):
    try:
        from PIL import Image
        Image.frombytes("RGBA", (width, height), bytes(rgba)).save(path)
        return
    except ImportError:
        pass
    raw = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


# ---------------------------------------------------------------- source game layout

def find_file(root, rel):
    """Case-insensitive lookup of a game-relative path (the .dat files use upper case)."""
    parts = re.split(r"[\\/]+", rel.strip())
    cur = root
    for p in parts:
        if not p:
            continue
        hit = None
        try:
            for name in os.listdir(cur):
                if name.lower() == p.lower():
                    hit = name
                    break
        except OSError:
            return None
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def dat_base(rel):
    """The file name of a path out of a .dat, which writes them with backslashes and in capitals."""
    return re.split(r"[\\/]+", rel.strip())[-1].lower()


def read_dat(root):
    """The file lists of gta_vc.dat or gta3.dat, in order, and of data/default.dat with them.

    Returns (dat name, ides, ipls, cols, texdicts, zones).

    The level .dat is the map. Beside it sits data/default.dat, which describes what the engine
    keeps for the whole session, and exactly one kind of line is taken from there: TEXDICTION.
    GTA III keeps its two in that file - models/generic.txd and models/misc.txd, loaded from disk
    rather than out of gta3.img - and 608 of III's models name `generic` in their IDE txd column,
    so without it they have no dictionary at all. default.dat's IDE, IPL and COLFILE lines are
    deliberately left behind: they are the vehicles and weapons, numbered 0..199, none of which is
    in gta3.img and none of which a region loads.

    A .zon in the IPL list is a zone file rather than a scene: GTA III loads data/gta3.zon with an
    IPL line, and it belongs on a region's Zones key, not its Ipl key. MAPZONE lines are dropped -
    III's data/map.zon holds the island-loading zones, which mean nothing outside III's own
    streaming - and so are MODELFILE lines, for the same reason as default.ide.
    """
    for name in ("data/gta_vc.dat", "data/gta3.dat"):
        path = find_file(root, name)
        if path:
            break
    else:
        sys.exit("no data/gta_vc.dat or data/gta3.dat under %s" % root)
    ides, ipls, cols, texdicts, zones = [], [], [], [], []
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            if not line or line[0] == "#":
                continue
            tok = line.split()
            key = tok[0].upper()
            if key == "IDE" and len(tok) > 1:
                ides.append(tok[1])
            elif key == "IPL" and len(tok) > 1:
                (zones if tok[1].lower().endswith(".zon") else ipls).append(tok[1])
            elif key == "COLFILE" and len(tok) > 2:
                cols.append(tok[2])
            elif key == "TEXDICTION" and len(tok) > 1:
                texdicts.append(tok[1])
    default = find_file(root, "data/default.dat")
    if default and default != path:
        with open(default, "r", encoding="latin-1") as f:
            for line in f:
                tok = line.strip().split()
                if len(tok) > 1 and tok[0].upper() == "TEXDICTION":
                    texdicts.append(tok[1])
    return os.path.basename(path), ides, ipls, cols, texdicts, zones


# ---------------------------------------------------------------- renaming clashes

# GTA III and San Andreas read the same IDE flag word but disagree about its lowest bit. In III
# 0x1 is CSimpleModelInfo::m_normalCull - "cull this when it is not being looked at" - while in San
# Andreas the same bit is IS_ROAD, which puts a wet-road reflection on the model whenever it rains.
# Everything above it means the same in both: 0x2 do not fade in, 0x4 draw last, 0x8 additive,
# 0x10 subway, 0x20 ignore lighting, 0x40 no z-buffer write. So clearing the one bit is the whole
# remap. Only three of III's 2909 objects set it, but a puddle reflection on a subway wall is a
# strange thing to ship, and Vice City agrees with San Andreas here and needs no remap at all.
III_DROP_FLAGS = 0x1


def remap_flags(tok, section, drop):
    """An objs/tobj line's flag column with `drop` cleared, or the line unchanged.

    III and Vice City both write `id, model, txd, meshCount, drawDist * meshCount, flags`, and
    tobj puts timeOn/timeOff after that, so the column is at 4 + meshCount either way.
    """
    if not drop or section not in ("objs", "tobj") or len(tok) < 6:
        return tok
    try:
        at = 4 + int(tok[3])
        flags = int(tok[at])
    except (ValueError, IndexError):
        return tok
    if not flags & drop:
        return tok
    tok = list(tok)
    tok[at] = str(flags & ~drop)
    return tok


def rename_ide(text, renames, drop_flags=0):
    out = []
    section = None
    for line in text.splitlines():
        s = line.strip().lower()
        if section is None:
            if s in ("objs", "tobj", "anim", "peds", "cars", "weap", "hier", "path", "2dfx", "txdp"):
                section = s
            out.append(line)
            continue
        if s == "end":
            section = None
            out.append(line)
            continue
        if section in ("objs", "tobj", "anim", "peds", "cars", "weap", "hier") and not s.startswith("#"):
            tok = [t.strip() for t in re.split(r"[,\s]+", line.strip()) if t.strip()]
            if len(tok) >= 3:
                new = list(tok)
                new[1] = renames.get(new[1].lower() + ".dff", new[1])
                new[2] = renames.get(new[2].lower() + ".txd", new[2])
                new = remap_flags(new, section, drop_flags)
                if new != tok:
                    line = ", ".join(new)
        elif section == "txdp" and not s.startswith("#"):
            tok = [t.strip() for t in re.split(r"[,\s]+", line.strip()) if t.strip()]
            if len(tok) >= 2:
                tok[0] = renames.get(tok[0].lower() + ".txd", tok[0])
                tok[1] = renames.get(tok[1].lower() + ".txd", tok[1])
                line = ", ".join(tok)
        out.append(line)
    return "\n".join(out) + "\n"


def rename_ipl(text, renames):
    out = []
    section = None
    for line in text.splitlines():
        s = line.strip().lower()
        if section is None:
            if s in ("inst", "cull", "pick", "zone", "path", "occl", "mult", "enex", "cars",
                     "jump", "tcyc", "auzo", "grge"):
                section = s
            out.append(line)
            continue
        if s == "end":
            section = None
            out.append(line)
            continue
        if section == "inst" and not s.startswith("#"):
            tok = [t.strip() for t in re.split(r"[,\s]+", line.strip()) if t.strip()]
            if len(tok) >= 2:
                tok[1] = renames.get(tok[1].lower() + ".dff", tok[1])
                line = ", ".join(tok)
        out.append(line)
    return "\n".join(out) + "\n"


COL_DAYLIGHT = 255        # what a fully lit outdoor surface reads in San Andreas own collision


def light_col1(out, at, end):
    """Give a COL1 model's surfaces the daylight value San Andreas expects. Returns faces touched.

    San Andreas lights a ped, a car, anything dynamic, partly from the ground beneath it:
    CEntity::GetLightingFromCol (0x5447B0) reads the brightness the entity picked up from the
    surface it is standing on and adds it to the ambient of the time cycle. That brightness comes
    from a byte per collision face - material, flag, brightness, light - and San Andreas' own
    collision uses the whole range, 255 being both "fully lit" and much its most common value.

    III and Vice City never used the field. Every face, sphere and box in both games reads zero,
    because their engines lit dynamic objects some other way. Carried across as they are, a ped who
    steps off San Andreas' ground onto an imported street is told the ground gives off no light at
    all and visibly darkens, which is what makes peds look wrong on these maps and only on these
    maps.

    There is nothing to preserve here - the source is uniformly zero, so no information is lost by
    replacing it - and outdoors, fully lit, is both the sane default for a city street and what San
    Andreas writes most often itself. The layout is: bounds, then a count and that many spheres of
    20 bytes, a count of lines, a count and that many boxes of 28, a count and that many vertices
    of 12, then a count and that many faces of 16, each ending in the four surface bytes.
    """
    p = at + 32 + 40                         # header, then bounds
    faces = 0
    try:
        count = struct.unpack_from("<I", out, p)[0]; p += 4
        for _ in range(count):               # spheres: radius, centre, surface
            out[p + 19] = COL_DAYLIGHT
            p += 20
        p += 4                               # the line count, always zero in both games
        count = struct.unpack_from("<I", out, p)[0]; p += 4
        for _ in range(count):               # boxes: min, max, surface
            out[p + 27] = COL_DAYLIGHT
            p += 28
        count = struct.unpack_from("<I", out, p)[0]
        p += 4 + count * 12                  # vertices
        count = struct.unpack_from("<I", out, p)[0]; p += 4
        if p + count * 16 > end:
            return 0                         # not the layout assumed; leave the model alone
        for _ in range(count):               # faces: a, b, c, surface
            out[p + 15] = COL_DAYLIGHT
            p += 16
            faces += 1
    except (struct.error, IndexError):
        return 0
    return faces


def rename_col(data, renames, relight=True):
    """A .col archive is a run of COLL/COL2/COL3 entries: fourcc, u32 size, char name[22], u16 id."""
    out = bytearray(data)
    at = 0
    lit = 0
    while at + 8 <= len(out):
        fourcc = bytes(out[at:at + 4])
        if fourcc not in (b"COLL", b"COL2", b"COL3", b"COL4"):
            break
        size = struct.unpack_from("<I", out, at + 4)[0]
        name = bytes(out[at + 8:at + 30]).split(b"\0", 1)[0].decode("latin-1")
        new = renames.get(name.lower() + ".dff")
        if new:
            out[at + 8:at + 30] = new.encode("latin-1")[:21].ljust(22, b"\0")
        if relight and fourcc == b"COLL":
            lit += light_col1(out, at, at + 8 + size)
        at += 8 + size
    return bytes(out), lit


# ---------------------------------------------------------------- namespacing
#
# Every model, texture dictionary and collision name the game sees from a region is given a name
# of its own: a two-character tag for the source game, then the original stem, unique within its
# namespace and within the 19 characters an IMG entry, an IDE txd column and a COL header all
# allow. San Andreas keeps no names - CBaseModelInfo stores only CKeyGen::GetUppercaseKey(name),
# a CRC32, and CTxdStore slots the same - so renaming costs nothing at run time; what it buys is
# that nothing of ours can share a key with anything of the game's or of another region's.
#
# Texture names inside a dictionary are deliberately left alone. RenderWare resolves a material's
# texture within the model's own dictionary, so they only have to be unique there, and Vice City
# already is. See docs/asset-namespacing.md for the measurements behind all of this.

NAME_BUDGET = 19          # 23-byte IMG name field, less ".dff"/".txd" and the terminator
TAG = {"gta_vc.dat": "v_", "gta3.dat": "l_"}

# Names the game creates or matches by literal, which a region must never produce.
RESERVED = {"generic", "particle", "vehicle", "hud", "fonts", "ps2btns", "script", "grass_pc",
            "frontend1", "frontend2", "frontend3", "frontend_pc", "player", "loadscs",
            "gen_int1", "gen_int2", "gen_int3", "gen_int4", "gen_int5", "gen_intb", "savehous",
            "stadint", "int_la", "int_sf", "int_veg", "int_cont", "levelmap", "procobj",
            "proc_int", "props"}


def ckey(name):
    """CKeyGen::GetUppercaseKey: reflected CRC-32 over the uppercased name, no final complement."""
    return zlib.crc32(name.upper().encode("latin-1")) ^ 0xFFFFFFFF


def col_names(data):
    """Model names in a .col archive (COLL/COL2/COL3/COL4 chunks)."""
    names, at = [], 0
    while at + 8 <= len(data):
        if data[at:at + 4] not in (b"COLL", b"COL2", b"COL3", b"COL4"):
            break
        size = struct.unpack_from("<I", data, at + 4)[0]
        names.append(data[at + 8:at + 30].split(b"\0", 1)[0].decode("latin-1"))
        at += 8 + size
    return names


def sa_name_keys(sa):
    """CKeyGen keys of every model and dictionary name San Andreas already has, by namespace.

    Models are keyed with collision names too, because LoadCollisionFileFirstTime matches a col
    entry against model keys. Sources: all four archives, every IDE under data/, and the loose
    models/*.dff|txd|col files - generic.txd lives there and nowhere else.
    """
    dff, txd = set(), set()
    for rel in ("models/gta3.img", "models/gta_int.img", "models/player.img", "models/cutscene.img"):
        path = find_file(sa, rel)
        if not path:
            continue
        for name in read_img_v2_names(path):
            stem, ext = os.path.splitext(name)
            if ext == ".dff" or ext == ".col":
                dff.add(ckey(stem))
            elif ext == ".txd":
                txd.add(ckey(stem))
    for root, _dirs, files in os.walk(os.path.join(sa, "data")):
        for f in files:
            if not f.lower().endswith(".ide"):
                continue
            section = None
            for line in open(os.path.join(root, f), encoding="latin-1", errors="replace"):
                s = line.strip().lower()
                if section is None:
                    if s in ("objs", "tobj", "anim", "peds", "cars", "weap", "hier", "txdp"):
                        section = s
                    continue
                if s == "end":
                    section = None
                    continue
                if not s or s.startswith("#"):
                    continue
                tok = [t for t in re.split(r"[,\s]+", s) if t]
                if section == "txdp" and len(tok) >= 2:
                    txd.add(ckey(tok[0])); txd.add(ckey(tok[1]))
                elif len(tok) >= 3:
                    dff.add(ckey(tok[1])); txd.add(ckey(tok[2]))
    models = find_file(sa, "models")
    if models and os.path.isdir(models):
        for f in os.listdir(models):
            stem, ext = os.path.splitext(f.lower())
            if ext == ".dff":
                dff.add(ckey(stem))
            elif ext == ".txd":
                txd.add(ckey(stem))
        coll = os.path.join(models, "coll")
        if os.path.isdir(coll):
            for f in os.listdir(coll):
                if f.lower().endswith(".col"):
                    with open(os.path.join(coll, f), "rb") as h:
                        for n in col_names(h.read()):
                            dff.add(ckey(n))
    for r in RESERVED:
        dff.add(ckey(r)); txd.add(ckey(r))
    return {".dff": dff, ".txd": txd}


def split_time_suffix(stem):
    """_nt/_dy stays at the very end: CBaseModelInfo::FindOtherTimeModel matches it literally."""
    if stem[-3:].lower() in ("_nt", "_dy"):
        return stem[:-3], stem[-3:]
    return stem, ""


def fallback_name(tag, stem, salt):
    """tag + as much of the stem as fits + ~ + four hex digits of the name's key + _nt/_dy."""
    head, suffix = split_time_suffix(stem)
    tail = "~%04X" % (ckey(tag + stem + ("#%d" % salt if salt else "")) & 0xFFFF)
    return (tag + head)[:NAME_BUDGET - len(tail) - len(suffix)] + tail + suffix


def make_renames(entries, sa_keys, tag):
    """A new stem for every .dff/.txd entry, and which entries to leave out of the archive.

    Returns (renames, dropped, reasons): renames maps the lowercase source name to its new stem;
    dropped is the set of entry indices that are later copies of a .dff name the archive already
    carries - the source game binds only the first, and nothing can refer to the second - and
    reasons maps each new stem to why it looks the way it does, for names.txt.

    A .txd carried twice is not dropped: the packing loop merges same-named dictionaries into
    one, which is what fixed Vice City's white leaves, and both copies keep the one new name.
    """
    renames, reasons, dropped = {}, {}, set()
    used_names = {".dff": set(), ".txd": set()}
    used_keys = {".dff": set(), ".txd": set()}
    seen = {}
    for index, (name, _off, _size) in enumerate(entries):
        low = name.lower()
        stem, ext = os.path.splitext(name)
        ext = ext.lower()
        if ext not in (".dff", ".txd"):
            continue
        if low in seen:
            if ext == ".dff":
                dropped.add(index)
            continue
        seen[low] = index

        plain = tag + stem
        why = "tag"
        if stem.lower() in RESERVED:
            candidate, why = fallback_name(tag, stem, 0), "reserved name"
        elif len(plain) > NAME_BUDGET:
            candidate, why = fallback_name(tag, stem, 0), "too long for the 19-character field"
        else:
            candidate = plain
        salt = 0
        while (candidate.lower() in used_names[ext] or ckey(candidate) in used_keys[ext]
               or ckey(candidate) in sa_keys[ext] or candidate.lower() in RESERVED):
            salt += 1
            candidate = fallback_name(tag, stem, salt)
            why = "key clash, salted"
        used_names[ext].add(candidate.lower())
        used_keys[ext].add(ckey(candidate))
        renames[low] = candidate
        reasons[low] = why
    return renames, dropped, reasons


def assert_namespaces(renames, sa_keys):
    """Refuse to write an archive whose names could collide: this is the guarantee, not the loop."""
    by_ext = {".dff": {}, ".txd": {}}
    for low, new in renames.items():
        ext = os.path.splitext(low)[1]
        if len(new) > NAME_BUDGET:
            sys.exit("namespacing: %s -> %s is %d characters, over the %d the game allows"
                     % (low, new, len(new), NAME_BUDGET))
        key = ckey(new)
        other = by_ext[ext].get(key)
        if other is not None and other.lower() != new.lower():
            sys.exit("namespacing: %s and %s share CKeyGen key %08X" % (other, new, key))
        if key in sa_keys[ext]:
            sys.exit("namespacing: %s has the same CKeyGen key as a San Andreas %s" % (new, ext))
        by_ext[ext][key] = new


# ---------------------------------------------------------------- main

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 2:
        sys.exit(__doc__)
    src, sa = args[0], args[1]
    region = "vicecity"
    for o in opts:
        if o.startswith("--region="):
            region = o.split("=", 1)[1]
    lite = "--lite" in opts
    do_rename = "--no-rename" not in opts and not lite

    dat_name, ide_rel, ipl_rel, col_rel, texdict_rel, zon_rel = read_dat(src)
    prefix = "iii_" if dat_name.lower() == "gta3.dat" else "vc_"
    out = os.path.join(sa, "MapExpander", region)
    # Empty the folders this writes before filling them again. A region's Ide/Ipl/Col keys are
    # wildcards, so a file left behind by an earlier run of a different source - or of an earlier
    # version of this script - is loaded as if it belonged, and an IDE that names models the
    # archive no longer carries is the one thing the streamer does not survive.
    for sub in (("tiles",) if lite else ("col", "ide", "ipl", "tiles")):
        folder = os.path.join(out, sub)
        os.makedirs(folder, exist_ok=True)
        for name in os.listdir(folder):
            path = os.path.join(folder, name)
            if os.path.isfile(path):
                os.remove(path)
    print("source: %s (%s)%s" % (src, dat_name, " - lite: tiles, text, water and zones only" if lite else ""))
    print("output: %s" % out)

    img = find_file(src, "models/gta3.img")
    dirf = find_file(src, "models/gta3.dir")
    if not img or not dirf:
        sys.exit("models/gta3.img + gta3.dir not found under %s" % src)
    entries = read_img_v1(img, dirf)
    with open(img, "rb") as f:
        archive = f.read()
    # GTA III's gta3.dir has four entries of zero length (plaster, g, stu_man, stu_wom - no IDE
    # declares any of them). An entry like that is worse than an absent one: San Andreas registers
    # the model, then builds a read of size zero the first time something asks for it and retries
    # that read for ever, with no crash and nothing in the log. Leave them out of the archive so
    # the model has no data behind it at all, which the loader already knows to skip.
    empty = [name for name, _off, size in entries if size == 0]
    entries = [e for e in entries if e[2] > 0]

    # The dictionaries the .dat loads from disk rather than from the archive. GTA III's
    # models/generic.txd is one of them and 608 of its models name it in their txd column, so it
    # has to travel with the region under a name of its own - `generic` is San Andreas' too, which
    # is why RESERVED forbids it. They double as the pool a model borrows a missing texture from,
    # below, exactly as the source game resolves through them at run time.
    loose = []
    for rel in ([] if lite else texdict_rel):
        path = find_file(src, rel)
        if not path:
            print("  TEXDICTION %s not found" % rel)
            continue
        with open(path, "rb") as f:
            loose.append((os.path.basename(path).lower(), f.read()))
    print("archive: %d entries%s%s"
          % (len(entries),
             "" if not empty else ", %d empty one(s) left out (%s)" % (len(empty), ", ".join(empty)),
             "" if not loose else ", plus %d dictionar%s the .dat loads from disk (%s)"
             % (len(loose), "y" if len(loose) == 1 else "ies", ", ".join(n for n, _b in loose))))

    # Everything that ends up in the archive, archive entries first, so an index means the same
    # thing to make_renames and to the packing loop below.
    sources = list(entries)
    sources += [(name, None, None) for name, _blob in loose]

    tag = TAG.get(dat_name.lower(), prefix[:1] + "_")
    renames, dropped, reasons = {}, set(), {}
    if do_rename:
        sa_keys = sa_name_keys(sa)
        if not sa_keys[".dff"]:
            print("warning: no San Andreas names found under %s; key clashes with the game not checked" % sa)
        renames, dropped, reasons = make_renames(sources, sa_keys, tag)
        assert_namespaces(renames, sa_keys)

    def renamed(name):
        new_base = renames.get(name.lower())
        return new_base + os.path.splitext(name)[1] if new_base else name

    # Which dictionary each model is bound to, in post-rename names. The IDEs are written out
    # further down; they are read here too because the packing below needs to know which textures
    # each dictionary is actually asked for.
    model_txd = {}
    for rel in ([] if lite else ide_rel):
        path = find_file(src, rel)
        if not path:
            continue
        with open(path, "r", encoding="latin-1") as f:
            section = None
            for line in f:
                s = line.strip().lower()
                if section is None:
                    if s in ("objs", "tobj"):
                        section = s
                    continue
                if s == "end":
                    section = None
                    continue
                if s.startswith("#"):
                    continue
                tok = [t.strip() for t in re.split(r"[,\s]+", line.strip()) if t.strip()]
                if len(tok) >= 3:
                    model = renames.get(tok[1].lower() + ".dff", tok[1]).lower()
                    txd = renames.get(tok[2].lower() + ".txd", tok[2]).lower() + ".txd"
                    model_txd[model] = txd

    # Split the archive: models/textures into the new IMG, collision into loose files,
    # radar tiles into PNGs. Everything else (audio, misc) is dropped.
    packed, cols, tiles, skipped = [], 0, 0, 0
    col_faces_lit = 0
    txd_parts = {}
    dff_refs = {}
    tex_done, tex_failed, txds = 0, 0, 0
    dff_done, dffs, dff_merged = 0, 0, 0
    failed_txds = []
    failed_dffs = []
    dropped_dups = []

    def blobs():
        """(index, name, bytes) for every source entry, in the order `sources` names them."""
        for i, (name, off, size) in enumerate(entries):
            yield i, name, archive[off:off + size]
        for i, (name, data) in enumerate(loose):
            yield len(entries) + i, name, data

    for index, name, blob in blobs():
        if index in dropped:
            dropped_dups.append(name)
            continue
        ext = os.path.splitext(name)[1].lower()
        low = name.lower()
        if ext == ".txd" and re.fullmatch(r"radar\d+\.txd", low):
            tex = txd_first_texture(blob)
            if tex:
                w, h, rgba = tex
                write_png(os.path.join(out, "tiles", low[:-4] + ".png"), w, h, rgba)
                tiles += 1
            else:
                print("  %s: no texture found" % name)
        elif lite:
            continue
        elif ext == ".col":
            with open(os.path.join(out, "col", name), "wb") as f:
                data, faces = rename_col(blob, renames)

                f.write(data)

                col_faces_lit += faces
            cols += 1
        elif ext == ".txd":
            txds += 1
            chunks, done, bad = txd_texture_chunks(blob)
            tex_done += done
            tex_failed += bad
            if bad:
                failed_txds.append(name)
            key = renamed(name)
            if chunks is None:
                packed.append((key, blob))          # not a dictionary; pass it through untouched
            else:
                part = txd_parts.setdefault(key.lower(), [key, [], 0])
                part[1].extend(chunks)
                part[2] += 1
        elif ext == ".dff":
            key = renamed(name)
            dff_refs[key.lower().rsplit(".", 1)[0]] = dff_texture_names(blob)
            dffs += 1
            try:
                blob, done, merged = convert_dff(blob)
                dff_done += done
                dff_merged += merged
            except RwError as err:
                # One model San Andreas may not be able to read is a model that does not draw;
                # a whole import that stops on it is a region that does not exist. Pass it
                # through as it came and say which one it was.
                failed_dffs.append("%s (%s)" % (name, err))
            packed.append((key, blob))
        else:
            skipped += 1
    # A texture a model's own dictionary does not hold is resolved by III and Vice City out of a
    # dictionary their engines keep loaded for the whole map - the files the .dat names with
    # TEXDICTION, models/generic.txd above all. San Andreas has no equivalent: RenderWare only ever
    # looks in the dictionary the model is bound to, so those models render untextured, which is why
    # Vice City's palms came out as white squares even once their own dictionary was correct.
    # 190 of Vice City's models depend on it. Copy each missing texture into the dictionary that is
    # asked for it, so every dictionary is self-contained by the time San Andreas sees it and
    # nothing has to be resolved at run time. GTA III needs far less of this than Vice City,
    # because its models name generic.txd in their IDE txd column and it is packed above as their
    # own dictionary rather than borrowed from.
    #
    # The pool is the .dat's global dictionaries first, because that is where the source game looks,
    # then every dictionary of the archive itself. GTA III ships a second archive as well,
    # models/txd.img, which CStreaming::Init prefers over gta3.img when it is there; it is generated
    # from gta3.img by CreateTxdImageForVideoCard and holds the same 717 dictionary names in
    # whatever formats the card that generated it wanted, so it is only worth opening if a name is
    # still unaccounted for after the two pools above.
    def borrow(pool):
        """Copy into each dictionary the textures its models ask for and it does not hold."""
        taken, missing = 0, set()
        for model, refs in dff_refs.items():
            part = txd_parts.get(model_txd.get(model, ""))
            if not part:
                continue
            have = {n.lower() for n, _c in part[1]}
            for ref in refs:
                if ref.lower() in have:
                    continue
                found = pool.get(ref.lower())
                if found:
                    part[1].append(found)
                    have.add(ref.lower())
                    taken += 1
                else:
                    missing.add(ref)
        return taken, missing

    fallback = {}
    for globalname, _blob in ([] if lite else loose):
        for texname, chunk in (txd_parts.get(renamed(globalname).lower(), [None, []])[1]):
            fallback.setdefault(texname.lower(), (texname, chunk))
    for _key, chunks, _sources in txd_parts.values():
        for texname, chunk in chunks:
            fallback.setdefault(texname.lower(), (texname, chunk))

    borrowed, unresolved = borrow(fallback)
    second_img = find_file(src, "models/txd.img") if unresolved and not lite else None
    second_dir = find_file(src, "models/txd.dir") if second_img else None
    if second_dir:
        with open(second_img, "rb") as f:
            second = f.read()
        extra = {}
        for name, off, size in read_img_v1(second_img, second_dir):
            if not name.lower().endswith(".txd") or size == 0:
                continue
            chunks, _done, _bad = txd_texture_chunks(second[off:off + size])
            for texname, chunk in (chunks or []):
                extra.setdefault(texname.lower(), (texname, chunk))
        again, unresolved = borrow(extra)
        if again:
            print("copied %d further texture(s) out of the source's second archive, models/txd.img"
                  % again)
        borrowed += again
    if borrowed:
        print("copied %d texture(s) into the dictionaries that reference them but did not hold "
              "them; the source game resolves those through the dictionaries its .dat loads "
              "globally" % borrowed)
    if unresolved:
        print("warning: %d texture name(s) a model references exist nowhere in the source: %s"
              % (len(unresolved), ", ".join(sorted(unresolved)[:8])))

    # Vice City's own gta3.dir carries the same TXD name more than once: generic.txd is in there
    # both as the dictionary holding the tree leaf textures and as a second one holding ivy and road
    # surfaces. A name is one streaming slot, so packing both means the second silently replaces the
    # first and every model bound to it renders untextured - white leaves, in the case that found
    # this. Merge same-named dictionaries into one instead, keeping the first texture of any name.
    shared = []
    for key, chunks, sources in txd_parts.values():
        seen, unique = set(), []
        for texname, chunk in chunks:
            low = texname.lower()
            if low in seen:
                continue
            seen.add(low)
            unique.append((texname, chunk))
        if sources > 1:
            shared.append("%s (%d dictionaries, %d texture(s))" % (key, sources, len(unique)))
        packed.append((key, build_txd(unique)))
    if shared:
        print("merged %d texture dictionary name(s) the source archive carried more than once: %s"
              % (len(shared), ", ".join(sorted(shared))))

    if lite:
        print("%d radar tiles" % tiles)
    else:
        total = write_img_v2(os.path.join(out, region + ".img"), packed)
        print("%s.img: %d models/textures, %.1f MB; %d collision files, %d radar tiles, %d other entries dropped"
              % (region, len(packed), total / 1e6, cols, tiles, skipped))
        if col_faces_lit:
            print("collision: %d face(s) given San Andreas daylight, which is where a ped standing "
                  "on them takes its lighting from - both source games leave it at zero"
                  % col_faces_lit)
        print("textures: %d raster(s) in %d TXD(s) rewritten from D3D8 to San Andreas' D3D9%s"
              % (tex_done, txds, "" if not tex_failed else
                 "; %d raster(s) in %d dictionar%s left alone and will render white (%s)"
                 % (tex_failed, len(failed_txds), "y" if len(failed_txds) == 1 else "ies",
                    ", ".join(sorted(failed_txds)[:6]))))
        print("models: %d of %d clump(s) rebuilt as San Andreas RenderWare 3.6.0.3%s"
              % (dff_done, dffs, "" if not failed_dffs else
                 "; %d could not be read and went through as %s came (%s)"
                 % (len(failed_dffs), "it" if len(failed_dffs) == 1 else "they",
                    "; ".join(sorted(failed_dffs)[:6]))))
        if dff_merged:
            print("meshes: %d geometry/geometries had identical materials merged, so the D3D "
                  "pipeline instances one mesh per material instead of one per face" % dff_merged)
    if dropped_dups:
        print("dropped %d later cop%s of a .dff name the archive already carries (the source game "
              "binds only the first): %s" % (len(dropped_dups), "y" if len(dropped_dups) == 1 else "ies",
                                             ", ".join(sorted(dropped_dups))))
    if renames:
        counts = {}
        for why in reasons.values():
            counts[why] = counts.get(why, 0) + 1
        print("named %d model/texture entries under the %s tag: %s"
              % (len(renames), tag, ", ".join("%d %s" % (n, w) for w, n in sorted(counts.items(), key=lambda kv: -kv[1]))))
        with open(os.path.join(out, "names.txt"), "w", encoding="latin-1") as f:
            f.write("# new name, original name, why - written by tools/import_city.py; the loader reads it\n")
            for low in sorted(renames, key=lambda k: renames[k].lower()):
                ext = os.path.splitext(low)[1]
                f.write("%s%s\t%s\t%s\n" % (renames[low], ext, low, reasons[low]))

    for rel in ([] if lite else col_rel):
        p = find_file(src, rel)
        if not p:
            print("  COLFILE %s not found" % rel)
            continue
        with open(p, "rb") as f:
            blob = f.read()
        with open(os.path.join(out, "col", os.path.basename(p)), "wb") as f:
            data, faces = rename_col(blob, renames)

            f.write(data)

            col_faces_lit += faces
        cols += 1

    ide_out, ipl_out = [], []
    ide_drop_flags = III_DROP_FLAGS if dat_name.lower() == "gta3.dat" else 0
    if lite:
        ide_rel, ipl_rel = [], []
    for rel in ide_rel:
        p = find_file(src, rel)
        if not p:
            print("  IDE %s not found" % rel)
            continue
        with open(p, "r", encoding="latin-1") as f:
            text = f.read()
        name = os.path.basename(p).lower()
        with open(os.path.join(out, "ide", name), "w", encoding="latin-1") as f:
            f.write(rename_ide(text, renames, ide_drop_flags))
        ide_out.append(name)
    for rel in ipl_rel:
        p = find_file(src, rel)
        if not p:
            print("  IPL %s not found" % rel)
            continue
        with open(p, "r", encoding="latin-1") as f:
            text = f.read()
        name = os.path.basename(p).lower()
        with open(os.path.join(out, "ipl", name), "w", encoding="latin-1") as f:
            f.write(rename_ipl(text, renames) if renames else text)
        ipl_out.append(name)
    if not lite:
        print("%d IDE, %d IPL, %d collision files" % (len(ide_out), len(ipl_out), cols))

    copied, zones_have = [], []
    # GTA III has neither navig.zon nor info.zon: its zone boxes are in data/gta3.zon, which the
    # .dat loads with an IPL line of its own. The lines are the same nine fields Zones.cpp already
    # reads, with the GXT key optional at the end, so a zone file travels unchanged and lands on
    # the region's Zones key rather than its Ipl key - the IPL loader would only count it and
    # move on. The two named paths are a safety net for a .dat that does not list them.
    for rel, dst in ([("text/american.gxt", "american.gxt"), ("data/waterpro.dat", "waterpro.dat"),
                      ("data/navig.zon", "navig.zon"), ("data/info.zon", "info.zon")] +
                     [(z, dat_base(z)) for z in zon_rel]):
        p = find_file(src, rel)
        if not p or dst in copied:
            continue
        with open(p, "rb") as f, open(os.path.join(out, dst), "wb") as g:
            g.write(f.read())
        copied.append(dst)
        if dst.endswith(".zon"):
            zones_have.append(dst)
    print("copied: %s" % ", ".join(copied))

    # Which zone file the Zones key should name. San Andreas has 380 navigation-zone slots and
    # vanilla fills 379, so a region has room for a handful of zones, not for every file the source
    # game loads - Vice City's info.zon alone is 378 lines. navig.zon is the one carrying the area
    # names a player reads off the map, so prefer it where the source has one; GTA III has only
    # gta3.zon and it is the same nine-field format.
    zone_out = [z for z in zones_have if z == "navig.zon"] or zones_have[:1]

    # Where the source map's own (0,0) sits in San Andreas. Vice City's comes from the VC2SAMP
    # placement, Liberty City's from LC2SAMP (spicybung's filterscript, itself a rework of Kye's):
    # every one of its CreateObject calls is the III IPL position plus a constant, and matching
    # 1102 model names across the two gives SA = III + (-5350, -200, 0) with no scatter at all.
    # The two landmasses then sit either side of San Andreas without touching it or each other.
    ORIGIN = {"vc_": (5998.5, 1052.5, -6.0), "iii_": (-5350.0, -200.0, 0.0)}
    origin_x, origin_y, origin_z = ORIGIN[prefix]

    base = "MapExpander\\%s\\" % region
    lines = ["[%s]" % region,
             "TileFolder   = %stiles" % base,
             "FilePattern  = radar%02d.png",
             "TileSize     = 500",
             "TilesX       = 8",
             "TilesY       = 8",
             "GridMinX     = -2000",
             "GridMaxY     = 2000",
             "WorldOriginX = %g" % origin_x, "WorldOriginY = %g" % origin_y,
             "WorldOriginZ = %g" % origin_z]
    if "waterpro.dat" in copied:
        lines += ["Water        = %swaterpro.dat" % base, "WaterXOffset = %d" % (400 if prefix == "vc_" else 0)]
    if zone_out:
        lines.append("Zones        = " + ", ".join(base + n for n in zone_out))
    lines += ["RegionName   = %s" % ("Vice City" if prefix == "vc_" else "Liberty City")]
    if not lite:
        lines += ["Img          = %s%s.img" % (base, region),
                  "Ide          = " + ", ".join(base + "ide\\" + n for n in ide_out),
                  "Ipl          = " + ", ".join(base + "ipl\\" + n for n in ipl_out),
                  "Col          = %scol\\*.col" % base,
                  "Names        = %snames.txt" % base]
    # [World] Gxt replaces San Andreas' own strings wholesale rather than merging by region, so a
    # second city's text would take the first one's place. Left commented out for that reason.
    lines += ["", "; [World] - one GXT at a time; this one replaces San Andreas' strings, not just",
              "; the region's, so uncomment it only if the source game's text is what you want.",
              "; Gxt = %samerican.gxt" % base]
    with open(os.path.join(out, "region.ini"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote %s - paste it into MapExpander.ini" % os.path.join(out, "region.ini"))


if __name__ == "__main__":
    main()
