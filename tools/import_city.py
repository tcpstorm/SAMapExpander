#!/usr/bin/env python3
"""Turn a GTA III / Vice City install into a MapExpander region folder.

    import_city.py <source game dir> <SA game dir> [--region=vicecity] [--lite] [--no-rename]
                                                   [--parked-cars-only]

Writes <SA game dir>/MapExpander/<region>/ with

    <region>.img   every .dff/.txd from the source gta3.img, repacked as an SA "VER2" archive
    col/           every .col from the archive plus the COLFILE entries of the source .dat
    ide/, ipl/     the IDE and IPL files the source .dat lists, in its order
    tiles/         radar00.png .. radarNN.png decoded from the radar TXDs
    object.dat     the source game's object.dat, model names renamed and its collision damage
                   effects mapped onto San Andreas' - what makes a prop breakable rather than
                   scenery; a model with no line here is placed as a building and never reacts
    american.gxt, waterpro.dat, and the source's zone file (navig.zon/info.zon, or III's gta3.zon)
    zone_names.txt the display name of every zone the region creates, under its namespaced key
    weapons.dat    the guns the source game has and San Andreas does not, each over the San Andreas
                   weapon nearest to it, with ide/weapons.ide carrying their meshes (Vice City only)
    pickups.dat    the world pickups the source game's main script places, copied from
                   tools/pickups_<region>.dat beside this script
    pednodes.dat   the source game's own pedestrian path graph, in its own coordinates: where a
                   pedestrian may stand and where they may walk next. San Andreas cannot hold
                   these - a path node position is an int16 times 0.125 and reaches only +/-4096 -
                   so the plugin keeps the graph itself
    pedgrp.dat     who walks where: the source game's ped groups and the zone boxes they belong
                   to, each of its ped models paired with the San Andreas model standing in for it
    parkedcars.dat where the source game parked a car, mined from the CREATE_CAR_GENERATOR calls
                   in its main.scm, each vehicle paired with the San Andreas model standing in for
                   it. These are the plugin's own spawns, not San Andreas car generators
    region.ini     the [region] keys to paste into MapExpander.ini, lists in .dat order, with the
                   origin the conversion this project follows places that city at

The dictionaries a source .dat loads from disk rather than out of the archive - III's
models/generic.txd and models/misc.txd, named by TEXDICTION in data/default.dat - travel with the
region too: 608 of III's models are bound to `generic`, which is also the name of a San Andreas
dictionary, so it has to be packed and renamed like everything else.

--lite writes only the last three groups (tiles, text/water/zones, region.ini): what a region
needs when something else - a server, a map mod - already places the buildings.

--parked-cars-only writes parkedcars.dat into a region folder that already exists and adds the
one key to its region.ini, touching nothing else. A full import rewrites the archive, the
collision and every IDE and IPL; adding one data file should not cost that.

Every model and texture dictionary is renamed under a two-character tag for the source game
("v_" for Vice City, "l_" for III) - in the archive, the IDE model/txd columns, the IPL inst
lines and the collision entry names, all together - and names.txt records what each was called.
A name is kept readable when it fits the game's 19-character field and takes a short hashed
tail when it does not; every name is checked to be unique, and to share no CKeyGen key with
anything San Andreas has, before the archive is written. See docs/asset-namespacing.md.

The GXT keys and the zone labels that name them go the same way, under a one-character tag
("V", "L") because a key is only seven characters wide - readable where that fits, a hashed
"V~XXXXX" where it does not - so that a donor game's text adds to San Andreas' strings rather
than silently replacing 694 of them. The emitted american.gxt is the source table with its keys
replaced, the .zon files are rewritten to match, and names.txt records both.
--no-rename skips all of it, which is only safe for a source that clashes with nothing.
Runs on plain Python; Pillow is used for the PNGs if it is installed, else a built-in writer.
"""
import collections
import math
import time
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


def txd_chunks_asis(data):
    """[(texture name, chunk bytes)] from a dictionary already in San Andreas' own format.

    txd_texture_chunks above rewrites a III/VC raster into San Andreas' D3D9 form; this one is for
    reading San Andreas' own dictionaries, where the chunk is already right and copying it verbatim
    is both correct and lossless.
    """
    out = []
    for kind, s, e in rw_sections(data, 0, len(data)):
        if kind != RW_TEX_DICTIONARY:
            continue
        for k, ts, te in rw_sections(data, s, e):
            if k != RW_TEXTURE_NATIVE:
                continue
            for k2, bs, _be in rw_sections(data, ts, te):
                if k2 != RW_STRUCT:
                    continue
                name = data[bs + 8:bs + 40].split(b"\0")[0].decode("ascii", "replace")
                out.append((name, data[ts - 12:te]))
                break
        break
    return out


def rename_texture_chunk(chunk, new_name):
    """A copy of a TextureNative chunk carrying a different texture name.

    The chunk is always <TextureNative header 12><Struct header 12><struct body>, and RenderWare
    keeps the 32-byte texture name at body+8 - so the name is at offset 32 of the chunk, whether the
    chunk was built by txd_texture_chunks or lifted whole out of a San Andreas dictionary.
    """
    kind, _size, _ver = struct.unpack_from("<III", chunk, 0)
    inner, _isize, _iver = struct.unpack_from("<III", chunk, 12)
    if kind != RW_TEXTURE_NATIVE or inner != RW_STRUCT:
        raise RwError("not a texture chunk with its struct first")
    name = new_name.encode("latin-1")[:31].ljust(32, b"\0")
    return chunk[:32] + name + chunk[64:]


# ---------------------------------------------------------------- the weapon HUD icon
#
# CHud::DrawWeaponIcon (0x58D7D0) does not look a texture up by name. It takes the weapon's model
# info - GetWeaponInfo(type, STD)->m_nModelId1 through CModelInfo::ms_modelInfoPtrs - reads the
# dictionary bound to that model (m_nTxdIndex at +0x0A, through CTxdStore's pool at 0xC8800C), and
# then at 0x58D83E does:
#
#     push "ICON"                       ; the literal at 0x866C54
#     push [modelInfo + 4]              ; CBaseModelInfo::m_nKey, the model NAME's key
#     call CKeyGen::AppendStringToKey   ; 0x53CF70
#     call RwTexDictionaryFindNamedTexture-by-key   ; 0x734E50, which keys each texture's own name
#                                                   ; through CKeyGen::GetUppercaseKey (0x53CF30)
#
# GetUppercaseKey folds toupper(c) of every character into a standard reflected CRC-32 (the table at
# 0x8CD068 is exactly that table); AppendStringToKey continues the same fold without the toupper,
# which is why the literal is spelled in capitals. So the key of "modelname" continued with "ICON"
# is the key of the single string "modelnameicon", in any case - which is how San Andreas' own
# desert_eagle.txd gets away with desert_eagleicon while sniper.txd spells it SNIPERicon.
# keygen_key/keygen_append below reproduce both so that the name we write can be checked against
# the key the game will actually compute, rather than assumed from the spelling.
#
# CHud::DrawCrossHairs (0x58E888) does the same thing with the literal "CROSSHAIR", which is the
# scoped weapon's reticle - sniper.txd holds SNIPERcrosshair.
#
# Where the art comes from: Vice City already ships the icon in the weapon's own dictionary, named
# after the weapon and nothing else - python.txd holds `python` (64x64, with alpha) beside the gun's
# own skin `gun_python` (64x64, no alpha), exactly the shape San Andreas' icons have. So the icon is
# already there under a name San Andreas will never ask for, and all that is needed is a second copy
# under the name it will. Where the source game has no such texture - and for the scoped reticle,
# which neither III nor Vice City keeps in the weapon's dictionary at all - the donor's own San
# Andreas art is copied instead. A weapon that can get neither is left without an icon and named in
# the log; a blank or a wrong-shaped texture would be worse than none.

CRC32_TABLE = []
for _i in range(256):
    _c = _i
    for _bit in range(8):
        _c = (_c >> 1) ^ (0xEDB88320 if _c & 1 else 0)
    CRC32_TABLE.append(_c)


def keygen_key(name):
    """CKeyGen::GetUppercaseKey (0x53CF30): toupper each character, fold as reflected CRC-32."""
    key = 0xFFFFFFFF
    for ch in name:
        key = (key >> 8) ^ CRC32_TABLE[(ord(ch.upper()) ^ key) & 0xFF]
    return key


def keygen_append(key, text):
    """CKeyGen::AppendStringToKey (0x53CF70): the same fold, without the toupper."""
    for ch in text:
        key = (key >> 8) ^ CRC32_TABLE[(ord(ch) ^ key) & 0xFF]
    return key


def hud_texture_name(model, suffix):
    """The texture CHud will look for, checked against the key it will actually build.

    Not a spelling convention: the name is only right if it hashes to the key the game computes,
    so that is what is asserted here rather than eyeballed.
    """
    name = model + suffix.lower()
    assert keygen_key(name) == keygen_append(keygen_key(model), suffix), \
        "%s does not hash to the key CHud builds for %s + %s" % (name, model, suffix)
    return name


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


def area_placements(text):
    """[(area, x, y, z)] for every inst line of an IPL that names an interior.

    Area 0 is the outside world and 13 is "everywhere", so neither describes a room; both are left
    out. See interior_boxes for what this is gathered for.
    """
    out, section = [], None
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        low = s.lower()
        if low in ("inst", "end") or (len(low) == 4 and low.isalpha()):
            section = low if low != "end" else None
            continue
        if section != "inst":
            continue
        t = [x.strip() for x in s.split(",")]
        if len(t) < 6:
            continue
        try:
            area, x, y, z = int(t[2]), float(t[3]), float(t[4]), float(t[5])
        except ValueError:
            continue
        if 0 < area < 256 and area != 13:
            out.append((area, x, y, z, t[1].lower()))
    return out


# A group smaller than this is a stray rather than a room, and gets no box.
MIN_INTERIOR_PIECES = 3
# A room whose floor is above this is not somewhere the player can walk to. Vice City's three
# stadium events are arenas floating at z 248..326 over the Hyman Memorial Stadium, which the game
# only ever teleported the player into from a mission; their collision is up there either way, but
# switching one on because someone flew through that airspace would black out the whole world. Those
# boxes are still written, commented out, so anyone who wants them can see them and uncomment.
MAX_REACHABLE_FLOOR = 120.0


def interior_boxes(placements, radius=90.0, slack=2.0):
    """A box per interior, worked out from where that interior's own pieces are.

    The point of this is that nobody should have to measure a room by hand. Vice City's interiors
    sit at real coordinates inside the buildings and every piece of one carries its area code, so
    the extent of an area's placements IS the room - the data already says where it is, and the
    importer is the thing holding that data.

    What stops a plain bounding box working is that an area is not always one room. Some carry
    pieces that are nowhere near it: the Malibu's area also holds a fake skyline out to x 575 that
    exists to be seen through the windows, area 2 holds two island LODs Vice City mislabelled, and
    a couple of areas hold cutscene backdrops parked well outside. Bound those together with the
    room and the box swallows streets - which is exactly how the police station came out six units
    too tall and the width of the block.

    So group each area's placements by proximity first and keep the crowd: seed at every placement,
    take everything within `radius` horizontally, and the largest such group is the room. Anything
    left over is either a second room of the same interior, kept when there is enough of it to be
    real, or a stray, dropped. `slack` is then the only fudge left, and it is small and uniform: a
    placement's coordinates are its model's origin, so the walls stand a little outside the
    outermost origin. It is not tuning; the plugin's own margins handle the doorway.
    """
    by_area = collections.defaultdict(list)
    for area, x, y, z, name in placements:
        by_area[area].append((x, y, z, name))
    boxes = []
    for area in sorted(by_area):
        rest = by_area[area]
        while len(rest) >= MIN_INTERIOR_PIECES:
            best = max((([p for p in rest if abs(p[0] - s[0]) <= radius and abs(p[1] - s[1]) <= radius])
                        for s in rest), key=len)
            keep = set(best)
            rest = [p for p in rest if p not in keep]
            # A group of one or two is a stray the clustering could not attach to anything - a lone
            # ceiling fixture, a prop left over the road - and a box round it is a box round nothing.
            if len(best) < MIN_INTERIOR_PIECES:
                continue
            # Some of what carries a room's area code is not the room, and must not be allowed to
            # stretch the box. The Malibu Club's area also holds club_exterior01..12, a fake skyline
            # standing out in the street to be seen through the club's windows; the police station
            # has cop_fake_ext, the lawyers' office a pair of cutscene backdrops. Bound those with
            # the room and the box reaches out over the pavement, which is the one failure that
            # matters, because there it blacks out the world. They name themselves, so the box is
            # measured from the room's own pieces and the scenery is only allowed to come along.
            room = [p for p in best if not ("exterior" in p[3] or "_ext" in p[3] or "fake" in p[3])]
            if len(room) < MIN_INTERIOR_PIECES:
                continue
            xs, ys, zs = [p[0] for p in room], [p[1] for p in room], [p[2] for p in room]
            boxes.append((area, min(xs) - slack, min(ys) - slack, min(zs) - slack,
                          max(xs) + slack, max(ys) + slack, max(zs) + slack, len(room)))
    return boxes


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


# ---------------------------------------------------------------- object.dat
#
# data/object.dat is the file that decides whether a placed model is a lump of scenery or a thing
# the world can push, break and smash. All three games read it the same way: each line names a
# MODEL, and the loader looks that name up and hangs the line's index on the model info. An IPL
# `inst` line whose model carries such an index is built as a dynamic object (a CDummyObject that
# the streamer promotes to a real CObject when the player comes near); one that does not is built
# as a CBuilding, which is static geometry and can never take damage. So a region imported without
# an object.dat has no breakable glass, no traffic cones to knock over, no smashable crates - not
# because anything is missing from the models, but because every one of them came in as a building.
#
# The three column sets are not the same, and they have to be established before anything can be
# mapped between them:
#
#   III  A..K   name mass turnMass airRes elasticity percentSubmerged uprootLimit colDamageMult
#               colDamageEffect specialColResponse cameraAvoid                          (11 fields)
#   VC   A..K   the same eleven, in the same order and the same units
#   SA   A..L + FX + breakable
#               the same eleven, then L causesExplosion, then FX_TYPE FX_OFFSET(x,y,z) FX_NAME
#               (17 fields), and if the damage effect is 200 or 202 seven more breakable columns:
#               smashMultiplier breakVelocity(x,y,z) breakVelocityRand gunBreakMode sparks (24)
#
# A..K therefore carry across verbatim - the meaning of every one of them is unchanged - and San
# Andreas' extra columns get the values its own file uses for an object with no effect attached:
# no explosion, FX_TYPE 0 and the FX name `none`. (The header in San Andreas' object.dat says the
# name must read "noname" when FX_TYPE is 0; its own 928 such lines all write `none`, so that is
# what is written here.)
#
# What does need translating is column I, the collision damage effect, because the three games
# number it differently and III and Vice City spend most of their numbering on effects hardcoded to
# one model each - "smash this particular cardboard box completely". San Andreas has no handler for
# any of those, so they collapse onto its generic smash. The full table is COLLISION_DAMAGE_EFFECT
# below; the important cases are that III's 3/4 are Vice City's 20/21 (San Andreas agrees with Vice
# City), and that every 50-and-above "smash_<some model>_completely" becomes San Andreas' 20.
#
# Two San Andreas effects are deliberately never emitted: 200 (breakable) and 202 (breakable then
# removed), which is how San Andreas' own lamp posts and palms come apart. Those need break-away
# geometry inside the model that III and Vice City's DFFs simply do not carry, so asking for them
# would be asking the game to shatter something that has no pieces. Because of that no imported
# line ever needs the seven breakable columns, and every line written here is the 17-field form.
#
# Breakable glass needs no translation at all, which is worth saying plainly because it is the
# thing most obviously missing from an imported city. Glass is not driven by object.dat's damage
# effect: it is the IDE flag word, 0x200 "code glass" and 0x400 "artist glass", and Vice City and
# San Andreas number those bits identically - San Andreas' own `wglasssmash` (0x404) and
# `od_copwindows` (0x204) are the same models with the same flags as Vice City's, and both games
# give them the same object.dat line, damage effect 0 and special collision response 4. The flags
# already survive the import untouched. What was missing was the object.dat line that makes the
# pane a dynamic object in the first place, because CGlass only ever works on a CObject.

# Column I as III and Vice City write it -> column I as San Andreas reads it.
#
#   0   none                              -> 0   none
#   1   change_model                      -> 1   change_model (San Andreas ships 39 lines of this
#                                                on models that are not flagged damageable, so it
#                                                is safe on ours, which never are either)
#   2   split_model                       -> 0   San Andreas has no equivalent; neither shipped
#                                                file uses it, so this is theory only
#   3   VC smash_and_damage_trafficlights -> 20  smash_completely (the object's own half of it;
#       III smash_completely                     San Andreas has no traffic-light damage state)
#   4   III change_then_smash             -> 21  change_then_smash
#   20  VC smash_completely               -> 20
#   21  VC change_then_smash              -> 21
#   50  smash_cardbox_completely          -> 20
#   51  smash_yellow_target_completely    -> 20
#   60  smash_woodenbox_completely        -> 20
#   70  smash_trafficcone_completely      -> 20
#   80  smash_barpost_completely          -> 20
#   91..95 smash_newstandnew1..5          -> 20
#   100 smash_blackbag                    -> 20
#   110 smash_vegpalm                     -> 20
#   120 burst_beachball                   -> 20
#   131 smash_beachlounge_wood            -> 20
#   132 smash_beachlounge_towel           -> 20
COLLISION_DAMAGE_EFFECT = {0: 0, 1: 1, 2: 0, 3: 20, 4: 21, 20: 20, 21: 21}
COLLISION_DAMAGE_EFFECT.update(dict.fromkeys((50, 51, 60, 70, 80, 91, 92, 93, 94, 95,
                                              100, 110, 120, 131, 132), 20))

# III and Vice City's column J, the special collision response case, is 0 none, 1 lampost,
# 2 smallbox, 3 bigbox, 4 fencepart - which is exactly San Andreas' 0..4. San Andreas adds 5
# grenade, 6 swing door, 7 lock door, 8 hanging, 9 pool ball, none of which a source value can
# land on, so the column travels unchanged and anything outside 0..4 is refused rather than guessed.
SPECIAL_COL_RESPONSE_MAX = 4

# The 17 columns San Andreas reads, with the seven extra breakable ones deliberately absent.
OBJECT_DAT_HEADER = [
    "; San Andreas object.dat for a MapExpander region, written by tools/import_city.py.",
    ";",
    "; Every line is one of the source game's own object.dat lines with its model name replaced by",
    "; the name the region carries it under (see names.txt) and its collision damage effect mapped",
    "; onto San Andreas' numbering. Lines naming a model this region does not define are left out.",
    ";",
    "; A  Name           E  Elasticity        I  CollisionDamageEffect   FX_TYPE   0, never used",
    "; B  Mass           F  PercentSubmerged  J  SpecialColResponse      FX_OFFSET 0 0 0",
    "; C  TurnMass       G  UprootLimit       K  CameraAvoid             FX_NAME   none",
    "; D  AirResistance  H  CollDamageMult    L  CausesExplosion (0)",
    ";",
]


def read_object_dat(path):
    """The (name, [B..K]) rows of a III or Vice City object.dat, in file order.

    Comments start with `;`, and the file ends with a `*` line the parser stops at. Vice City's
    ships one damaged line - `parkingmeter` has a second row's worth of numbers run onto the end of
    it - so a row with more than eleven columns keeps the first eleven, which is what the source
    game's own sscanf does with it too.
    """
    rows, malformed = [], []
    for line in open(path, "r", encoding="latin-1"):
        s = line.strip()
        if not s or s[0] in ";#":
            continue
        if s[0] == "*":
            break
        tok = [t for t in re.split(r"[,\s]+", s) if t]
        if len(tok) < 11:
            malformed.append(s)
            continue
        rows.append((tok[0], tok[1:11]))
    return rows, malformed


def decimal(value):
    """A float with its point kept: San Andreas' own file never writes a bare integer here."""
    text = "%g" % value
    return text if "." in text or "e" in text else text + ".0"


def convert_object_dat(path, renames, models):
    """San Andreas object.dat text for the rows whose model this region actually defines.

    `renames` is the archive's rename table (lowercase `name.dff` -> new stem) and `models` the set
    of lowercase model names the region's objs/tobj IDE lines declare. A row is kept only when its
    name is in `models`: object data attaches to a model info by name, so a row for a weapon or a
    pickup - the fifteen-odd rows at the top of both files - would find nothing to attach to, and
    would spend one of San Andreas' fixed object-data slots doing it.

    Returns (text, kept, skipped, notes).
    """
    rows, malformed = read_object_dat(path)
    notes = []
    if malformed:
        notes.append("%d malformed line(s) ignored (%s)"
                     % (len(malformed), "; ".join(m[:40] for m in malformed[:3])))

    out, kept, skipped, seen, unmapped = list(OBJECT_DAT_HEADER), 0, 0, set(), {}
    for name, fields in rows:
        low = name.lower()
        if low not in models:
            skipped += 1
            continue
        if low in seen:
            # GTA III lists `vheistlocdoor` twice. The source loader binds the model to whichever
            # row it read last; keeping only the first is the same object with one slot spent.
            notes.append("duplicate row for %s ignored" % name)
            continue
        seen.add(low)

        try:
            effect = int(fields[7])
            response = int(fields[8])
            camera = int(fields[9])
            floats = [float(f) for f in fields[:7]]
        except ValueError:
            notes.append("unreadable numbers on %s, line left out" % name)
            continue

        sa_effect = COLLISION_DAMAGE_EFFECT.get(effect)
        if sa_effect is None:
            unmapped[effect] = unmapped.get(effect, 0) + 1
            sa_effect = 0
        if not 0 <= response <= SPECIAL_COL_RESPONSE_MAX:
            notes.append("special collision response %d on %s is not one San Andreas shares, "
                         "written as 0" % (response, name))
            response = 0
        camera = 1 if camera else 0

        out.append("%-22s %s%4d,%3d,%3d,   0,   0,   0.0, 0.0, 0.0,  none"
                   % (renames.get(low + ".dff", name) + ",",
                      "".join("%-11s" % (decimal(f) + ",") for f in floats),
                      sa_effect, response, camera))
        kept += 1
    for effect, count in sorted(unmapped.items()):
        notes.append("collision damage effect %d is in neither game's documented set (%d line(s)), "
                     "written as 0" % (effect, count))
    out.append("* ;end of file")
    return "\n".join(out) + "\n", kept, skipped, notes


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


# ---------------------------------------------------------------- GXT keys and zone labels
#
# The same problem one field narrower. A region's american.gxt is merged whole into San Andreas'
# text lookup, and where a Vice City key spells the same word as a San Andreas one the donor's
# string silently takes its place: measured over the retail files, 1,134 of Vice City's 4,707 keys
# and 717 of GTA III's 2,649 already exist in San Andreas, and 694 respectively 468 of those carry
# different text - "Micro SMG" becomes "Uz-1", "Berkley's RC Van" becomes "Top Fun", the taxi
# tutorial and a pause-map help line are rewritten, and so are several hundred credits lines.
# Adding a second region makes it worse, because the two donors collide with each other as well.
#
# So the keys are namespaced exactly as the model and dictionary names are, only the budget is
# 7 characters rather than 19. Two places enforce it and both were read out of the exe:
#
#   * CTheZones::CreateZone (0x5728A0) upper-cases the caller's label in place and then calls
#     strncpy(dst, src, 7) twice - 0x572935 for the info label, 0x572978 for the text label -
#     writing the terminator itself at byte 7 (0x57294B, 0x57298E). A zone can therefore never be
#     looked up by more than seven characters.
#   * A III/VC TKEY entry is 12 bytes: a TDAT offset and an 8 byte, not necessarily terminated
#     name. Gxt.cpp reads at most seven characters out of it for the same reason.
#
# San Andreas itself never sees a key name: CText::Get hashes with CKeyGen::GetUppercaseKey and
# binary-searches the block by hash, and an SA-format GXT stores only that hash. The name is ours
# to choose, and names.txt records what each one was.
#
# The scheme:
#
#   1. A one-character tag per source game, "V" for Vice City and "L" for GTA III. Two characters
#      would leave five for the name and push almost everything into the hashed form; one leaves
#      six, which keeps 1,918 of Vice City's keys and 1,444 of III's fully readable - including
#      every zone label Vice City's navig.zon uses.
#   2. tag + the key when that fits in seven characters and the key is plain [A-Z0-9_].
#   3. Otherwise tag + "~" + five base-36 digits of the tagged key's CKeyGen key. The "~" is the
#      same marker the model names use for a hashed tail, and it is what makes the scheme safe
#      across regions without either import knowing about the other: a source key never contains
#      "~", so a hashed key can never equal a readable one, and two tags never equal each other.
#      The only collisions left to check are two hashed keys of the same region, and a renamed key
#      landing on a hash San Andreas already uses; both are checked before anything is written.
#
# Every reference to a key is rewritten with it. In an imported region there is exactly one class
# of them - the name column of a .zon line, which is the zone's GXT key - because the IDE 2dfx
# sections carry only light and attractor data, no III/VC IPL has a "zone" or "enex" section, and
# nothing else in the source data names a string. The keys the plugin registers itself (Gxt.cpp's
# MXTABLE sentinel and Zones.cpp's MXREG<n> region names) are reserved rather than renamed: they
# are ours already and nothing in San Andreas answers to them.

GXT_BUDGET = 7            # CTheZones::CreateZone's strncpy(dst, src, 7), and Gxt.cpp's key reader
GXT_TAG = {"gta_vc.dat": "V", "gta3.dat": "L"}
GXT_HASH_MARK = "~"       # never appears in a source key, so it separates the two forms for good
GXT_HASH_DIGITS = 5       # 36^5 = 60,466,176 - four orders of magnitude above a region's key count
GXT_RESERVED = {"MXTABLE"} | {"MXREG%d" % i for i in range(10)}
B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def base36(value, digits):
    out = ""
    for _ in range(digits):
        out = B36[value % 36] + out
        value //= 36
    return out


def gxt_pair(data, at):
    """(key block, key block size, TDAT block, TDAT size) for the pair of blocks starting at `at`.

    `at` may point at eight bytes of table name first, which is how Vice City writes every table
    but MAIN. Returns None when the blocks are not where the header says they are.
    """
    if data[at:at + 4] != b"TKEY" and data[at + 8:at + 12] == b"TKEY":
        at += 8
    if data[at:at + 4] != b"TKEY" or at + 8 > len(data):
        return None
    key_size = struct.unpack_from("<I", data, at + 4)[0]
    key_block = at + 8
    dat_header = key_block + key_size
    if data[dat_header:dat_header + 4] != b"TDAT" or dat_header + 8 > len(data):
        return None
    dat_size = struct.unpack_from("<I", data, dat_header + 4)[0]
    return key_block, key_size, dat_header + 8, dat_size


def gxt_tables(data):
    """(entry size, [pair, ...]) for a GXT file, whatever of the three formats it is.

    Entry size is 12 for GTA III and Vice City, whose TKEY entries carry a name, and 8 for San
    Andreas, whose entries carry only the CKeyGen hash.
    """
    if data[:4] == b"TKEY":                       # GTA III: no header, one table, 16 bit text
        pair = gxt_pair(data, 0)
        return 12, ([pair] if pair else [])
    if data[:4] == b"TABL":                       # Vice City: a TABL of named tables, then pairs
        size = struct.unpack_from("<I", data, 4)[0]
        pairs = []
        for e in range(8, 8 + size - 11, 12):
            pair = gxt_pair(data, struct.unpack_from("<I", data, e + 8)[0])
            if pair:
                pairs.append(pair)
        return 12, pairs
    at = 4                                        # San Andreas: version/bits header, hashed keys
    offsets = []
    if data[at:at + 4] == b"TABL":
        size = struct.unpack_from("<I", data, at + 4)[0]
        for e in range(at + 8, at + 8 + size - 11, 12):
            offsets.append(struct.unpack_from("<I", data, e + 8)[0])
        at = at + 8 + size
    pairs = []
    first = gxt_pair(data, at)
    if first:
        pairs.append(first)
    for off in offsets:
        if first and at <= off < first[2] + first[3]:
            continue                              # the main table, already taken
        pair = gxt_pair(data, off)
        if pair:
            pairs.append(pair)
    return 8, pairs


def gxt_key_names(data):
    """Every key name in a III/VC GXT, in file order, upper-cased, without repeats.

    Empty for a San Andreas-format file: there are no names in one, only hashes.
    """
    entry_size, pairs = gxt_tables(data)
    if entry_size != 12:
        return []
    names, seen = [], set()
    for key_block, key_size, _dat, _dat_size in pairs:
        for e in range(key_block, key_block + key_size - 11, 12):
            name = data[e + 4:e + 12].split(b"\0")[0].decode("latin-1").upper()
            if name and name not in seen:
                seen.add(name)
                names.append(name)
    return names


def gxt_strings(data):
    """{upper-cased key name: text} for a III/VC GXT, converted the way Gxt.cpp converts it.

    III and Vice City store 16 bit characters; San Andreas' font is 8 bit, and ASCII and the
    Latin-1 range pass through unchanged because the code points line up. Anything else becomes
    a question mark, which is exactly what the loader would show.
    """
    entry_size, pairs = gxt_tables(data)
    if entry_size != 12:
        return {}
    out = {}
    for key_block, key_size, dat, dat_size in pairs:
        for e in range(key_block, key_block + key_size - 11, 12):
            offset = struct.unpack_from("<I", data, e)[0]
            if offset >= dat_size:
                continue
            name = data[e + 4:e + 12].split(b"\0")[0].decode("latin-1").upper()
            chars, p = [], dat + offset
            while p + 1 < dat + dat_size:
                c = data[p] | (data[p + 1] << 8)
                if not c:
                    break
                chars.append(chr(c) if (c < 0x80 or 0xA0 <= c <= 0xFF) else "?")
                p += 2
            out.setdefault(name, "".join(chars))
    return out


def rewrite_gxt(data, renames):
    """The same GXT with every TKEY name replaced by its new one. Returns (bytes, [names missed]).

    Only the eight name bytes change: the TDAT blocks, the offsets into them and the table layout
    are untouched, so what the loader reads is the source file under a different set of keys.
    `missed` is None for a San Andreas-format source, which has no names to rewrite.
    """
    out = bytearray(data)
    entry_size, pairs = gxt_tables(data)
    if entry_size != 12:
        return bytes(out), None
    missed = []
    for key_block, key_size, _dat, _dat_size in pairs:
        for e in range(key_block, key_block + key_size - 11, 12):
            name = data[e + 4:e + 12].split(b"\0")[0].decode("latin-1").upper()
            new = renames.get(name)
            if not new:
                missed.append(name)
                continue
            out[e + 4:e + 12] = new.encode("latin-1").ljust(8, b"\0")
    return bytes(out), missed


def sa_gxt_keys(sa):
    """Every CKeyGen key San Andreas' own text already answers, from all of text/*.gxt.

    All five languages are read rather than american.gxt alone: they cost nothing, and a key that
    exists only in one of them is still San Andreas' and must not be taken over by a region.
    """
    keys = set()
    folder = find_file(sa, "text")
    if not folder or not os.path.isdir(folder):
        return keys
    for name in sorted(os.listdir(folder)):
        if not name.lower().endswith(".gxt"):
            continue
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        entry_size, pairs = gxt_tables(data)
        for key_block, key_size, _dat, _dat_size in pairs:
            for e in range(key_block, key_block + key_size - (entry_size - 1), entry_size):
                if entry_size == 12:
                    keys.add(ckey(data[e + 4:e + 12].split(b"\0")[0].decode("latin-1")))
                else:
                    keys.add(struct.unpack_from("<I", data, e + 4)[0])
    return keys


def gxt_fallback(tag, key, salt):
    """tag + ~ + five base-36 digits of the tagged key's CKeyGen key."""
    seed = tag + key + ("#%d" % salt if salt else "")
    return tag + GXT_HASH_MARK + base36(ckey(seed) % (36 ** GXT_HASH_DIGITS), GXT_HASH_DIGITS)


def make_gxt_renames(names, sa_keys, tag):
    """A new key for every source key, and why it looks the way it does.

    Both maps are keyed by the upper-cased source key, which is the only form anything compares:
    CKeyGen upper-cases, CTheZones::CreateZone upper-cases, and so does Gxt.cpp.
    """
    renames, reasons = {}, {}
    used_names, used_keys = set(), set()
    for name in names:
        if name in GXT_RESERVED:
            # A key the plugin registers itself. It is in our namespace already and San Andreas
            # answers to nothing by that name, so it stays as it is rather than being renamed
            # out of reach of the code that looks it up.
            renames[name] = name
            reasons[name] = "reserved for MapExpander, kept"
            continue
        plain = tag + name
        if len(plain) <= GXT_BUDGET and re.fullmatch(r"[A-Z0-9_]+", name):
            candidate, why = plain, "tag"
        else:
            candidate, why = gxt_fallback(tag, name, 0), "too long for the 7-character key"
        salt = 0
        while (candidate in used_names or ckey(candidate) in used_keys
               or ckey(candidate) in sa_keys or candidate in GXT_RESERVED):
            salt += 1
            candidate = gxt_fallback(tag, name, salt)
            why = "key clash, salted"
        used_names.add(candidate)
        used_keys.add(ckey(candidate))
        renames[name] = candidate
        reasons[name] = why
    return renames, reasons


def assert_gxt_namespace(renames, sa_keys):
    """Refuse to write a region whose text could take San Andreas' strings. This is the guarantee."""
    by_key = {}
    for old, new in renames.items():
        if len(new) > GXT_BUDGET:
            sys.exit("gxt namespacing: %s -> %s is %d characters, over the %d a zone label holds"
                     % (old, new, len(new), GXT_BUDGET))
        key = ckey(new)
        other = by_key.get(key)
        if other is not None and other != new:
            sys.exit("gxt namespacing: %s and %s share CKeyGen key %08X" % (other, new, key))
        if key in sa_keys and new not in GXT_RESERVED:
            sys.exit("gxt namespacing: %s has the same CKeyGen key as one San Andreas already uses" % new)
        by_key[key] = new


def zon_labels(text):
    """Every GXT key a .zon file names: the zone's own name column and its optional label column."""
    labels, section, seen = [], None, set()
    for line in text.splitlines():
        clean = line.split("#")[0].strip()
        if not clean:
            continue
        tok = [t for t in re.split(r"[,\s]+", clean) if t]
        if len(tok) == 1:
            section = None if tok[0].lower() == "end" else tok[0].lower()
            continue
        if section not in (None, "zone") or len(tok) < 9:
            continue
        for label in (tok[0], tok[9] if len(tok) >= 10 else None):
            if label and label.upper() not in seen:
                seen.add(label.upper())
                labels.append(label.upper())
    return labels


def rename_zon(text, renames):
    """A .zon file with its zone name and label columns replaced by their namespaced keys.

    Only those two columns are touched; the boxes travel unchanged, because Zones.cpp adds the
    region's origin to them itself.
    """
    out, section = [], None
    for line in text.splitlines(True):
        body = line.rstrip("\r\n")
        eol = line[len(body):]
        clean = body.split("#")[0].strip()
        tok = [t for t in re.split(r"[,\s]+", clean) if t]
        if len(tok) == 1:
            section = None if tok[0].lower() == "end" else tok[0].lower()
            out.append(line)
            continue
        if section not in (None, "zone") or len(tok) < 9:
            out.append(line)
            continue
        tok[0] = renames.get(tok[0].upper(), tok[0])
        if len(tok) >= 10:
            tok[9] = renames.get(tok[9].upper(), tok[9])
        out.append(", ".join(tok) + eol)
    return "".join(out)


# ---------------------------------------------------------------- weapons

# Which San Andreas weapon each Vice City gun is paired with, and why that one.
#
# Under the plugin's default RegionWeaponMode = Add the pairing is a DONOR, not a replacement: the
# Vice City gun becomes a weapon type of its own (San Andreas has ten free records at the end of
# aWeaponInfo, 70..79, and src/WeaponTypes.cpp widens the name table and the five switches on weapon
# type to reach them), and the donor only says which case in each of those switches it should land
# in and which of San Andreas' columns it should inherit - the slot, the animation group, the target
# range, the skill thresholds. San Andreas' own weapon is left exactly as it was.
#
# Under RegionWeaponMode = Replace the same file means the older thing: the gun is written over the
# donor, and that changes the donor for the whole game.
#
# So the donor is chosen for closeness of class, slot and role, which is what both modes want.
#
# `default` says whether the line is written live or commented out. Live are the ten guns Vice City
# has and San Andreas simply does not - which is exactly the ten free type numbers. Commented out
# are the two where both games have the same gun under two names, so a new type would buy nothing
# but a different mesh. (Under Replace the four melee lines trade away the katana, the knife, the
# shovel and the pool cue; comment them out if you are running that mode.)
def repoint_pickup_weapons(blob, prefix):
    """The mined pickup table, with each weapon column brought back into step with the weapon map.

    A mined line names the San Andreas weapon the pickup should give, worked out when the table was
    mined - `weapon, COUNTRYRIFLE, ...` for the .308 - and keeps what the source game called it in
    the comment: `# laser`. That pairing is exactly what VC_WEAPON_MAP decides, so the moment the
    map changes its mind the table is stale, and nothing notices.

    It went wrong the first time the map was corrected. The .308 moved from COUNTRYRIFLE to
    SNIPERRIFLE, because only the sniper rifle carries a camera mode and so only the sniper rifle
    scopes; the mined table still said COUNTRYRIFLE, and with no import paired to that weapon any
    more the pickup handed out San Andreas' own country rifle. Eight units from the Python it sits
    beside in Vice City, which is exactly how it was found.

    So the comment is the authority, not the column: where the source name in the comment is one
    this map knows, the column is rewritten to whatever that name is paired with now. A line naming
    a weapon the map has nothing to say about - the health, the armour, San Andreas' own sniper - is
    left exactly as it is.
    """
    if prefix != "vc_":
        return blob
    # The script's own name for a gun is not always the name the map gives it - main.scm calls the
    # .308 "laser" where the map calls it "LaserScope" - so match on either being a prefix of the
    # other, and only when exactly one entry matches, so a short name cannot land on the wrong gun.
    def donor_for(source):
        if len(source) < 3:
            return None
        # The IMPORT's own name, not the San Andreas weapon it borrows from. Naming the donor was
        # wrong in a way that only showed once two imports shared one: the .308 moved onto the
        # sniper rifle, and Vice City's plain sniper was already there, so both pickups read
        # SNIPERRIFLE and both handed out the .308. The import has a weapon type and a name of its
        # own, so ask for it by name and the question does not arise - and a pickup that really
        # should give San Andreas' weapon still says so, because its source name is not one of ours.
        hits = {name.upper() for name, _donor, _live, _why in VC_WEAPON_MAP
                if name.lower().startswith(source) or source.startswith(name.lower())}
        return hits.pop() if len(hits) == 1 else None

    out, changed = [], 0
    for line in blob.decode("latin-1").splitlines():
        body, _, comment = line.partition("#")
        fields = [f.strip() for f in body.split(",")]
        source = comment.strip().split()[0].lower() if comment.strip() else ""
        donor = donor_for(source) if len(fields) == 7 and fields[0].lower() == "weapon" else None
        if donor:
            if fields[1].upper() != donor:
                changed += 1
                fields[1] = donor
                body = ", ".join(fields) + "   "
                line = body + "#" + comment
        out.append(line)
    if changed:
        print("pickups.dat: %d weapon column(s) brought back into step with the weapon map" % changed)
    return ("\n".join(out) + "\n").encode("latin-1")


VC_WEAPON_MAP = [
    ("Python", "DESERT_EAGLE", True,
     "Vice City Python .357 - San Andreas' Desert Eagle is a re-skinned Python already: same fire "
     "offset to two decimals, the `python` animation group, and a texture dictionary called python"),
    ("Ruger", "AK47", True,
     "Vice City Ruger - a 30-round assault rifle; the AK-47 rather than the M4 because Vice City "
     "has an m4 of its own and San Andreas' is the same gun"),
    ("M60", "MINIGUN", True,
     "Vice City M60 - San Andreas has no light machine gun at all; the minigun is its other "
     "slot-7 continuous-fire heavy weapon"),
    ("StubbyShotgun", "SAWNOFF", True,
     "Vice City stubby shotgun - the short-barrelled close-range one, which is what the sawn-off is"),
    ("SilencedIngram", "TEC9", True,
     "Vice City silenced Ingram - a slot-5 submachine gun; San Andreas' silenced pistol is a "
     "pistol and not the same class"),
    ("LaserScope", "SNIPERRIFLE", True,
     "Vice City .308 laser-sighted sniper - the sniper rifle and not the country rifle, because "
     "the scope is the donor's: CPed::UpdateCameraWeaponModes gives SNIPERRIFLE camera mode 7 and "
     "COUNTRYRIFLE none at all, and CWeapon::Fire reads that mode to decide whether to call "
     "FireSniper. Over the country rifle it drew, aimed and fired but never zoomed in"),
    ("Machete", "KATANA", True,
     "Vice City machete - a long blade swung one-handed, so it takes the katana's slot, animation "
     "group and combo"),
    ("Cleaver", "KNIFE", True,
     "Vice City cleaver - the knife's slot and combo, though the stealth kill belongs to the knife "
     "and stays with it"),
    ("Hammer", "SHOVEL", True,
     "Vice City hammer - both are blunt tools on the baseball-bat combo"),
    ("ScrewDriver", "POOLCUE", True,
     "Vice City screwdriver - nothing in San Andreas stabs except the knife, so the pool cue's "
     "one-handed combo is the closest there is"),
    ("Uzi", "MICRO_UZI", False,
     "Vice City Uzi - the same weapon as San Andreas' micro uzi with a different mesh"),
    ("Colt45", "PISTOL", False,
     "Vice City Colt 45 - literally San Andreas' pistol: same model name, same clip, same damage"),
]


# The fire types CWeaponInfo::FindWeaponFireType (0x5BCF30) knows. A name outside this set makes it
# answer INSTANT_HIT without saying so, so anything else is written as "keep San Andreas' own".
SA_FIRE_TYPES = ("MELEE", "INSTANT_HIT", "PROJECTILE", "AREA_EFFECT", "CAMERA", "USE_3RD_PERSON_AIM")

# Vice City's weapon.dat, by column letter: A name, B fire type, C range, D firing rate, E reload,
# F ammunition, G damage, H speed, I radius, J life span, K spread, L/M/N fire offset, P animation,
# R/S/T animation loop, U/V/W animation 2 loop, X breakout, Y model, Z model 2, a flags, b slot.
VC_COL = dict(name=0, fire=1, range=2, ammo=5, damage=6, speed=7, radius=8, lifespan=9, spread=10,
              offx=11, offy=12, offz=13, anim=14, model=22, model2=23, flags=24, slot=25)
VC_COLUMNS = 26


# ---------------------------------------------------------------- restart points
#
# Where the source game puts the player back after they die or are arrested.
#
# Neither donor records this in a data file: both register their hospitals and police stations from
# main.scm, with ADD_HOSPITAL_RESTART and ADD_POLICE_RESTART and literal coordinates, exactly the
# way San Andreas does. So the script is walked here, once per import, the same way the world
# pickups were mined - and with the same rule, that a candidate has to earn its place rather than
# be believed because two bytes matched.
#
# THE OPCODES, established rather than assumed. Sweeping both donors' scripts for every 16-bit value
# under 0x0400 that is followed by exactly four type-tagged float parameters holding plausible map
# coordinates finds 0x016C and 0x016D in both games, and the same pair is what San Andreas' own
# main.scm uses - so the numbering did not move between the three games, only the parameter list
# did: III and Vice City take (x, y, z, heading), San Andreas takes a fifth int, the town.
#
# WHAT MAKES A HIT REAL. A raw byte match is worth little: 0x016C occurs 35 times in III's main.scm
# and only 3 of those are instructions. Four things are checked, and all four have to hold.
#
#   * The parameter list decodes exactly. Four parameters, every one carrying the float tag 0x06,
#     and no fifth float behind them. (A III float is a signed 16-bit value over 16; a Vice City
#     float is a 32-bit IEEE one. Reading either game the other way gives coordinates nowhere near
#     its map, which is its own check.)
#   * The hit CHAINS with another. Every kept instruction either ends exactly where another kept
#     one begins, or begins exactly where another ends. A misaligned read inside some unrelated
#     stream does not produce two of these back to back at exact instruction lengths, and the games
#     register their points in runs: III writes a hospital immediately followed by a police station,
#     three times - once per island - and Vice City writes four in a row, twice.
#   * The coordinates are on the map and the heading is an angle.
#   * The point stands inside one of the region's own zone boxes, read out of the same .zon file
#     the region ships. This is the check that turns a plausible number into a verified one, and in
#     GTA III it is emphatic: the Portland pair lands inside the zones III itself named HOSPI_1 and
#     COPS_1, and the Staunton hospital inside HOSPI_2.
#
# The counts that come out are 3 hospital and 3 police for GTA III - Portland, Staunton, Shoreside
# Vale, one of each per island - and 4 and 4 for Vice City. Running the same walk over San Andreas'
# own main.scm gives 8 and 7, which is what the executable's own arrays hold, so the decoder agrees
# with the game.
#
# THE HEIGHT SENTINEL. A z of -100 or below does not mean "a hundred units under the sea": it is the
# script convention for "put it on the ground here", and San Andreas' own handler for 0x016C
# implements it at 0x47C869 - `fcom [0x859014]`, which is the -100.0 the fall floor uses - by
# replacing z with CWorld::FindGroundZForCoord(x, y). GTA III uses it for the Portland police
# station. It is written through to the .dat as it stands and resolved by the plugin at the moment
# it is needed, when the region's collision is in memory and can answer.
#
# THE TOWN COLUMN is not written. San Andreas' fifth parameter gates a point behind an island being
# unlocked (CRestart::FindClosestHospitalRestartPoint skips a point whose town is above
# CStats::GetStatValue(0xB5) at 0x460973), and a region has no such progression - the whole imported
# city is walkable from the first frame - so every mined point is live from the start. The donors'
# own island gating is in the script, not in these instructions: III registers its three pairs as
# the islands open, and all three are written here.

SCM_RESTART_OPCODES = {0x016C: "hospital", 0x016D: "police"}
# Below this a script coordinate means "find the ground", not a height. See the note above.
SCM_GROUND_Z = -100.0


def scm_code_segment(blob):
    """Offset of the first instruction after a III/VC main.scm's three header segments.

    Each of the three begins with GOTO (opcode 0002) and one int32 parameter (tag 01) holding the
    offset of the next; the third jump lands on the main code. Anything else is not one of these
    two games' scripts, and saying so is better than walking a file that is not what we think.
    """
    at = 0
    for _ in range(3):
        if blob[at:at + 3] != b"\x02\x00\x01":
            return None
        nxt = struct.unpack_from("<I", blob, at + 3)[0]
        if not (at < nxt < len(blob)):
            return None
        at = nxt
    return at


def scm_read_float(blob, at, prefix):
    """The value of a type-06 float parameter at `at`, and the offset just past it."""
    if prefix == "iii_":
        return struct.unpack_from("<h", blob, at + 1)[0] / 16.0, at + 3
    return struct.unpack_from("<f", blob, at + 1)[0], at + 5


def zon_boxes(text):
    """(name, x1, y1, x2, y2) of every zone box in a .zon file, normalised.

    The same nine-or-ten column lines zon_labels reads; only the footprint is wanted here, because
    a restart point's own height is the thing being checked against the map rather than with it.
    """
    boxes, section = [], None
    for line in text.splitlines():
        clean = line.split("#")[0].strip()
        if not clean:
            continue
        tok = [t for t in re.split(r"[,\s]+", clean) if t]
        if len(tok) == 1:
            section = None if tok[0].lower() == "end" else tok[0].lower()
            continue
        if section not in (None, "zone") or len(tok) < 9:
            continue
        try:
            x1, y1, _z1, x2, y2, _z2 = (float(t) for t in tok[2:8])
        except ValueError:
            continue
        boxes.append((tok[0].upper(), min(x1, x2), min(y1, y2), max(x1, x2), max(y1, y2)))
    return boxes


def zone_at(boxes, x, y):
    """The name of the smallest zone box containing (x, y), or None."""
    best, area = None, None
    for name, x1, y1, x2, y2 in boxes:
        if x1 <= x <= x2 and y1 <= y <= y2:
            a = (x2 - x1) * (y2 - y1)
            if area is None or a < area:
                best, area = name, a
    return best


def scm_restart_points(scm_path, prefix, zone_text=""):
    """Every hospital and police restart point the source main.scm registers, and what was rejected.

    Returns (points, notes). A point is a dict of kind, x, y, z, heading, zone and at (the byte
    offset in the script, so any line of the emitted file can be gone back to).
    """
    with open(scm_path, "rb") as f:
        blob = f.read()
    code = scm_code_segment(blob)
    if code is None:
        return [], ["%s does not begin with three segment jumps, so it is not a III/VC main.scm; "
                    "no restart points mined" % os.path.basename(scm_path)]
    boxes = zon_boxes(zone_text)
    notes = []

    # Every offset whose two bytes read as one of the opcodes AND whose next bytes decode as four
    # float parameters and no fifth. Deliberately every byte offset, not every instruction: walking
    # the script properly would need the parameter count of all 800-odd opcodes, and the chain test
    # below establishes the instruction boundaries far more cheaply.
    raw, at, end = [], code, len(blob)
    while at < end - 2:
        op = struct.unpack_from("<H", blob, at)[0]
        if op in SCM_RESTART_OPCODES:
            p, vals = at + 2, []
            while len(vals) < 4 and p < end and blob[p] == 0x06:
                v, p = scm_read_float(blob, p, prefix)
                vals.append(v)
            if len(vals) == 4 and (p >= end or blob[p] != 0x06):
                raw.append((at, p, op, vals))
        at += 1

    starts = {r[0] for r in raw}
    ends = {r[1] for r in raw}
    points, dropped = [], collections.OrderedDict(
        [("not part of a run of restart instructions", 0),
         ("off the map or not an angle", 0),
         ("outside every zone the region ships", 0)])
    for at, stop, op, (x, y, z, heading) in raw:
        if at not in ends and stop not in starts:
            dropped["not part of a run of restart instructions"] += 1
            continue
        if not (-4000.0 <= x <= 4000.0 and -4000.0 <= y <= 4000.0
                and -200.0 <= z <= 500.0 and -360.0 <= heading <= 360.0):
            dropped["off the map or not an angle"] += 1
            continue
        zone = zone_at(boxes, x, y) if boxes else None
        if boxes and zone is None:
            dropped["outside every zone the region ships"] += 1
            continue
        points.append({"kind": SCM_RESTART_OPCODES[op], "x": x, "y": y, "z": z,
                       "heading": heading, "zone": zone, "at": at})
    if not boxes:
        notes.append("no zone file to check the points against; they were taken on the decode and "
                     "the chain test alone")
    for why, n in dropped.items():
        if n:
            notes.append("%d byte match(es) rejected: %s" % (n, why))
    return points, notes


def write_restarts(out, src, prefix, region_title, zone_text):
    """restarts.dat for the region, mined from the source game's own main.scm. Its name, or None."""
    scm = find_file(src, "data/main.scm")
    if not scm:
        print("  data/main.scm not found - the region gets no restart points, so dying inside it "
              "will still wake you in San Andreas")
        return None
    points, notes = scm_restart_points(scm, prefix, zone_text)
    for note in notes:
        print("  restarts.dat: %s" % note)
    if not points:
        print("  restarts.dat: nothing mined out of data/main.scm; not written")
        return None

    hospitals = sum(1 for p in points if p["kind"] == "hospital")
    text = [
        "# %s restart points, mined from %s data/main.scm" % (region_title, region_title),
        "#",
        "# Where this city puts you back after you die (hospital) or are arrested (police).",
        "# Nothing of this goes into San Andreas' own restart arrays: the plugin hooks",
        "# CRestart::FindClosestHospitalRestartPoint and FindClosestPoliceRestartPoint and answers",
        "# from this table while the player is inside the region, so a save game stays byte for",
        "# byte what vanilla San Andreas writes.",
        "#",
        "# Source opcodes, verified against the script stream rather than assumed:",
        "#   0x016C ADD_HOSPITAL_RESTART   (x, y, z, heading)",
        "#   0x016D ADD_POLICE_RESTART     (x, y, z, heading)",
        "# Parameters are type-tagged: 01=int32 02=global 03=local 04=int8 05=int16 06=float.",
        "# A float is a 32-bit IEEE value in Vice City and a signed 16-bit one over 16 in GTA III.",
        "# Every line below decoded as four floats, chains with another restart instruction at an",
        "# exact instruction boundary, and stands inside one of this region's own zone boxes - the",
        "# zone it landed in is named at the end of the line.",
        "#",
        "# A z of %.0f or below is the script's \"put it on the ground here\" sentinel, not a" % SCM_GROUND_Z,
        "# height - San Andreas' own handler for 0x016C resolves it with CWorld::FindGroundZForCoord",
        "# at 0x47C869 - and the plugin resolves it the same way, once the region's collision is in",
        "# memory. GTA III uses it for the Portland police station.",
        "#",
        "# There is no town column. San Andreas' fifth parameter holds back a restart point until an",
        "# island is unlocked; an imported city is open from the first frame, so every point here is",
        "# live from the start.",
        "#",
        "# Columns: kind, x, y, z, heading",
        "#   kind      `hospital` (where you wake after dying) or `police` (after being arrested)",
        "#   x, y, z   %s's own coordinates; the region origin is NOT applied" % region_title,
        "#   heading   degrees, the way the source game's script writes them",
        "#",
        "# %d hospital and %d police point(s), in the order the script registers them."
        % (hospitals, len(points) - hospitals),
    ]
    for p in points:
        text.append("%-9s %9.3f, %9.3f, %8.3f, %7.3f   # %s%s @%#x"
                    % (p["kind"] + ",", p["x"], p["y"], p["z"], p["heading"],
                       p["zone"] or "no zone file", " (height from the ground)"
                       if p["z"] <= SCM_GROUND_Z else "", p["at"]))
    with open(os.path.join(out, "restarts.dat"), "w", encoding="latin-1") as f:
        f.write("\n".join(text) + "\n")
    print("restarts.dat: %d hospital and %d police point(s) mined from %s"
          % (hospitals, len(points) - hospitals, scm))
    return "restarts.dat"


def read_source_weapons(root):
    """Every line of the source game's data/weapon.dat, keyed by lowercased weapon name."""
    path = find_file(root, "data/weapon.dat")
    if not path:
        return {}
    out = {}
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line or line.upper().startswith("ENDWEAPONDATA"):
                continue
            tok = [t for t in re.split(r"[,\s]+", line) if t]
            if len(tok) == VC_COLUMNS:
                out[tok[0].lower()] = tok
    return out


def read_weap_ide(root):
    """The `weap` section of the source game's data/default.ide, by model id.

    default.ide hangs off data/default.dat rather than the level .dat, so read_dat never sees it -
    which is right for everything else in it (vehicles, peds, the weapons' own low model ids) and
    wrong only for this section, which is where a weapon's mesh and dictionary are declared.
    """
    path = find_file(root, "data/default.ide")
    if not path:
        return {}
    out, section = {}, None
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            s = line.strip().lower()
            if section is None:
                if s == "weap":
                    section = s
                continue
            if s == "end":
                section = None
                continue
            if s.startswith("#"):
                continue
            tok = [t for t in re.split(r"[,\s]+", line.strip()) if t]
            if len(tok) >= 3 and tok[0].isdigit():
                out[int(tok[0])] = tok
    return out


def sa_weapon_anims(sa):
    """San Andreas weapon name -> the animation file its own weap IDE line names.

    The fourth column of a weap line is an IFP block, not the animation group in weapon.dat, and
    CFileLoader::LoadWeaponObject hands it to SetAnimFile without checking that the block exists -
    so a Vice City name San Andreas has never heard of ("m60", "baseball") would leave the model
    asking the streamer for a file that is not there. The imported mesh therefore takes the
    animation file of the San Andreas weapon it is replacing, which by definition exists.
    """
    model_of, anim_of = {}, {}
    path = find_file(sa, "data/weapon.dat")
    if path:
        with open(path, "r", encoding="latin-1") as f:
            for line in f:
                if not line or line[0] not in ("$", "\xa3"):
                    continue
                tok = [t for t in re.split(r"[,\s]+", line.strip()) if t]
                if len(tok) >= 6 and tok[5].lstrip("-").isdigit():
                    model_of.setdefault(tok[1].upper(), int(tok[5]))
    ide = find_file(sa, "data/default.ide")
    if ide:
        section = None
        with open(ide, "r", encoding="latin-1") as f:
            for line in f:
                s = line.strip().lower()
                if section is None:
                    if s == "weap":
                        section = s
                    continue
                if s == "end":
                    section = None
                    continue
                tok = [t for t in re.split(r"[,\s]+", line.strip()) if t]
                if len(tok) >= 4 and tok[0].isdigit():
                    anim_of[int(tok[0])] = tok[3]
    return {name: anim_of.get(model, "null") for name, model in model_of.items()}


def sa_weapon_meshes(sa):
    """San Andreas weapon name -> (model name, dictionary name) from its weapon.dat and default.ide.

    The donor's own HUD art lives in that dictionary, which is where an imported weapon borrows an
    icon from when the source game has none of its own.
    """
    model_of, mesh_of = {}, {}
    path = find_file(sa, "data/weapon.dat")
    if path:
        with open(path, "r", encoding="latin-1") as f:
            for line in f:
                if not line or line[0] not in ("$", "\xa3"):
                    continue
                tok = [t for t in re.split(r"[,\s]+", line.strip()) if t]
                if len(tok) >= 6 and tok[5].lstrip("-").isdigit():
                    model_of.setdefault(tok[1].upper(), int(tok[5]))
    ide = find_file(sa, "data/default.ide")
    if ide:
        section = None
        with open(ide, "r", encoding="latin-1") as f:
            for line in f:
                s = line.strip().lower()
                if section is None:
                    if s == "weap":
                        section = s
                    continue
                if s == "end":
                    section = None
                    continue
                tok = [t for t in re.split(r"[,\s]+", line.strip()) if t]
                if len(tok) >= 3 and tok[0].isdigit():
                    mesh_of[int(tok[0])] = (tok[1], tok[2])
    return {name: mesh_of[model] for name, model in model_of.items() if model in mesh_of}


def sa_weapon_dictionaries(sa, wanted):
    """{dictionary stem: {texture name lower: (name, chunk)}} for `wanted` out of SA's gta3.img."""
    out = {}
    path = find_file(sa, "models/gta3.img")
    if not path or not wanted:
        return out
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            return out
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(32 * count)
        for at in range(0, 32 * count, 32):
            off, streamed, archived = struct.unpack_from("<IHH", table, at)
            name = table[at + 8:at + 32].split(b"\0", 1)[0].decode("latin-1").lower()
            if not name.endswith(".txd") or name[:-4] not in wanted:
                continue
            f.seek(off * SECTOR)
            blob = f.read(max(streamed, archived) * SECTOR)
            out[name[:-4]] = {n.lower(): (n, c) for n, c in txd_chunks_asis(blob)}
    return out


def weapon_hud_plan(src, sa, renames):
    """The HUD textures each imported weapon needs, and where each one comes from.

    Returns ({dictionary file name lower: [(new texture name, name to copy inside that dictionary
    or None, ready-made chunk or None)]}, notes for the log). The local name is tried first, because
    it is the source game's own art for the gun the player is actually holding; the chunk is the
    donor's San Andreas art, used when the source has none.
    """
    plan, notes = {}, []
    weapons = read_source_weapons(src)
    weap_ide = read_weap_ide(src)
    if not weapons or not weap_ide:
        return plan, notes

    meshes = sa_weapon_meshes(sa)
    wanted = {meshes[donor][1].lower() for _n, donor, _l, _w in VC_WEAPON_MAP if donor in meshes}
    donor_txds = sa_weapon_dictionaries(sa, wanted)

    for source_name, donor, _live, _why in VC_WEAPON_MAP:
        tok = weapons.get(source_name.lower())
        if not tok:
            continue
        ide = weap_ide.get(int(tok[VC_COL["model"]]))
        if not ide:
            continue
        model = renames.get(ide[1].lower() + ".dff", ide[1])
        key = (renames.get(ide[2].lower() + ".txd", ide[2]) + ".txd").lower()

        donor_model, donor_txd = meshes.get(donor, (None, None))
        donor_art = donor_txds.get((donor_txd or "").lower(), {})

        for suffix in ("ICON", "CROSSHAIR"):
            wanted_name = hud_texture_name(model, suffix)
            # Vice City names its icon after the weapon and nothing else; it keeps no reticle at
            # all, so that one always comes from the donor.
            local = ide[1] if suffix == "ICON" else None
            spare = donor_art.get((donor_model or "").lower() + suffix.lower())
            if not local and not spare:
                if suffix == "CROSSHAIR":
                    continue          # the donor has no reticle either, so neither weapon draws one
                notes.append("%s: no icon in the source and none on %s either" % (model, donor))
                continue
            plan.setdefault(key, []).append((wanted_name, local, spare[1] if spare else None))
    return plan, notes


def write_weapons(out, src, sa, renames, prefix):
    """weapons.dat and ide/weapons.ide for the region, or (None, None) when there is nothing to say.

    Only Vice City has anything: every gun GTA III carries, San Andreas carries too - even the M16,
    which is San Andreas' M4 with different art - and III's weapon meshes are not importable in any
    case, being clumps inside models/Generic/weapons.dff rather than entries in gta3.img.
    """
    if prefix != "vc_":
        return None, None
    weapons = read_source_weapons(src)
    weap_ide = read_weap_ide(src)
    if not weapons or not weap_ide:
        print("  no data/weapon.dat or no weap section in data/default.ide; no weapons imported")
        return None, None
    sa_anim = sa_weapon_anims(sa)

    def renamed(name, ext):
        return renames.get(name.lower() + ext, name)

    lines, ide_lines, missing = [], [], []
    for source_name, sa_name, live, why in VC_WEAPON_MAP:
        tok = weapons.get(source_name.lower())
        if not tok:
            missing.append(source_name)
            continue
        ide = weap_ide.get(int(tok[VC_COL["model"]]))
        if not ide:
            missing.append(source_name)
            continue

        # The model is written by name, not by id: the id in the emitted IDE is remapped into the
        # free range when the region loads, and the name is the only thing that survives that.
        model = renamed(ide[1], ".dff")
        fire = tok[VC_COL["fire"]].upper()
        melee = fire == "MELEE"
        # "PYTHON/DESERT_EAGLE": the name the new weapon type answers to, then the San Andreas
        # weapon it is paired with. Under RegionWeaponMode = Add that pairing is only a donor -
        # whose code paths, slot, animation group and unstated columns the new gun borrows - and San
        # Andreas' own weapon is untouched. Under Replace the new gun is written over it.
        field = ["%s/%s" % (source_name.upper(), sa_name),
                 fire if fire in SA_FIRE_TYPES else "*",
                 "*",                      # target range: San Andreas' own; Vice City has one range column
                 tok[VC_COL["range"]],
                 model,
                 "*",                      # model 2: San Andreas' own
                 "*",                      # slot: San Andreas' own, so the weapon wheel keeps its order
                 "*",                      # animation group: San Andreas' own, which addresses its own IFPs
                 # San Andreas' melee parser writes neither of these - a melee weapon's damage is in
                 # its combo, not in CWeaponInfo - and Vice City's melee ammunition column is the
                 # placeholder 1000 for every one of them.
                 "*" if melee else tok[VC_COL["ammo"]],
                 "*" if melee else tok[VC_COL["damage"]],
                 tok[VC_COL["offx"]], tok[VC_COL["offy"]], tok[VC_COL["offz"]],
                 "*"]                      # flags: the two games' flag words share no bit meanings
        if fire in ("PROJECTILE", "AREA_EFFECT"):
            field += [tok[VC_COL["speed"]], tok[VC_COL["radius"]],
                      tok[VC_COL["lifespan"]], tok[VC_COL["spread"]]]
        text = "%s%s   # %s" % ("" if live else ";", "  ".join(field), why)
        lines.append(text)

        # Every mapped weapon's mesh travels, commented-out ones included, so that uncommenting a
        # line is all it takes. The id stays as Vice City wrote it; CityLoader remaps it.
        ide_lines.append("%s, %s, %s, %s, %s" % (ide[0], model, renamed(ide[2], ".txd"),
                                                 sa_anim.get(sa_name, "null"), ", ".join(ide[4:])))

    if missing:
        print("  weapons not found in the source data: %s" % ", ".join(missing))
    if not lines:
        return None, None

    with open(os.path.join(out, "ide", "weapons.ide"), "w", encoding="latin-1") as f:
        f.write("# The source game's weapon meshes, written by tools/import_city.py from\n"
                "# data/default.ide. Loaded only when the region's Weapons key is set: a weapon\n"
                "# model info costs one of the fifty-one San Andreas has, and a region that is not\n"
                "# replacing any weapon has nothing to point at them.\n"
                "weap\n" + "\n".join(ide_lines) + "\nend\n")

    header = (
        "# The guns Vice City has and San Andreas does not. Written by tools/import_city.py; see\n"
        "# the Weapons note in MapExpander.ini for what the columns mean.\n"
        "#\n"
        "# Column A is NEWNAME/SAWEAPON. Under the plugin's default RegionWeaponMode = Add the\n"
        "# NEWNAME becomes a weapon type of its own and SAWEAPON is only the donor it borrows its\n"
        "# behaviour and its unstated columns from - San Andreas' Desert Eagle stays a Desert Eagle\n"
        "# and the Python is a new gun beside it. There are exactly ten free weapon types, which is\n"
        "# exactly how many live lines are below.\n"
        "#\n"
        "# UNDER RegionWeaponMode = Replace THE SAME FILE MEANS SOMETHING ELSE: each gun is written\n"
        "# over its donor, for the whole game and not only inside the region, so the first line\n"
        "# would make the Desert Eagle a Python in Los Santos too. In that mode you probably want\n"
        "# the four melee lines commented out, since they trade the katana, the knife, the shovel\n"
        "# and the pool cue away.\n"
        "#\n"
        "# A column of * keeps what was already there - the donor's under Add, San Andreas' own\n"
        "# under Replace. Everything Vice City has no column for is left that way: the target\n"
        "# range, the weapon slot, the animation group, the skill levels and their accuracy and\n"
        "# move speed, and the animation frame numbers - those last address San Andreas' own\n"
        "# animations, which Vice City's numbers do not.\n"
        "#\n"
        "# What does carry over: the mesh, the weapon range, the clip size, the damage, the fire\n"
        "# offset, and for a projectile its speed, radius, lifespan and spread.\n"
        "#\n"
        "# name/donor    fire type  target  range  model  model2 slot anim clip dmg  offset x y z  flags\n")
    with open(os.path.join(out, "weapons.dat"), "w", encoding="latin-1") as f:
        f.write(header + "\n".join(lines) + "\n")

    live = sum(1 for l in lines if not l.startswith(";"))
    print("weapons.dat: %d line(s), %d of them live; ide/weapons.ide: %d model(s)"
          % (len(lines), live, len(ide_lines)))
    return "weapons.dat", "weapons.ide"


# ---------------------------------------------------------------- pedestrian paths

# Both donor games write path node coordinates multiplied by sixteen. Nothing says so in the files;
# it is simply what their exporter did, and it is confirmed here by the result: Vice City's ped
# nodes come out spanning x -1829..889 and y -1589..1508, which is its map to the metre.
PATH_SCALE = 16.0

# A path group is always twelve node slots, used or not. A node's first column says which:
#   0  unused - the slot is padding and its coordinates are all zero
#   1  external - an end of the group, joined to the matching end of a neighbouring group
#   2  internal - joined only inside its own group, through the `next` column
PATH_NODE_UNUSED, PATH_NODE_EXTERNAL, PATH_NODE_INTERNAL = 0, 1, 2
PATH_GROUP_SLOTS = 12

# Vice City's IPL path groups are headed "type, id": 0 is a PEDESTRIAN path, 1 a vehicle path and
# 2 a boat path. That is what the game's own loader does - CFileLoader::LoadScene sends type 0 to
# LoadPedPathNode, 1 to LoadCarPathNode and 2 to LoadCarPathNode with the water flag - and San
# Andreas' leftover paths*.ipl, written in the same format, agree: matched against its binary
# NODES*.DAT, type 0 lands only on ped nodes and types 1 and 2 only on vehicle nodes, and the
# pedestrian-crossing flag is set on type 0 alone. An earlier reading had 0 and 1 the other way
# round, which put the cars on the pavements and the peds in the road; the "9514 type-0 nodes to
# 4588 type-1" count that looked inverted was the pavement outnumbering the road, as it should.
VC_PATH_PED, VC_PATH_CAR, VC_PATH_BOAT = 0, 1, 2
# GTA III says it in words instead, in the group header's first column.
III_PATH_KIND = {"car": VC_PATH_CAR, "ped": VC_PATH_PED}

# Columns after the three coordinates. Both games write the first nine columns the same way -
# type, next, crossing, x, y, z, median, left lanes, right lanes - and Vice City adds three more
# that GTA III has no equivalent of. GTA III writes 160 in the median column and its loader throws
# it away; Vice City's car nodes carry the full width of the central reservation in metres there
# (its loader stores 8 * min(width, 15)), and its ped nodes the width of the pavement.
PATH_COL_CROSSING, PATH_COL_MEDIAN, PATH_COL_LEFT, PATH_COL_RIGHT = 2, 6, 7, 8

# How close two external nodes have to be before they are taken to be the same point. The two games
# joined them at run time by looking for the nearest external node of another group; here they are
# merged outright, which is the same thing and leaves a simpler graph. The threshold is generous
# against the measured data: for Vice City the nearest external node in another group is within
# 0.35 units for 95 per cent of them, and within 1.6 for 99 per cent.
PATH_MERGE_RADIUS = 2.0


def read_path_groups(text):
    """Every path group in an IPL or IDE `path` section, as {"head": [...], "nodes": [[...], ...]}.

    The section is unusual among the two games' formats in being indented rather than flat: a group
    starts with an unindented header line and its twelve node lines follow, each beginning with a
    tab. That indentation is the only thing separating a header from a node, so it is what this
    reads - `end` closes the section as it does everywhere else.
    """
    groups, inside, current = [], False, None
    for raw in text.splitlines():
        stripped = raw.strip()
        if not inside:
            if stripped.lower() == "path":
                inside = True
            continue
        if stripped.lower() == "end":
            inside = False
            continue
        if not stripped or stripped.startswith("#"):
            continue
        fields = [t.strip() for t in stripped.split(",")]
        if raw[:1].isspace():
            if current is not None:
                current["nodes"].append(fields)
        else:
            current = {"head": fields, "nodes": []}
            groups.append(current)
    return groups


class PedGraph:
    """The pedestrian graph of a whole city: unique nodes, and which of them are joined.

    Nodes go in with the coordinates the source game gives, in its own world, and links are added
    both ways - the two games' path links are one-directional in the file only because a group
    lists each node once. Coincident external nodes are merged as they arrive, so that the graph
    comes out connected across the group boundaries the exporter left everywhere.
    """

    CELL = 4.0                # merge lookup grid; must be at least PATH_MERGE_RADIUS

    def __init__(self):
        self.pos = []         # [x, y, z] per node
        self.links = []       # set of neighbour indices per node
        self.zone = []        # ped group id per node, filled in later; -1 until then
        self.lanes = []       # (median, left lanes, right lanes, crossing) - what traffic needs
        self.merges = 0       # external nodes that landed on one already there
        self._cells = {}

    def _key(self, x, y, z):
        return (int(x // self.CELL), int(y // self.CELL), int(z // self.CELL))

    def add(self, x, y, z, merge):
        """Return the index of a node at this point, merging onto an existing one when asked."""
        if merge:
            kx, ky, kz = self._key(x, y, z)
            best, bestd = -1, PATH_MERGE_RADIUS ** 2
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        for i in self._cells.get((kx + dx, ky + dy, kz + dz), ()):
                            px, py, pz = self.pos[i]
                            d = (px - x) ** 2 + (py - y) ** 2 + (pz - z) ** 2
                            if d < bestd:
                                best, bestd = i, d
            if best >= 0:
                self.merges += 1
                return best
        index = len(self.pos)
        self.pos.append([x, y, z])
        self.links.append(set())
        self.zone.append(-1)
        self.lanes.append((10.0, 1, 1, 0))   # median, left lanes, right lanes, crossing
        if merge:
            self._cells.setdefault(self._key(x, y, z), []).append(index)
        return index

    def join(self, a, b):
        if a != b and a >= 0 and b >= 0:
            self.links[a].add(b)
            self.links[b].add(a)

    def add_group(self, nodes):
        """One twelve-slot path group. Returns how many live nodes it contributed."""
        mine, live = {}, 0
        for slot, fields in enumerate(nodes[:PATH_GROUP_SLOTS]):
            if len(fields) < 6:
                continue
            try:
                kind = int(float(fields[0]))
                x = float(fields[3]) / PATH_SCALE
                y = float(fields[4]) / PATH_SCALE
                z = float(fields[5]) / PATH_SCALE
            except ValueError:
                continue
            if kind == PATH_NODE_UNUSED:
                continue
            # Only the group's ends are merged with other groups. An internal node that happened to
            # land on another group's internal node is a coincidence, not a junction, and merging
            # the two would join two pavements that the source game kept apart.
            index = self.add(x, y, z, kind == PATH_NODE_EXTERNAL)
            mine[slot] = index
            live += 1
            # What traffic needs and a pedestrian does not: how wide the central reservation is and
            # how many lanes run each way. A merged node keeps whichever description came first;
            # they are two ends of the same junction and the source game agreed with itself.
            if len(self.lanes) > index and self.lanes[index] == (10.0, 1, 1, 0):
                def column(at, default, cast=int):
                    try:
                        return cast(float(fields[at]))
                    except (IndexError, ValueError):
                        return default
                # The median goes down exactly as the source game wrote it, undivided, because the
                # two games do not agree about what it is. GTA III writes the constant 160 for
                # every car node, which is 10 units once the coordinate scale is taken off it.
                # Vice City writes a continuous small value - 3.5, 4, 1.92 - which is already a
                # width and would become a couple of centimetres if the same scale were applied.
                # Rather than guess which reading is right for which, the raw number is recorded
                # and nothing uses it; lane placement is done from the lane counts and San Andreas'
                # own lane width, which cannot be wrong for either game.
                self.lanes[index] = (column(PATH_COL_MEDIAN, 0.0, float),
                                     column(PATH_COL_LEFT, 1), column(PATH_COL_RIGHT, 1),
                                     column(PATH_COL_CROSSING, 0))
        for slot, fields in enumerate(nodes[:PATH_GROUP_SLOTS]):
            if slot not in mine or len(fields) < 2:
                continue
            try:
                nxt = int(float(fields[1]))
            except ValueError:
                continue
            if nxt in mine:
                self.join(mine[slot], mine[nxt])
        return live

    def drop_isolated(self):
        """Throw away nodes nothing links to, and renumber. Returns how many went.

        A node with no neighbour is a place a pedestrian could be put and then have nowhere to walk,
        which is worse than not putting one there: it would stand in the street for ever.
        """
        keep = [i for i in range(len(self.pos)) if self.links[i]]
        if len(keep) == len(self.pos):
            return 0
        renumber = {old: new for new, old in enumerate(keep)}
        dropped = len(self.pos) - len(keep)
        self.pos = [self.pos[i] for i in keep]
        self.zone = [self.zone[i] for i in keep]
        self.lanes = [self.lanes[i] for i in keep]
        self.links = [set(renumber[n] for n in self.links[i] if n in renumber) for i in keep]
        return dropped


def vc_path_graph(ipl_texts, want):
    """Vice City: one kind of path graph out of the `path` sections of its IPL files.

    Vice City puts its whole path network in the IPLs, at absolute world coordinates, so this is
    only a matter of reading it. GTA III does not - see iii_path_graph.
    """
    graph, groups = PedGraph(), 0
    for text in ipl_texts:
        for group in read_path_groups(text):
            if not group["head"]:
                continue
            try:
                kind = int(float(group["head"][0]))
            except ValueError:
                continue
            if kind != want:
                continue
            if graph.add_group(group["nodes"]):
                groups += 1
    return graph, groups


def iii_path_graph(ide_texts, ipl_texts, renames, want, negate_w=True):
    """GTA III: one kind of path graph, assembled from path groups attached to road models.

    III stores its paths quite differently from Vice City, which is why they are read separately.
    A III path group lives in an IDE, is headed `ped, <model id>, <model name>` or `car, ...`, and
    its coordinates are relative to the model rather than to the world: the group describes the
    pavement or the carriageway across one road tile, and the tile is placed many times. So the
    world nodes are the group transformed by every `inst` line that places that model - which is
    also why III's road tiles are the same handful of meshes laid end to end.

    An inst line carries a quaternion, and III's road tiles are laid flat and turned only about the
    vertical, so only that rotation is applied. A tile pitched or rolled would come out wrong, and
    none of III's are. Which sign that rotation takes is decided by measurement rather than by
    argument - see iii_rotation_sign.
    """
    want_word = "ped" if want == VC_PATH_PED else "car"
    by_model, graph, groups, placed = {}, PedGraph(), 0, 0
    for text in ide_texts:
        for group in read_path_groups(text):
            head = group["head"]
            if len(head) < 3 or head[0].strip().lower() != want_word:
                continue
            by_model.setdefault(head[2].strip().lower(), []).append(group["nodes"])
            groups += 1
    if not by_model:
        return graph, 0
    # The IDE names the model as the source game called it; the IPLs this reads have already been
    # renamed under the region's tag, so look the group up under both.
    tagged = {}
    for name, nodes in by_model.items():
        tagged[name] = nodes
        new = renames.get(name + ".dff")
        if new:
            tagged[new.lower()] = nodes
    for text in ipl_texts:
        section = None
        for raw in text.splitlines():
            s = raw.strip()
            low = s.lower()
            if section is None:
                if low in ("inst", "cull", "pick", "zone", "path", "occl", "mult", "enex", "cars",
                           "jump", "tcyc", "auzo", "grge"):
                    section = low
                continue
            if low == "end":
                section = None
                continue
            if section != "inst" or not s or s.startswith("#"):
                continue
            tok = [t.strip() for t in re.split(r"[,\s]+", s) if t.strip()]
            if len(tok) < 12:
                continue
            nodesets = tagged.get(tok[1].lower())
            if not nodesets:
                continue
            try:
                ox, oy, oz = float(tok[2]), float(tok[3]), float(tok[4])
                rz, rw = float(tok[10]), float(tok[11])
            except ValueError:
                continue
            # A quaternion turning only about the vertical is (0, 0, sin(a/2), cos(a/2)); III writes
            # the scalar part last, and whether it is negated relative to the usual convention is
            # settled by measurement rather than by reading it off - see iii_rotation_sign.
            angle = (-2.0 if negate_w else 2.0) * math.atan2(rz, rw)
            cos_a, sin_a = math.cos(angle), math.sin(angle)
            for nodes in nodesets:
                turned = []
                for fields in nodes:
                    if len(fields) < 6:
                        turned.append(fields)
                        continue
                    try:
                        x, y, z = float(fields[3]), float(fields[4]), float(fields[5])
                    except ValueError:
                        turned.append(fields)
                        continue
                    wx = ox + (x * cos_a - y * sin_a) / PATH_SCALE
                    wy = oy + (x * sin_a + y * cos_a) / PATH_SCALE
                    wz = oz + z / PATH_SCALE
                    # add_group divides by PATH_SCALE itself, so hand it back scaled up again.
                    turned.append(list(fields[:3]) +
                                  ["%.6f" % (wx * PATH_SCALE), "%.6f" % (wy * PATH_SCALE),
                                   "%.6f" % (wz * PATH_SCALE)] + list(fields[6:]))
                if graph.add_group(turned):
                    placed += 1
    return graph, placed


def iii_rotation_sign(ide_texts, ipl_texts, renames, want):
    """Which way GTA III's placement quaternions turn, decided by the data rather than by argument.

    A III path group is authored once and instanced at every placement of its road model, so the
    ends of two adjacent tiles only land on each other if the rotation is applied the right way
    round. Get the sign wrong and every group is turned the wrong way about its tile: the graph
    still builds, but the external ends that should be the same point are not, so far fewer of them
    merge. So both signs are tried and the one that merges more wins - which is a measurement of
    the answer rather than a reading of the convention, and it says so in the log either way.
    """
    best = None
    for negate_w in (True, False):
        graph, placed = iii_path_graph(ide_texts, ipl_texts, renames, want, negate_w)
        if best is None or graph.merges > best[0].merges:
            best = (graph, placed, negate_w)
    return best


def write_path_nodes(out, name, graph, zones, what, extra):
    """One path graph as a table: a node per line, its links on the same line.

    `extra` says whether the lane columns are written. A pedestrian has no use for them - both
    games leave the median at zero for a ped node - and traffic needs all of them.
    """
    lines = [
        "# %s for this region, written by tools/import_city.py." % what,
        "#",
        "# The source game's own graph, in the region's own coordinates. San Andreas cannot hold",
        "# these as they are: it stores a path node position as an int16 times 0.125, which reaches",
        "# only +/-4095.875, and this region sits well past that. See docs/known-limitations.md.",
        "#",
        "# One node per line, numbered by the order they appear in:",
        "#",
        ("#   x y z zone median left right crossing link link ..." if extra else
         "#   x y z zone link link ..."),
        "#",
        "# zone is a line number in pedgrp.dat's zone table, or -1 for nowhere in particular. The",
        "# links are node numbers and every one of them is written on both nodes.",
    ]
    if extra:
        lines += [
            "# left and right are how many lanes run each way - the columns the source game's own",
            "# traffic drove by - and crossing is its pedestrian-crossing flag.",
            "#",
            "# median is the source game's own number, exactly as it wrote it, and the two games",
            "# do not agree about what it means: GTA III writes 160 for every car node and Vice",
            "# City writes a continuous small value like 3.5 or 4. Nothing reads it. Lanes are",
            "# placed from the counts above and San Andreas' own 5.4-unit lane width instead,",
            "# which cannot be wrong for either of them.",
        ]
    lines += [
        "#",
        "# %d node(s), %d link(s), %d zone(s)."
        % (len(graph.pos), sum(len(l) for l in graph.links) // 2, len(zones)),
    ]
    for i, (x, y, z) in enumerate(graph.pos):
        links = " ".join(str(n) for n in sorted(graph.links[i]))
        if extra:
            median, left, right, crossing = graph.lanes[i]
            lines.append("%.3f %.3f %.3f %d %.2f %d %d %d %s"
                         % (x, y, z, graph.zone[i], median, left, right, crossing, links))
        else:
            lines.append("%.3f %.3f %.3f %d %s" % (x, y, z, graph.zone[i], links))
    with open(os.path.join(out, name), "w", encoding="latin-1") as f:
        f.write("\n".join(lines) + "\n")
    return name


# ---------------------------------------------------------------- who walks where

# The two donor games decide who walks in a district exactly the same way as each other, and not at
# all the way San Andreas does - which is why this is read from the source rather than mapped onto
# anything of San Andreas'.
#
#   data/pedgrp.dat        numbered groups of ped model names. GTA III writes eight per group and
#                          has 31 groups; Vice City writes sixteen and has 67.
#   the zone boxes         data/gta3.zon for III, the type 2 "info" zones of data/info.zon for
#                          Vice City. A name may be used by several boxes; they share one record.
#   data/main.scm          and here is the link, which is in neither of the above: both games set
#                          a zone's ped group from their mission script, with the opcode this file
#                          calls SET_ZONE_PED_GROUP, once for daytime and once for night.
#
# San Andreas does none of this. Its zones carry no population fields at all; its script hands each
# zone one of twenty popcycle types and data/popcycle.dat then gives percentages per hour across
# eighteen columns. There is no honest mapping between the two schemes, and there does not need to
# be one: San Andreas' own spawner never runs over an imported city - it has no path nodes there -
# so the plugin is the only thing putting people in these streets and it reads this table directly.
# San Andreas' own map keeps its own crowd, untouched.

# SET_ZONE_PED_GROUP. Both games use the same opcode number, which is the one thing about their
# scripts that is the same; the pickup work on this project found their float encodings differ, so
# nothing here is assumed to carry over without being counted against the source.
SCM_SET_ZONE_PED_GROUP = 0x0324
SCM_LABEL_BYTES = 8
# Immediate integer parameters: type byte then that many bytes, little endian and signed.
SCM_INT_SIZES = {0x04: 1, 0x05: 2, 0x06: 4}


def scm_int(blob, at):
    """(value, bytes consumed) of an immediate integer parameter, or (None, 0)."""
    if at >= len(blob):
        return None, 0
    size = SCM_INT_SIZES.get(blob[at])
    if not size or at + 1 + size > len(blob):
        return None, 0
    return int.from_bytes(blob[at + 1:at + 1 + size], "little", signed=True), 1 + size


def scm_zone_ped_groups(path, group_count, known_zones):
    """{ZONE: (day group, night group)} out of a compiled main.scm.

    The opcode is found by scanning for its two bytes rather than by walking the script, which
    would mean implementing both games' whole instruction sets to know how long each one is. A
    two-byte needle on its own would match plenty of coincidences, so every candidate has to earn
    its place: the eight bytes after it must read as a zone label the zone file actually declares,
    and the two parameters after that must be a day/night flag and a group number in range. The
    caller is told how many were found so a wrong count is visible rather than silent - GTA III
    should give 84 and Vice City 96, two per zone.

    A later call wins over an earlier one, which is what the games themselves do: Vice City sets
    TERMINL twice in its start-up block and means the second.
    """
    with open(path, "rb") as f:
        blob = f.read()
    needle = SCM_SET_ZONE_PED_GROUP.to_bytes(2, "little")
    found, at = {}, 0
    while True:
        at = blob.find(needle, at)
        if at < 0:
            break
        start = at
        at += 1
        p = start + 2
        raw = blob[p:p + SCM_LABEL_BYTES]
        if len(raw) < SCM_LABEL_BYTES:
            continue
        # The label is NUL terminated inside its eight bytes. What follows the terminator is not
        # reliably anything: Vice City pads with 0xCC, GTA III mostly pads with NULs but leaves the
        # odd byte of its assembler's scratch behind - "CHINA\0I\0", "PARK\0\0U\0" - and rejecting
        # those cost two of GTA III's zones. So the tail only has to be bytes a text buffer could
        # hold; the real filter is the zone name and the two parameters below.
        name = raw.split(b"\0", 1)[0]
        if not name or any(b not in (0x00, 0xCC) and not 0x20 <= b <= 0x7E for b in raw):
            continue
        try:
            label = name.decode("ascii")
        except UnicodeDecodeError:
            continue
        if not re.fullmatch(r"[A-Za-z0-9_]+", label) or label.upper() not in known_zones:
            continue
        day, used = scm_int(blob, p + SCM_LABEL_BYTES)
        if used == 0 or day not in (0, 1):
            continue
        group, used2 = scm_int(blob, p + SCM_LABEL_BYTES + used)
        if used2 == 0 or not 0 <= group < group_count:
            continue
        pair = list(found.get(label.upper(), (0, 0)))
        pair[0 if day else 1] = group
        found[label.upper()] = tuple(pair)
    return found


def read_donor_pedgrp(path):
    """The donor's data/pedgrp.dat: [(comment, [model names])] in file order, index = group number.

    The comment at the end of a line is the group's name in the game's own words ("redlight night",
    "ghetto1") and it is kept, because it is the only thing that makes the emitted table readable.

    Three of GTA III's lines are missing a comma between two names - groups 8, 9 and 17 in the
    stock file - so the split is on whitespace as well as commas rather than on commas alone.
    """
    groups = []
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            line = raw.rstrip("\n")
            comment = ""
            for marker in ("//", "#"):
                cut = line.find(marker)
                if cut >= 0:
                    if not comment:
                        comment = line[cut + len(marker):].strip()
                    line = line[:cut]
            if not line.strip():
                continue
            names = [t for t in re.split(r"[,\s]+", line.strip()) if t]
            if names:
                groups.append((comment, names))
    return groups


def read_zon_boxes(text, want_types):
    """[(name, x1, y1, z1, x2, y2, z2)] from the zone lines of a .zon, for the given zone types."""
    boxes, section = [], None
    for raw in text.splitlines():
        s = raw.strip()
        low = s.lower()
        if section is None:
            if low in ("zone", "end"):
                section = "zone" if low == "zone" else None
            continue
        if low == "end":
            section = None
            continue
        if not s or s.startswith("#") or s.startswith(";"):
            continue
        t = [x.strip() for x in re.split(r"[,\s]+", s) if x.strip()]
        if len(t) < 9:
            continue
        try:
            kind = int(float(t[1]))
            v = [float(x) for x in t[2:8]]
        except ValueError:
            continue
        if want_types is not None and kind not in want_types:
            continue
        boxes.append((t[0], min(v[0], v[3]), min(v[1], v[4]), min(v[2], v[5]),
                      max(v[0], v[3]), max(v[1], v[4]), max(v[2], v[5])))
    return boxes


def stamp_zones(graph, boxes, zone_index):
    """Give every node the zone it stands in, smallest box first. Returns how many got one.

    Smallest first is what both games do when boxes overlap, and they overlap a great deal: Vice
    City draws 165 info boxes over 44 names, and a small one inside a large one is how a shop front
    is given a different crowd from the street it stands on.
    """
    ordered = sorted(range(len(boxes)),
                     key=lambda i: ((boxes[i][4] - boxes[i][1]) * (boxes[i][5] - boxes[i][2])))
    stamped = 0
    for n, (x, y, z) in enumerate(graph.pos):
        for i in ordered:
            name, x1, y1, z1, x2, y2, z2 = boxes[i]
            if x1 <= x <= x2 and y1 <= y <= y2 and z1 <= z <= z2:
                zi = zone_index.get(name.upper())
                if zi is not None:
                    graph.zone[n] = zi
                    stamped += 1
                break
    return stamped


def sa_ped_models(sa):
    """Every ped model San Andreas declares, in data/peds.ide order."""
    path = find_file(sa, "data/peds.ide")
    if not path:
        return []
    names, section = [], None
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            s = raw.strip()
            low = s.lower()
            if section is None:
                if low == "peds":
                    section = "peds"
                continue
            if low == "end":
                section = None
                continue
            if not s or s.startswith("#"):
                continue
            t = [x.strip() for x in s.split(",")]
            if len(t) > 2 and t[1].lower() != "null":
                names.append(t[1])
    return names


# San Andreas and Vice City name a ped model the same way - ethnicity, sex, age, then what they are
# doing: WMYST is a White Male Young on the STreet, BFORI a Black Female Old and RIch. San Andreas
# adds a city letter in front for a model that belongs to one of its three cities (SWMYST is the
# San Fierro one, VWMYCD a Las Vegas one) and a longer role tail. That shared convention is what
# makes a placeholder mapping possible at all, and it is why most of Vice City's crowd maps onto a
# San Andreas model that means the same thing rather than onto a stand-in.
#
# GTA III shares nothing with either: its peds are male02, fatfemale01, docker1, pimp. Those are
# mapped by the hand-written table below.
PED_ETHNICITY = "BHWOSIJVCDG"      # the first letter of a model name, when it is one of these
PED_SEX = "MF"
PED_AGE = "YO"

# What Vice City's role suffixes mean and what San Andreas has that is nearest. The first entry
# that San Andreas actually declares wins; an empty list means "nothing like it, take the fallback".
VC_PED_ROLE = {
    "ST": ["ST"],                       # street
    "RI": ["RI"],                       # rich
    "BU": ["BU"],                       # business
    "TR": ["TR1", "TR2", "TR3", "TR"],  # tramp - San Andreas numbers them
    "BE": ["BE"],                       # beach
    "PR": ["PRO"],                      # prostitute
    "CR": ["CR"],                       # criminal
    "DK": ["CON", "CONB"],              # docker - San Andreas has no docks, so a labourer
    "CW": ["CON", "CONB"],              # construction worker
    "GO": ["GOL1", "GOL2"],             # golfer
    "JG": ["JG"],                       # jogger
    "SH": ["ST"],                       # shopper - San Andreas has no shopper model
    "SK": ["BMX", "BP"],                # skater - the nearest thing on wheels
    "PI": ["PIMP", "CR"],               # pimp
    "BB": ["MOUN", "ST"],               # basketball
    "AP": ["AP"],                       # airport worker
    "LG": ["LG"],                       # lifeguard
    "MD": ["VA", "ST"],                 # maid
    "CG": ["ST"],                       # cheerleader
    "TO": ["RI", "BE"],                 # tourist
    "G1": ["ST"],
    "G2": ["ST"],
}

# GTA III's ped names carry no structure at all, so its crowd is placed by hand. Each is the San
# Andreas model that reads most like it in the street; where San Andreas has no equivalent the
# nearest role is used and the comment says what was lost.
III_PED_MAP = {
    "male01": "WMYST", "male02": "WMYST", "male03": "HMYST",
    "female01": "WFYST", "female02": "WFYST", "female03": "HFYST",
    "fatmale01": "WMOST", "fatmale02": "BMOST",
    "fatfemale01": "WFOST", "fatfemale02": "BFOST",
    "worker1": "WMYCON", "worker2": "WMYCONB",
    "const1": "WMYCON", "const2": "WMYCONB",
    "docker1": "WMYCON", "docker2": "BMYCON",
    "scum_man": "WMOTR1", "scum_wom": "SWFOST",
    "pimp": "BMYPIMP",
    "criminal01": "WMYCR", "criminal02": "BMYCR",
    "prostitute": "WFYPRO", "prostitute2": "BFYPRO",
    "cas_man": "WMYRI", "cas_wom": "WFYRI",
    "li_man1": "WMORI", "li_man2": "WMOST", "li_wom1": "WFORI", "li_wom2": "WFOST",
    "ct_man1": "OMYST", "ct_man2": "OMOST", "ct_wom1": "OFYST", "ct_wom2": "OFOST",
    "b_man1": "BMYST", "b_man2": "BMYRI", "b_man3": "BMOST",
    "b_wom1": "BFYST", "b_wom2": "BFYRI", "b_wom3": "BFOST",
    "hos_man": "WMYCON", "hos_wom": "WFYST",      # hospital staff; SA's medics are not civilians
    "st_man": "WMYBU", "st_wom": "WFYBU",         # studio
    "stud_man": "WMYST", "stud_wom": "WFYST",     # students
    "shopper1": "WFYST", "shopper2": "HFYST", "shopper3": "BFYST",
    "mod_man": "WMYRI", "mod_wom": "WFYRI",
    "fan_man1": "WMYST", "fan_man2": "BMYST", "fan_wom": "WFYST",
    "p_man1": "BMYST", "p_man2": "HMYST", "p_wom1": "BFYST", "p_wom2": "HFYST",
}

# When nothing else matches. male01 is deliberately not used: both donor pedgrp.dat files say in
# their own headers not to put it in a group, and San Andreas' own does the same.
PED_FALLBACK = ["WMYST", "WFYST", "BMYST", "BFYST", "HMYST", "HFYST"]


def split_ped_name(name):
    """(ethnicity, sex, age, role) of a San Andreas / Vice City ped name, or None.

    San Andreas puts a city letter in front of some of its models - SWMYST, VWMYCD, CWMOFR - so a
    six-letter name whose first letter would otherwise make no sense as an ethnicity is tried again
    without it.
    """
    for start in (0, 1):
        s = name.upper()[start:]
        if len(s) >= 4 and s[0] in PED_ETHNICITY and s[1] in PED_SEX and s[2] in PED_AGE:
            return s[0], s[1], s[2], s[3:]
    return None


# What somebody does when San Andreas has nobody doing what they did. Ordinary before well-off,
# because a crowd of strangers reads better as ordinary; and this is the last thing tried before
# giving up on the description altogether, so it decides what a Vice City docker or lifeguard turns
# into. Without it the answer is whatever peds.ide happens to declare first, which is the RIch
# models, and a docks crowd came out dressed for a yacht club.
PED_ROLE_LAST_RESORT = ["ST", "TR1", "BU", "RI", "BE"]


def map_vc_ped(name, sa_by_key, sa_names):
    """The San Andreas model that stands in for a Vice City one, and why.

    Tried in order of how much is being given up: the same name, then the same person doing the
    same or the nearest job, then the same job done by somebody a little older or younger or of
    another background, then the same person doing something ordinary, and only then anybody at all.
    """
    upper = name.upper()
    if upper in sa_names:
        return sa_names[upper], "same name"
    parts = split_ped_name(upper)
    if not parts:
        return PED_FALLBACK[0], "no name to read"
    eth, sex, age, role = parts
    roles = [role] + [r for r in VC_PED_ROLE.get(role[:2], []) if r != role]
    for wanted in roles:
        hit = sa_by_key.get((eth, sex, age, wanted))
        if hit:
            return hit, "same person, same job" if wanted == role else "nearest job"
    other_age = "O" if age == "Y" else "Y"
    for wanted in roles:
        hit = sa_by_key.get((eth, sex, other_age, wanted))
        if hit:
            return hit, "same job, older or younger"
    for wanted in roles:
        for key, hit in sa_by_key.items():
            if key[1] == sex and key[3] == wanted:
                return hit, "same job, different background"
    for wanted in PED_ROLE_LAST_RESORT:
        hit = sa_by_key.get((eth, sex, age, wanted))
        if hit:
            return hit, "same person, ordinary job"
    for key, hit in sa_by_key.items():
        if key[:3] == (eth, sex, age):
            return hit, "same person, any job"
    for key, hit in sa_by_key.items():
        if key[1:3] == (sex, age):
            return hit, "same sex and age"
    for key, hit in sa_by_key.items():
        if key[1] == sex:
            return hit, "same sex"
    return PED_FALLBACK[0], "nothing alike"


def build_ped_map(donor_names, sa_list, prefix):
    """{donor name: (SA name, why)} for every model the donor's ped groups ask for."""
    sa_names = {n.upper(): n for n in sa_list}
    sa_by_key = {}
    for n in sa_list:
        parts = split_ped_name(n)
        if parts and parts not in sa_by_key:
            sa_by_key[parts] = n
    out = {}
    for name in donor_names:
        if prefix == "iii_":
            hit = III_PED_MAP.get(name.lower())
            out[name] = ((sa_names.get((hit or "").upper(), hit or PED_FALLBACK[0]), "hand-mapped")
                         if hit else (PED_FALLBACK[0], "no equivalent"))
        else:
            out[name] = map_vc_ped(name, sa_by_key, sa_names)
    return out


def write_ped_groups(out, groups, zones, ped_map, prefix):
    """pedgrp.dat: who walks in each of the region's districts, over San Andreas' own models."""
    lines = [
        "# Who walks where in this region, written by tools/import_city.py from the source game's",
        "# data/pedgrp.dat, its zone boxes and the SET_ZONE_PED_GROUP calls in its main.scm.",
        "#",
        "# Two kinds of line, in any order:",
        "#",
        "#   zone  <n> <NAME> <day group> <night group>",
        "#   group <n> <name> <DONOR/SAMODEL> <DONOR/SAMODEL> ...",
        "#",
        "# pednodes.dat stamps every pedestrian node with a zone number from the table here, so a",
        "# node knows who walks on it; the plugin picks the day or the night group by the clock and",
        "# then a model from it at random.",
        "#",
        "# The right-hand side of each pair is the San Andreas model actually used. The left-hand",
        "# side is what the source game asked for, and it is written down for exactly one reason:",
        "# these are placeholders. When the donor's own ped models are imported, this file is where",
        "# they go - change SAMODEL to the imported model's name and nothing else has to change,",
        "# the same way weapons.dat pairs an imported gun with the San Andreas weapon it borrows.",
        "#",
        "# Vice City and San Andreas happen to name a ped the same way - ethnicity, sex, age, then",
        "# the job - so most of the pairing here is the San Andreas model that means the same",
        "# thing. GTA III names its peds male02, docker1, pimp, and shares nothing, so its crowd is",
        "# paired by a table written by hand in tools/import_city.py.",
        "#",
        "# %d zone(s), %d group(s), %d distinct model(s)."
        % (len(zones), len(groups), len(ped_map)),
    ]
    for i, (name, day, night) in enumerate(zones):
        lines.append("zone  %-3d %-8s %d %d" % (i, name, day, night))
    for i, (comment, names) in enumerate(groups):
        pairs = " ".join("%s/%s" % (n, ped_map.get(n, (PED_FALLBACK[0], ""))[0]) for n in names)
        label = re.sub(r"[^A-Za-z0-9_]+", "_", comment).strip("_") or "group%d" % i
        lines.append("group %-3d %-18s %s" % (i, label, pairs))
    with open(os.path.join(out, "pedgrp.dat"), "w", encoding="latin-1") as f:
        f.write("\n".join(lines) + "\n")
    return "pedgrp.dat"


# ---------------------------------------------------------------- the cars parked at the kerb
#
# Empty streets are most of why an imported city still reads as a model rather than a place, and
# neither donor game puts its parked cars in a map file. Both create them from data/main.scm, with
# literal coordinates, through one opcode:
#
#   0x014B CREATE_CAR_GENERATOR  x y z angle model colour1 colour2 force alarm lock minDelay
#                                maxDelay  -> handle
#   0x014C SET_CAR_GENERATOR     handle count       (0 = off, >100 = for ever)
#
# The parameter order was read off the script stream rather than taken from an opcode listing,
# because the listings reorder the parameters for display. It is the same in both games; what is
# NOT the same is the float encoding, which the pickup work on this project already found: Vice
# City writes a 32-bit IEEE float behind a 0x06 type byte, GTA III writes an int16 divided by 16.
#
# Nothing walks the whole instruction set to find these - that would mean implementing two games'
# scripts - so the two opcode bytes are scanned for and every candidate has to earn its place.
# Four filters, in order of how cheap they are:
#
#   1. shape       twelve immediate parameters, the first four floats and the last eight integers,
#                  then a variable to store the handle in. This alone throws out 39 of Vice City's
#                  211 raw matches and 13 of GTA III's 162.
#   2. chain       a real generator has a neighbour: the bytes after it are either the
#                  SET_CAR_GENERATOR that switches it on, addressed to the same variable, or
#                  another well-shaped generator. Vice City's are all of the first kind and GTA
#                  III's almost all of the second. A coincidence has no neighbour.
#   3. sense       the model is -1 or an id the source game's own default.ide declares as a
#                  vehicle; the heading is a bearing; alarm and lock are percentages; the delays
#                  are ordered.
#   4. ground      the position falls inside one of the source game's own zone boxes - the same
#                  file the ped-group miner validates its zone labels against.
#
# A generator the script switches OFF the moment it makes it is left out: those are the ones a
# mission turns on later, and a car that belongs to a mission we are not running is not street
# furniture. That rule drops two of GTA III's and twenty-four of Vice City's.
#
# WHAT IS CARRIED ACROSS, and what is not:
#
#   position    the source game's own, in its own coordinates; the plugin adds the region origin.
#               A z of -100 means "on the ground here" in both donor games - and, as it happens,
#               in San Andreas too: CCarCtrl::CreateCarForScript compares z against the float at
#               0x859014, which is exactly -100.0, and resolves it through CWorld::FindGroundZ
#               itself. So the convention travels untranslated.
#   heading     degrees. Both donor games and San Andreas measure a heading the same way - local
#               +Y forward, turning anticlockwise about Z - which is confirmed by San Andreas'
#               own car generator multiplying its stored byte angle by the float at 0x859BBC,
#               2*pi/256, with no sign change. Degrees times pi/180 is the whole conversion.
#   model       paired with a San Andreas model, written into the file as DONOR/SAMODEL exactly
#               as pedgrp.dat pairs a person. A model of -1 is the source game asking for "any
#               ordinary car", which San Andreas' own generators ask for in the same way (a
#               negative model id, see CCarGenerator::DoInternalProcessing at 0x6F352E), so it is
#               written as `random` and answered from the `fleet` line at the top of the file.
#   lock        the percentage chance the source game gave that car of being locked. Applied.
#   alarm       the same for an alarm. Written down and NOT applied: San Andreas keeps an alarm
#               state rather than a flag and the sentinel for "fitted but quiet" was not
#               established here, and a car that howls when it is brushed is worse than one with
#               no alarm at all.
#   colour      the source game's own carcols.dat indices. Written down and NOT applied, because
#               a donor palette index is not a San Andreas palette index - they are different
#               tables of different length. 317 of the 320 generators say -1 anyway, which both
#               games read as "any colour this model comes in", so this costs three cars.
#   force       whether the generator ignores the game's parked-car budget. Dropped: it exists to
#               steer San Andreas' own CCarGenerator budget, and these are not in it.
#   delays      the minimum and maximum wait before a generator makes another car. Dropped: they
#               are what makes a stolen parked car reappear within seconds, which is the one
#               behaviour of the donor's system this deliberately does not reproduce.

SCM_CREATE_CAR_GENERATOR = 0x014B
SCM_SET_CAR_GENERATOR = 0x014C

# The "class" column of a cars line, in every one of the three games. It is what a donor's own
# random-car chooser draws on, so it is also what decides whether a model belongs in the fleet a
# `random` generator picks from.
VEHICLE_CLASSES = {
    "normal", "poorfamily", "richfamily", "executive", "worker", "big", "taxi", "moped",
    "motorbike", "leisureboat", "workerboat", "bicycle", "ignore", "special", "bike",
}
# Ordinary traffic: what a car generator asking for "any car" should be able to get. `ignore` is
# every mission and emergency vehicle, and the boats and bikes are not what a kerb wants.
FLEET_CLASSES = ("normal", "poorfamily", "richfamily", "executive", "worker", "taxi")


def read_vehicle_ide(path):
    """{id: (name, type, class)} from the cars section of a III / VC / SA IDE.

    Three of San Andreas' 212 lines - emperor, wayfarer and dodo - separate the model name from
    the texture dictionary with a tab instead of a comma, which shifts every field after them by
    one and silently loses those three models. The name field is therefore split again on
    whitespace and the row rebuilt, rather than trusting the commas.
    """
    out, section = {}, None
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            s = raw.strip()
            low = s.lower()
            if section is None:
                if low == "cars":
                    section = 1
                continue
            if low == "end":
                section = None
                continue
            if not s or s.startswith("#"):
                continue
            t = [x.strip() for x in s.split(",")]
            if len(t) > 1 and len(t[1].split()) > 1:
                merged = t[1].split()
                t = [t[0]] + merged[:2] + t[2:]
            if len(t) < 7:
                continue
            try:
                ident = int(t[0])
            except ValueError:
                continue
            # GTA III has no anims column, so the class is one field earlier than in VC and SA.
            klass = ""
            for i in (6, 7):
                if i < len(t) and t[i].lower() in VEHICLE_CLASSES:
                    klass = t[i].lower()
                    break
            out[ident] = (t[1].lower(), t[3].lower(), klass)
    return out


# San Andreas kept a great many of Vice City's and GTA III's vehicle names - landstal, peren,
# sentinel, infernus, cheetah, taxi, ambulan, rumpo, stallion and forty more - the same way it kept
# their ped naming, and for the same reason: it is the same series. So the pairing below only has
# to cover the models whose names San Andreas did not keep. Each is the San Andreas vehicle of the
# nearest class and era, and the comment says what is being given up.
VEHICLE_MAP = {
    # GTA III
    "idaho":    ("glendale",  "a 70s American mid-size; SA has no Idaho"),
    "blista":   ("blistac",   "SA renamed it the Blista Compact"),
    "kuruma":   ("previon",   "a Japanese saloon; SA has no Kuruma"),
    "police":   ("copcarla",  "a marked police car; SA's is Los Santos'"),
    "bellyup":  ("boxville",  "Bellyup Fish - a panel van; the livery is lost"),
    "mrwongs":  ("boxville",  "Mr Wong's laundry van; the livery is lost"),
    "mafia":    ("admiral",   "the Mafia Sentinel - a black executive saloon"),
    "yakuza":   ("sultan",    "the Yakuza Stinger - a fast Japanese saloon"),
    "columb":   ("huntley",   "the Cartel Cruiser - a boxy 4x4 wagon"),
    "panlant":  ("yankee",    "Panlantic's construction truck"),
    "toyz":     ("mrwhoop",   "the Toyz van; SA's novelty van is the ice cream one"),
    # Vice City
    "stinger":  ("comet",     "a two-seat European sports car"),
    "rio":      ("tropic",    "a cabin cruiser"),
    "fbicar":   ("washing",   "the FBI Washington, without the badge"),
    "angel":    ("freeway",   "a cruiser motorcycle"),
    "sentxs":   ("sentinel",  "the Sentinel XS is a Sentinel"),
    "gangbur":  ("burrito",   "the Gang Burrito is a Burrito"),
    "pheonix":  ("phoenix",   "Vice City spells it wrong; San Andreas does not"),
    "kaufman":  ("cabbie",    "the Kaufman Cab"),
    "vicechee": ("cheetah",   "the Vice Cheetah, without the police livery"),
}
# When a donor vehicle has no name in San Andreas and no line above. Ordinary before interesting,
# for the same reason the crowd falls back to a stranger in the street rather than to a millionaire.
VEHICLE_FALLBACK = "premier"


def map_vehicle(name, sa_names):
    """(SA model name, why) for one donor vehicle name."""
    low = name.lower()
    if low in sa_names:
        return sa_names[low], "same name"
    hit = VEHICLE_MAP.get(low)
    if hit and hit[0] in sa_names:
        return sa_names[hit[0]], hit[1] or "hand-mapped"
    return VEHICLE_FALLBACK, "no equivalent"


def scm_int(blob, at, floats_are_ieee):
    """(kind, value, bytes consumed) of one immediate script parameter, or None.

    kind is 'int', 'float' or 'var'. Type bytes: 01 int32, 02 global, 03 local, 04 int8,
    05 int16, 06 float - and a float is four IEEE bytes in Vice City and two fixed-point bytes,
    sixteenths of a unit, in GTA III.
    """
    if at >= len(blob):
        return None
    t = blob[at]
    width = {0x04: 1, 0x05: 2, 0x01: 4}.get(t)
    if width:
        if at + 1 + width > len(blob):
            return None
        return "int", int.from_bytes(blob[at + 1:at + 1 + width], "little", signed=True), 1 + width
    if t in (0x02, 0x03):
        if at + 3 > len(blob):
            return None
        return "var", int.from_bytes(blob[at + 1:at + 3], "little"), 3
    if t == 0x06:
        if not floats_are_ieee:
            if at + 3 > len(blob):
                return None
            return "float", int.from_bytes(blob[at + 1:at + 3], "little", signed=True) / 16.0, 3
        if at + 5 > len(blob):
            return None
        return "float", struct.unpack("<f", blob[at + 1:at + 5])[0], 5
    return None


def scm_parked_cars(path, prefix, vehicles, boxes):
    """[(x, y, z, heading, model, col1, col2, alarm, lock, offset)] out of a compiled main.scm.

    See the comment block above for the opcode, the four filters and what each one is worth. The
    counts are returned alongside so a bad import is visible rather than silent.
    """
    with open(path, "rb") as f:
        blob = f.read()
    ieee = prefix != "iii_"
    needle = SCM_CREATE_CAR_GENERATOR.to_bytes(2, "little")
    switch = SCM_SET_CAR_GENERATOR.to_bytes(2, "little")
    counts = collections.Counter()

    # ---- filter 1: shape
    shaped = {}          # start offset -> (values, handle kind, handle index, end offset)
    at = 0
    while True:
        at = blob.find(needle, at)
        if at < 0:
            break
        counts["raw"] += 1
        p = at + 2
        vals, ok = [], True
        for i in range(12):
            r = scm_int(blob, p, ieee)
            if r is None or r[0] == "var" or (i < 4) != (r[0] == "float"):
                ok = False
                break
            vals.append(r[1])
            p += r[2]
        if ok:
            handle = scm_int(blob, p, ieee)
            if handle and handle[0] == "var":
                shaped[at] = (vals, handle[1], p + handle[2])
            else:
                ok = False
        if not ok:
            counts["not the right shape"] += 1
        at += 1

    # ---- filter 2: chain. A generator's neighbour is the switch addressed to its own handle, or
    # another generator starting exactly where it ends. Nothing else has a neighbour.
    ends = {v[2]: start for start, v in shaped.items()}
    kept = {}
    for start, (vals, handle, end) in shaped.items():
        switched = None
        if blob[end:end + 2] == switch:
            h = scm_int(blob, end + 2, ieee)
            c = scm_int(blob, end + 2 + h[2], ieee) if h else None
            if h and h[0] == "var" and h[1] == handle and c and c[0] == "int":
                switched = c[1]
        chained = switched is not None or end in shaped or start in ends
        if not chained:
            counts["no neighbour"] += 1
            continue
        if switched == 0:
            counts["the script switches it off"] += 1
            continue
        kept[start] = vals

    # ---- filters 3 and 4: sense, then ground
    out = []
    for start in sorted(kept):
        x, y, z, angle, model, c1, c2, force, alarm, lock, lo, hi = kept[start]
        if model != -1 and model not in vehicles:
            counts["names no vehicle of this game"] += 1
            continue
        if not -360.0 <= angle <= 360.0 or not 0 <= alarm <= 100 or not 0 <= lock <= 100 \
                or force not in (0, 1) or lo > hi or lo < 0:
            counts["a column out of range"] += 1
            continue
        if boxes and not any(bx[1] <= x <= bx[4] and bx[2] <= y <= bx[5] for bx in boxes):
            counts["outside every zone of this map"] += 1
            continue
        out.append((x, y, z, angle % 360.0, model, c1, c2, alarm, lock, start))
    counts["kept"] = len(out)
    return out, counts


def write_parked_cars(out, generators, vehicles, sa_vehicles, counts, prefix, road_points):
    """parkedcars.dat: where the source game parked a car, over San Andreas' own models."""
    sa_names = {n.lower(): n for _i, (n, _t, _c) in sa_vehicles.items()}
    # The fleet a `random` generator draws from: the source game's own ordinary traffic, each
    # paired the same way a named car is. Boats, bikes, aircraft and everything the source game
    # marks `ignore` - its mission and emergency vehicles - are left out.
    fleet = []
    for _i, (name, kind, klass) in sorted(vehicles.items()):
        if kind == "car" and klass in FLEET_CLASSES:
            fleet.append((name, map_vehicle(name, sa_names)[0]))
    why = collections.Counter()
    rows = []
    for x, y, z, angle, model, c1, c2, alarm, lock, offset in generators:
        if model < 0:
            pair, note = "random/*", "any ordinary car"
        else:
            donor = vehicles[model][0]
            sa, reason = map_vehicle(donor, sa_names)
            why[reason] += 1
            pair, note = "%s/%s" % (donor, sa), donor
        rows.append("car   %9.2f %9.2f %8.2f %6.1f  %-20s %4d %4d %4d %4d   # %s @0x%x"
                    % (x, y, z, angle, pair, c1, c2, alarm, lock, note, offset))

    near = ""
    if road_points:
        # Not a filter - a car parked in a yard is legitimately away from a carriageway - but the
        # number says at a glance whether the mined coordinates are in the same world as the roads.
        # This is the independent check on the whole exercise: the placements come out of a
        # compiled script and the carriageways out of the map files, and nothing connects the two
        # except both being the same city.
        cells = {}
        for px, py in road_points:
            cells.setdefault((int(px // 64), int(py // 64)), []).append((px, py))
        far = 0
        worst = 0.0
        for x, y, _z, _a, _m, _c1, _c2, _al, _lk, _o in generators:
            best = 1e18
            cx, cy = int(x // 64), int(y // 64)
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    for px, py in cells.get((cx + dx, cy + dy), ()):
                        best = min(best, (px - x) ** 2 + (py - y) ** 2)
            best = best ** 0.5
            if best > 40.0:
                far += 1
            worst = max(worst, best if best < 1e8 else 0.0)
        near = ("; %d of them stand more than 40 units from the nearest carriageway node, "
                "furthest %.0f" % (far, worst))

    lines = [
        "# Where %s parked a car, written by tools/import_city.py from the source game's"
        % ("GTA III" if prefix == "iii_" else "Vice City"),
        "# data/main.scm. Neither donor game puts its parked cars in a map file: both create them",
        "# from the mission script with literal coordinates, through the CREATE_CAR_GENERATOR",
        "# opcode 0x014B. See tools/import_city.py for the opcode layout and the four filters",
        "# every candidate had to pass.",
        "#",
        "# These are NOT San Andreas car generators. CTheCarGenerators is a fixed 500-entry save",
        "# block written by array index with positions compressed to int16 * 0.125, which reaches",
        "# +/-4096 - a region beyond that cannot even be expressed in it, and a save made with one",
        "# set of regions would read a different city's cars with another. The plugin owns these,",
        "# spawns them itself, and San Andreas' own block stays exactly what vanilla writes.",
        "#",
        "# Two kinds of line:",
        "#",
        "#   fleet <DONOR/SAMODEL> <DONOR/SAMODEL> ...",
        "#   car   x y z heading <DONOR/SAMODEL> colour1 colour2 alarm lock",
        "#",
        "# fleet is what the source game meant by \"any ordinary car\": its own traffic models,",
        "# each paired with the San Andreas model standing in for it. A car line whose model reads",
        "# random/* is a generator that asked for one of those rather than for something in",
        "# particular - 61 of Vice City's 148 did.",
        "#",
        "# The right-hand side of every pair is the San Andreas model actually used; the left is",
        "# what the source game asked for. That is written down for one reason: these are",
        "# placeholders, and when the donor's own vehicles are imported this file is where they go",
        "# - change SAMODEL to the imported model's name and nothing else has to change, exactly",
        "# as pedgrp.dat pairs a person and weapons.dat pairs a gun.",
        "#",
        "#   x y z     the region's own coordinates; the plugin adds the region origin. A z of",
        "#             -100 means \"on the ground here\", which is the convention in both donor",
        "#             games AND in San Andreas - CCarCtrl::CreateCarForScript compares against",
        "#             the float at 0x859014, which is -100.0 exactly - so it travels untranslated.",
        "#   heading   degrees, 0 facing north, turning anticlockwise. The same in all three games.",
        "#   colour    the SOURCE game's carcols.dat indices, or -1 for \"any colour this model",
        "#             comes in\". Recorded, not applied: a donor palette index means a different",
        "#             colour in San Andreas' own, longer table.",
        "#   alarm     percent chance the source game gave this one an alarm. Recorded, not",
        "#             applied - see the note in tools/import_city.py.",
        "#   lock      percent chance it left it locked. Applied.",
        "#",
        "# %d generator(s) of %d the script contains%s."
        % (counts["kept"], counts["raw"], near),
        "# Left out: %s." % (", ".join("%d %s" % (n, k) for k, n in counts.most_common()
                                       if k not in ("raw", "kept")) or "none"),
        "",
        "fleet " + " ".join("%s/%s" % (d, s) for d, s in fleet),
        "",
    ]
    lines += rows
    with open(os.path.join(out, "parkedcars.dat"), "w", encoding="latin-1") as f:
        f.write("\n".join(lines) + "\n")
    return "parkedcars.dat", fleet, why


def region_road_points(out):
    """[(x, y)] from a region folder's own carnodes.dat, or None if it has none.

    Only used to report how far the mined placements sit from the roads. --parked-cars-only does
    not build the path graph, so it reads back the one the last full import wrote.
    """
    path = os.path.join(out, "carnodes.dat")
    if not os.path.exists(path):
        return None
    points = []
    with open(path, "r", encoding="latin-1") as f:
        for raw in f:
            line = raw.split("#", 1)[0]
            t = [x for x in re.split(r"[,\s]+", line.strip()) if x]
            if len(t) < 8:
                continue
            try:
                points.append((float(t[0]), float(t[1])))
            except ValueError:
                continue
    return points or None


def import_parked_cars(out, src, sa, prefix, boxes, road_points):
    """Mine the source main.scm for its parked cars and write the region's parkedcars.dat.

    Returns the file name, or None when there is nothing to write. Everything it needs it opens
    itself, so it can be run on its own against a region folder that already exists - see
    --parked-cars-only - as well as from the middle of a full import.
    """
    scm_path = find_file(src, "data/main.scm")
    ide_path = find_file(src, "data/default.ide")
    sa_ide_path = find_file(sa, "data/vehicles.ide")
    for what, path in (("data/main.scm", scm_path), ("data/default.ide", ide_path)):
        if not path:
            print("  %s not found - the region gets no parked cars" % what)
            return None
    if not sa_ide_path:
        print("  San Andreas' data/vehicles.ide not found - the region gets no parked cars")
        return None
    vehicles = read_vehicle_ide(ide_path)
    sa_vehicles = read_vehicle_ide(sa_ide_path)
    generators, counts = scm_parked_cars(scm_path, prefix, vehicles, boxes)
    if not generators:
        print("parkedcars.dat: no car generators found in data/main.scm (%d raw match(es)) - the "
              "region gets no parked cars" % counts["raw"])
        return None
    name, fleet, why = write_parked_cars(out, generators, vehicles, sa_vehicles, counts, prefix,
                                         road_points)
    randoms = sum(1 for g in generators if g[4] < 0)
    print("parkedcars.dat: %d parked car(s) of %d raw opcode match(es); %d ask for any ordinary "
          "car and draw on a fleet of %d model(s)"
          % (len(generators), counts["raw"], randoms, len(fleet)))
    print("  left out: %s"
          % (", ".join("%d %s" % (n, k) for k, n in counts.most_common()
                       if k not in ("raw", "kept")) or "none"))
    print("  models over San Andreas' own: %s"
          % ", ".join("%d %s" % (n, w) for w, n in why.most_common()))
    return name


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
    # The GXT keys are namespaced even for a --lite import. --lite leaves out the archive, so there
    # are no model names to rename, but the text and the zone files still travel and they are the
    # ones that were taking San Andreas' strings.
    do_gxt_rename = "--no-rename" not in opts

    dat_name, ide_rel, ipl_rel, col_rel, texdict_rel, zon_rel = read_dat(src)
    prefix = "iii_" if dat_name.lower() == "gta3.dat" else "vc_"
    out = os.path.join(sa, "MapExpander", region)
    # Stale files from an earlier run have to go, because a region's Ide/Ipl/Col keys are wildcards:
    # a file left behind by a different source, or by an older version of this script, is loaded as
    # if it belonged, and an IDE naming models the archive no longer carries is the one thing the
    # streamer does not survive.
    #
    # They are swept at the END of the import, not here. Emptying the folders first and refilling
    # them is fine right up until the run does not finish - and twice now it has not, once when the
    # game held the archive open and once when the machine ran out of memory for the converter. Both
    # left a region with its archive and its tiles and not one IDE, IPL or collision file, which the
    # plugin loads without complaint and then places nothing at all: a city that is discovered,
    # reported and invisible. Writing first and sweeping afterwards means a failed import leaves the
    # region exactly as it was, which is always better than leaving it half destroyed.
    run_started = time.time() - 1.0   # a second's grace for a coarse file-system clock
    for sub in (("tiles",) if lite else ("col", "ide", "ipl", "tiles")):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
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
    # each dictionary is actually asked for. The same pass collects the models the region declares
    # under their ORIGINAL names, which is the set object.dat's rows have to be matched against.
    model_txd = {}
    ide_models = set()
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
                    ide_models.add(tok[1].lower())

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
    # The weapon HUD art, added last so that it lands in the dictionary as it will be written and
    # cannot be lost to the merge above. See the note by hud_texture_name for what San Andreas
    # actually looks for and why the source game's own icon is already sitting in there under the
    # wrong name.
    hud_plan, hud_notes = ({}, []) if lite else weapon_hud_plan(src, sa, renames)
    hud_local, hud_donor, hud_none = 0, 0, []

    shared = []
    for key, chunks, sources in txd_parts.values():
        seen, unique = set(), []
        for texname, chunk in chunks:
            low = texname.lower()
            if low in seen:
                continue
            seen.add(low)
            unique.append((texname, chunk))
        for wanted, local, spare in hud_plan.get(key.lower(), ()):
            if wanted.lower() in seen:
                continue
            source = None
            if local:
                source = next((c for n, c in unique if n.lower() == local.lower()), None)
            if source is not None:
                hud_local += 1
            elif spare is not None:
                source = spare
                hud_donor += 1
            else:
                hud_none.append(wanted)
                continue
            seen.add(wanted.lower())
            unique.append((wanted, rename_texture_chunk(source, wanted)))
        if sources > 1:
            shared.append("%s (%d dictionaries, %d texture(s))" % (key, sources, len(unique)))
        packed.append((key, build_txd(unique)))
    if shared:
        print("merged %d texture dictionary name(s) the source archive carried more than once: %s"
              % (len(shared), ", ".join(sorted(shared))))
    if hud_local or hud_donor:
        print("weapon HUD art: %d texture(s) copied under the name CHud looks for - %d from the "
              "source game's own icon, %d from the San Andreas weapon the import is paired with"
              % (hud_local + hud_donor, hud_local, hud_donor))
    for note in hud_notes:
        print("  %s" % note)
    if hud_none:
        print("  warning: no art for %s, so %s will show no icon rather than a blank one"
              % (", ".join(sorted(hud_none)), "those weapons" if len(hud_none) > 1 else "that weapon"))

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
    # The files that travel byte for byte or nearly so: the source's text, its water heights and
    # its zone boxes. The list is built here rather than at the copy loop below because the GXT
    # keys and the zone labels that name them have to be namespaced together, and names.txt -
    # written a few lines down - records the result along with the model renames.
    #
    # GTA III has neither navig.zon nor info.zon: its zone boxes are in data/gta3.zon, which the
    # .dat loads with an IPL line of its own. The lines are the same nine fields Zones.cpp already
    # reads, with the GXT key optional at the end, so a zone file lands on the region's Zones key
    # rather than its Ipl key - the IPL loader would only count it and move on. The two named
    # paths are a safety net for a .dat that does not list them.
    text_files = ([("text/american.gxt", "american.gxt"), ("data/waterpro.dat", "waterpro.dat"),
                   ("data/navig.zon", "navig.zon"), ("data/info.zon", "info.zon")] +
                  [(z, dat_base(z)) for z in zon_rel])

    gxt_tag = GXT_TAG.get(dat_name.lower(), prefix[:1].upper())
    gxt_renames, gxt_reasons, gxt_text = {}, {}, {}
    gxt_source = find_file(src, "text/american.gxt")
    if gxt_source:
        with open(gxt_source, "rb") as f:
            gxt_blob = f.read()
        # Read whether or not the keys are being renamed: zone_names.txt takes its display names
        # from here either way.
        gxt_text = gxt_strings(gxt_blob)
    if do_gxt_rename and gxt_source:
        # Every key the region could ever be asked for: the ones in its own text, and the zone
        # labels, which are keys in their own right whether or not the GXT defines them. A label
        # left un-namespaced would go on doing exactly what this change exists to stop.
        keys = gxt_key_names(gxt_blob)
        seen = set(keys)
        for _rel, dst in text_files:
            if not dst.endswith(".zon"):
                continue
            p = find_file(src, _rel)
            if not p:
                continue
            with open(p, "r", encoding="latin-1") as f:
                for label in zon_labels(f.read()):
                    if label not in seen:
                        seen.add(label)
                        keys.append(label)
        if not keys:
            print("  text/american.gxt is in San Andreas' own format, which stores hashes and not "
                  "key names; its keys are left as they are and will still override San Andreas' text")
        else:
            sa_gxt = sa_gxt_keys(sa)
            if not sa_gxt:
                print("warning: no GXT found under %s/text; clashes with San Andreas' own keys not checked" % sa)
            gxt_renames, gxt_reasons = make_gxt_renames(keys, sa_gxt, gxt_tag)
            assert_gxt_namespace(gxt_renames, sa_gxt)
            counts = {}
            for why in gxt_reasons.values():
                counts[why] = counts.get(why, 0) + 1
            print("named %d GXT key(s) under the %s tag: %s (0 clash with San Andreas' %d key(s))"
                  % (len(gxt_renames), gxt_tag,
                     ", ".join("%d %s" % (n, w) for w, n in sorted(counts.items(), key=lambda kv: -kv[1])),
                     len(sa_gxt)))

    if renames or gxt_renames:
        counts = {}
        for why in reasons.values():
            counts[why] = counts.get(why, 0) + 1
        if renames:
            print("named %d model/texture entries under the %s tag: %s"
                  % (len(renames), tag, ", ".join("%d %s" % (n, w) for w, n in sorted(counts.items(), key=lambda kv: -kv[1]))))
        with open(os.path.join(out, "names.txt"), "w", encoding="latin-1") as f:
            f.write("# new name, original name, why - written by tools/import_city.py; the loader reads it\n")
            for low in sorted(renames, key=lambda k: renames[k].lower()):
                ext = os.path.splitext(low)[1]
                f.write("%s%s\t%s\t%s\n" % (renames[low], ext, low, reasons[low]))
            # The GXT keys go in the same file so one place answers "what was this called?", under
            # a .gxt extension that tells the loader they are text keys and not model names.
            for old in sorted(gxt_renames, key=lambda k: gxt_renames[k]):
                f.write("%s.gxt\t%s.gxt\t%s\n" % (gxt_renames[old], old, gxt_reasons[old]))

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
    ide_texts, ipl_texts = [], []      # as the source wrote them, for the path sections below
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
        ide_texts.append(text)
        name = os.path.basename(p).lower()
        with open(os.path.join(out, "ide", name), "w", encoding="latin-1") as f:
            f.write(rename_ide(text, renames, ide_drop_flags))
        ide_out.append(name)
    weapons_out, weap_ide_out = (None, None) if lite else write_weapons(out, src, sa, renames, prefix)
    if weap_ide_out:
        ide_out.append(weap_ide_out)
    inst_areas = []
    for rel in ipl_rel:
        p = find_file(src, rel)
        if not p:
            print("  IPL %s not found" % rel)
            continue
        with open(p, "r", encoding="latin-1") as f:
            text = f.read()
        ipl_texts.append(text)
        name = os.path.basename(p).lower()
        inst_areas += area_placements(text)
        with open(os.path.join(out, "ipl", name), "w", encoding="latin-1") as f:
            f.write(rename_ipl(text, renames) if renames else text)
        ipl_out.append(name)
    if not lite:
        print("%d IDE, %d IPL, %d collision files" % (len(ide_out), len(ipl_out), cols))

    # data/object.dat: which of the region's models are physical objects rather than scenery, and
    # how each of them reacts to being hit. Written last because it is matched against the model
    # names the IDEs above declare - a row for a weapon or a pickup has nothing to attach to.
    object_dat = None
    src_object_dat = None if lite else find_file(src, "data/object.dat")
    if src_object_dat:
        text, kept, skipped, notes = convert_object_dat(src_object_dat, renames, ide_models)
        with open(os.path.join(out, "object.dat"), "w", encoding="latin-1") as f:
            f.write(text)
        object_dat = "object.dat"
        print("object.dat: %d model(s) become dynamic objects - breakable, pushable, smashable - "
              "of %d row(s) the source lists; %d name a model no IDE of this region declares"
              % (kept, kept + skipped, skipped))
        for note in notes:
            print("  object.dat: %s" % note)
    elif not lite:
        print("  data/object.dat not found - the region's props will all be static scenery")

    # The pedestrian graph and the crowd that walks it. Both come out of files this script has
    # already opened for other reasons, so the cost is only the reading; both are skipped for a
    # --lite import, which places no map and therefore has no pavement to put anybody on.
    ped_nodes_out, ped_groups_out, car_nodes_out, parked_cars_out = None, None, None, None
    if not lite:
        if prefix == "iii_":
            graph, contributed, negate_w = iii_rotation_sign(ide_texts, ipl_texts, renames,
                                                             VC_PATH_PED)
            source_of_paths = "%d road placement(s) carrying a ped path group" % contributed
            print("paths: GTA III's placement quaternion turns with w %s - it merges %d external "
                  "end(s) that way and fewer the other, which is how the sign was settled"
                  % ("negated" if negate_w else "as written", graph.merges))
        else:
            graph, contributed = vc_path_graph(ipl_texts, VC_PATH_PED)
            source_of_paths = "%d ped path group(s)" % contributed
            negate_w = True
        orphans = graph.drop_isolated()
        if not graph.pos:
            print("no pedestrian path nodes found - the region gets no peds")
        else:
            zone_file = "data/gta3.zon" if prefix == "iii_" else "data/info.zon"
            # Vice City keeps three kinds of zone in three files and only the type 2 boxes of
            # info.zon carry population; GTA III keeps all of its in gta3.zon and the type means
            # something else there, so every box counts.
            zone_types = None if prefix == "iii_" else (2,)
            zone_path = find_file(src, zone_file)
            boxes = []
            if zone_path:
                with open(zone_path, "r", encoding="latin-1") as f:
                    boxes = read_zon_boxes(f.read(), zone_types)
            pedgrp_path = find_file(src, "data/pedgrp.dat")
            groups = read_donor_pedgrp(pedgrp_path) if pedgrp_path else []
            scm_path = find_file(src, "data/main.scm")
            names_in_boxes = set(b[0].upper() for b in boxes)
            assigned = (scm_zone_ped_groups(scm_path, len(groups), names_in_boxes)
                        if scm_path and groups else {})
            # One zone table line per name the script actually assigned; a box whose name was never
            # assigned is left off and its nodes fall back to the default group, which is what the
            # source game does with it too.
            zones = [(name, day, night) for name, (day, night) in sorted(assigned.items())]
            zone_index = {name: i for i, (name, _d, _n) in enumerate(zones)}
            stamped = stamp_zones(graph, boxes, zone_index) if zones else 0

            wanted = sorted(set(n for _c, names in groups for n in names))
            ped_map = build_ped_map(wanted, sa_ped_models(sa), prefix)
            ped_nodes_out = write_path_nodes(out, "pednodes.dat", graph, zones,
                                             "Pedestrian path nodes", False)
            ped_groups_out = write_ped_groups(out, groups, zones, ped_map, prefix)

            # The carriageways, read the same way and out of the same files. These are what the
            # plugin hands to CPathFind once the node encoding has been widened, which is what
            # traffic is made of; the pedestrian graph above stays in the plugin's own memory,
            # because San Andreas' own ped spawner would otherwise draw its own crowd over the top
            # of the source game's districts.
            if prefix == "iii_":
                cars, car_groups = iii_path_graph(ide_texts, ipl_texts, renames, VC_PATH_CAR,
                                                  negate_w)
                car_source = "%d road placement(s) carrying a car path group" % car_groups
            else:
                cars, car_groups = vc_path_graph(ipl_texts, VC_PATH_CAR)
                car_source = "%d car path group(s)" % car_groups
            car_orphans = cars.drop_isolated()
            if cars.pos:
                if zones:
                    stamp_zones(cars, boxes, zone_index)
                car_nodes_out = write_path_nodes(out, "carnodes.dat", cars, zones,
                                                 "Vehicle path nodes", True)
                lanes = collections.Counter(l[1] + l[2] for l in cars.lanes)
                print("carnodes.dat: %d node(s) and %d link(s) from %s%s; lanes per node %s"
                      % (len(cars.pos), sum(len(l) for l in cars.links) // 2, car_source,
                         "" if not car_orphans else
                         ", %d node(s) nothing links to left out" % car_orphans,
                         ", ".join("%d with %d" % (n, k) for k, n in sorted(lanes.items()))))
            else:
                print("no vehicle path nodes found - the region gets no traffic")

            why = collections.Counter(w for _n, w in ped_map.values())
            print("pednodes.dat: %d node(s) and %d link(s) from %s%s; %d stand in one of %d "
                  "district(s) the script gives a crowd"
                  % (len(graph.pos), sum(len(l) for l in graph.links) // 2, source_of_paths,
                     "" if not orphans else ", %d node(s) nothing links to left out" % orphans,
                     stamped, len(zones)))
            print("pedgrp.dat: %d group(s) of the source game's own, %d model(s) over San Andreas' "
                  "own: %s" % (len(groups), len(ped_map),
                               ", ".join("%d %s" % (n, w) for w, n in why.most_common())))
            if boxes and not zones:
                print("  no SET_ZONE_PED_GROUP calls found in data/main.scm - every node takes the "
                      "default group; the crowd will be right for the city and not for the street")

            # The cars the source game parked at its kerbs, mined from the same main.scm and
            # validated against the same zone boxes. Its own block, above, explains the opcode.
            parked_cars_out = import_parked_cars(
                out, src, sa, prefix, boxes,
                [(p[0], p[1]) for p in cars.pos] if cars.pos else None)

    copied, zones_have, gxt_missed = [], [], []
    for rel, dst in text_files:
        p = find_file(src, rel)
        if not p or dst in copied:
            continue
        with open(p, "rb") as f:
            blob = f.read()
        if gxt_renames and dst == "american.gxt":
            # The emitted table is the source file with its TKEY names replaced: same tables, same
            # TDAT blocks, same offsets, different keys. Nothing else understands this file - the
            # game never reads it, Gxt.cpp does - so there is no format to keep faith with.
            blob, gxt_missed = rewrite_gxt(blob, gxt_renames)
        elif gxt_renames and dst.endswith(".zon"):
            blob = rename_zon(blob.decode("latin-1"), gxt_renames).encode("latin-1")
        with open(os.path.join(out, dst), "wb") as g:
            g.write(blob)
        copied.append(dst)
        if dst.endswith(".zon"):
            zones_have.append(dst)
    print("copied: %s" % ", ".join(copied))
    if gxt_missed:
        # Cannot happen with the key list built above, which is the file's own names; if it ever
        # does, the entries kept their original keys and would still override San Andreas' text.
        print("WARNING: %d GXT entr%s kept its original key because no rename was worked out for it "
              "(%s)" % (len(gxt_missed), "y" if len(gxt_missed) == 1 else "ies",
                        ", ".join(sorted(set(gxt_missed))[:8])))

    # Which zone file the Zones key should name. San Andreas has 380 navigation-zone slots and
    # vanilla fills 379, so a region has room for a handful of zones, not for every file the source
    # game loads - Vice City's info.zon alone is 378 lines. navig.zon is the one carrying the area
    # names a player reads off the map, so prefer it where the source has one; GTA III has only
    # gta3.zon and it is the same nine-field format.
    zone_out = [z for z in zones_have if z == "navig.zon"] or zones_have[:1]

    # zone_names.txt: the display name of every zone the loader will create, under its new key.
    # The whole GXT is optional - it is 4,707 strings for the sake of twelve area names - so the
    # names the zones actually need are written out in plain text as well, and the region reads
    # right with [World] Gxt left off. Only types 0 and 1 are listed, which is what CreateZone
    # puts in the navigation array and therefore all the HUD and the pause map can ever name.
    zone_names, seen_keys = [], set()
    was_called = {new: old for old, new in gxt_renames.items()}
    for dst in zone_out:
        with open(os.path.join(out, dst), "r", encoding="latin-1") as f:
            section = None
            for line in f:
                clean = line.split("#")[0].strip()
                tok = [t for t in re.split(r"[,\s]+", clean) if t]
                if len(tok) == 1:
                    section = None if tok[0].lower() == "end" else tok[0].lower()
                    continue
                if section not in (None, "zone") or len(tok) < 9 or tok[1] not in ("0", "1"):
                    continue
                key = (tok[9] if len(tok) >= 10 else tok[0]).upper()
                if key in seen_keys:
                    continue
                seen_keys.add(key)
                # The .zon has already been rewritten, so the display name is looked up under the
                # key this zone came from rather than the one it now carries.
                original = was_called.get(key, key)
                text = gxt_text.get(original)
                note = ""
                if text is None:
                    # No string in the source game's own text either. Spell the original key out
                    # rather than the tagged one - the better half of a bad choice - and say in
                    # the file where the name came from.
                    text = original.replace("_", " ").title()
                    note = "   ; %s has no string in american.gxt" % original
                zone_names.append((key, text, note))
    zone_names_out = None
    if zone_names:
        zone_names_out = "zone_names.txt"
        with open(os.path.join(out, zone_names_out), "w", encoding="latin-1") as f:
            f.write("# key = display name, for the region's Zones lines - written by "
                    "tools/import_city.py.\n# The keys are namespaced; names.txt says what each "
                    "one was called in the source game.\n")
            for key, text, note in zone_names:
                f.write("%s = %s%s\n" % (key, text, note))
        print("%s: %d zone display name(s)" % (zone_names_out, len(zone_names)))

    # Where the source map's own (0,0) sits in San Andreas.
    #
    # Vice City's comes from the VC2SAMP placement. Liberty City's could come from LC2SAMP
    # (spicybung's filterscript, itself a rework of Kye's) - every one of its CreateObject calls is
    # the III IPL position plus a constant, and matching 1102 model names across the two gives
    # SA = III + (-5350, -200, 0) with no scatter at all - and that is where this used to put it.
    #
    # It is not used, because of what it costs. San Andreas files every static entity into a grid
    # that is a fixed 120 x 120 sectors however much ground it covers, so the price of a placement
    # is how far it drags the corners of that grid apart: the cell grows, and every per-frame scan
    # over it - the renderer's, the streamer's, CCollision's camera cache - walks proportionally
    # more entities. LC2SAMP's placement puts Liberty City off the west coast while Vice City sits
    # off the east, which stretches the grid across -7350..7998 and gives 128 unit cells, six and a
    # half times a vanilla sector's ground in every scan. That is a measurable frame rate cost and
    # it is what the west coast placement was reported for.
    #
    # North of San Andreas instead, the two cities bracket it on one axis rather than opposite ends
    # of another: -3000..7998 across and -3000..8000 up, 92 unit cells, a little over three times
    # vanilla instead of six and a half. Same map, same distance from anything, half the scan cost.
    # (The proper answer is to grow the grid's arrays so the cell stays at vanilla's 50 wherever a
    # map is put, which is being written; this is the placement that costs least until then.)
    ORIGIN = {"vc_": (5998.5, 1052.5, -6.0), "iii_": (0.0, 6000.0, 0.0)}
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
    if zone_names_out:
        lines.append("ZoneNames    = %s%s" % (base, zone_names_out))
    lines += ["RegionName   = %s" % ("Vice City" if prefix == "vc_" else "Liberty City")]

    # The pickups the source game's main script leaves around its map. They are not in the map
    # files - III and Vice City create almost all of them from main.scm with literal coordinates -
    # and walking another game's script bytecode is not something worth redoing on every import, so
    # the placements are mined once and kept beside this script. See MapExpander.ini for the
    # columns; the coordinates are the source game's own and the loader adds the region origin.
    pickups_out = None
    mined = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pickups_%s.dat" % region)
    if os.path.exists(mined):
        with open(mined, "rb") as f:
            blob = f.read()
        blob = repoint_pickup_weapons(blob, prefix)
        with open(os.path.join(out, "pickups.dat"), "wb") as f:
            f.write(blob)
        pickups_out = "pickups.dat"
        print("pickups.dat: %d line(s) from %s"
              % (sum(1 for l in blob.decode("latin-1").splitlines()
                     if l.strip() and not l.strip().startswith("#")), mined))
    else:
        print("no %s beside this script; the region gets no pickups.dat" % os.path.basename(mined))
    if pickups_out:
        lines.append("Pickups      = %s%s" % (base, pickups_out))
    if ped_nodes_out:
        lines.append("PedNodes     = %s%s" % (base, ped_nodes_out))
    if ped_groups_out:
        lines.append("PedGroups    = %s%s" % (base, ped_groups_out))
    if car_nodes_out:
        lines.append("CarNodes     = %s%s" % (base, car_nodes_out))
    if parked_cars_out:
        lines.append("ParkedCars   = %s%s" % (base, parked_cars_out))

    # The city's own hospitals and police stations, mined out of the source main.scm the same way -
    # see the block above scm_restart_points. Checked against the zone boxes the region is about to
    # ship, which is why this is done here and not with the pickups: the .zon file has been copied
    # and renamed by now, but the ORIGINAL is what the points have to be measured against, since
    # renaming only touches the label columns and never the boxes. Either file would do; the source
    # one is read so that a --no-rename run and a renamed run mine exactly the same table.
    restart_zone_text = ""
    for dst in zone_out:
        p = find_file(src, "data/%s" % dst)
        if p:
            with open(p, "r", encoding="latin-1") as f:
                restart_zone_text += f.read() + "\n"
    restarts_out = write_restarts(out, src, prefix,
                                  "Vice City" if prefix == "vc_" else "Liberty City",
                                  restart_zone_text)
    if restarts_out:
        lines.append("Restarts     = %s%s" % (base, restarts_out))
    if not lite:
        lines += ["Img          = %s%s.img" % (base, region),
                  "Ide          = " + ", ".join(base + "ide\\" + n for n in ide_out),
                  "Ipl          = " + ", ".join(base + "ipl\\" + n for n in ipl_out),
                  "Col          = %scol\\*.col" % base,
                  "Names        = %snames.txt" % base]
        if object_dat:
            lines.append("ObjectData   = %s%s" % (base, object_dat))
        # One box per interior, measured off the placements rather than written by hand - see
        # interior_boxes. Emitted with the area and a count so the file says what it is looking at.
        boxes = interior_boxes(inst_areas)
        if boxes:
            lines += ["", "; Interiors, one box per room, in this region's own coordinates, worked",
                      "; out from where each area's own pieces are. The plugin switches the room on",
                      "; when the player is inside one; it applies its own margins at the edges, so",
                      "; these are the rooms and not the doorways."]
            live = 0
            for i, (area, x0, y0, z0, x1, y1, z1, n) in enumerate(boxes, 1):
                out_of_reach = z0 > MAX_REACHABLE_FLOOR
                if out_of_reach:
                    lines.append("; This one floats at z %.0f, too high to walk to - see the note in"
                                 " the importer.\n; Switching it on because something flew through"
                                 " that airspace would black out the world." % z0)
                else:
                    live += 1
                lines.append("%sInterior%-4d = %d, %.0f, %.0f, %.0f, %.0f, %.0f, %.0f  ; %d piece(s)"
                             % (";" if out_of_reach else "", i, area, x0, y0, z0, x1, y1, z1, n))
            print("interiors: %d box(es) over %d area(s), measured from %d placement(s)%s"
                  % (live, len({b[0] for b in boxes if b[3] <= MAX_REACHABLE_FLOOR}), len(inst_areas),
                     "" if live == len(boxes) else
                     "; %d left commented, too high to walk to" % (len(boxes) - live)))
    # The emitted GXT carries namespaced keys, so it adds the region's strings to San Andreas'
    # rather than replacing them, and two regions no longer take each other's place either. It is
    # optional all the same: the zone names a player actually reads are in zone_names.txt, and
    # the rest of a donor game's text is only worth loading if something is going to ask for it.
    # The Weapons key is written live now that [World] RegionWeaponMode defaults to Add: the guns
    # become weapon types of their own and San Andreas' own weapons are not touched. Set that key to
    # Replace and the same file writes over them instead, which is the version a player has to agree
    # to rather than be given - so the note is here where it will be read.
    if weapons_out:
        lines += ["", "; The source game's guns, added as weapon types of their own alongside San",
                  "; Andreas' - its own weapons are unchanged. If [World] RegionWeaponMode is set",
                  "; to Replace instead, this same file writes them OVER the nearest San Andreas",
                  "; weapon, for the whole game and not only inside the region; the file says which",
                  "; line would trade what.",
                  "Weapons = %s%s" % (base, weapons_out)]
    lines += ["", "; [World] Gxt takes a comma-separated list; the keys in this file are tagged",
              "; \"%s\" so they add to San Andreas' strings instead of replacing them." % gxt_tag,
              "; Gxt = %samerican.gxt" % base]
    # Everything this run meant to write is written, so anything older in those folders is a leftover
    # from a previous import and goes now - see the note where run_started is taken. Reaching here at
    # all is what makes the sweep safe: a run that died earlier swept nothing and left the region
    # whole.
    stale = 0
    for sub in (("tiles",) if lite else ("col", "ide", "ipl", "tiles")):
        folder = os.path.join(out, sub)
        for name in os.listdir(folder):
            path = os.path.join(folder, name)
            if os.path.isfile(path) and os.path.getmtime(path) < run_started:
                os.remove(path)
                stale += 1
    if stale:
        print("swept %d file(s) left by an earlier import" % stale)

    with open(os.path.join(out, "region.ini"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote %s - paste it into MapExpander.ini" % os.path.join(out, "region.ini"))


def parked_cars_only():
    """--parked-cars-only: write just parkedcars.dat into a region folder that already exists.

    A full import rewrites the archive, the collision and every IDE and IPL, which takes minutes
    and has twice been interrupted with the game holding a file open. Adding one data file to a
    region that is already there should not cost that, so this does only that: it reads the source
    game's script and zone file, writes parkedcars.dat, and adds the one key to the region.ini the
    importer left behind. Nothing else in the folder is touched.
    """
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    if len(args) < 2:
        sys.exit(__doc__)
    src, sa = args[0], args[1]
    region = "vicecity"
    for o in opts:
        if o.startswith("--region="):
            region = o.split("=", 1)[1]
    dat_name = read_dat(src)[0]
    prefix = "iii_" if dat_name.lower() == "gta3.dat" else "vc_"
    out = os.path.join(sa, "MapExpander", region)
    if not os.path.isdir(out):
        sys.exit("%s does not exist - run a full import first" % out)
    print("source: %s (%s)\noutput: %s" % (src, dat_name, out))

    zone_path = find_file(src, "data/gta3.zon" if prefix == "iii_" else "data/info.zon")
    boxes = []
    if zone_path:
        with open(zone_path, "r", encoding="latin-1") as f:
            boxes = read_zon_boxes(f.read(), None if prefix == "iii_" else (2,))
    else:
        print("  no zone file found - the placements are not checked against the map's own boxes")
    name = import_parked_cars(out, src, sa, prefix, boxes, region_road_points(out))
    if not name:
        return

    # Put the key in the region.ini the importer already wrote, replacing any line that is there.
    ini = os.path.join(out, "region.ini")
    key = "ParkedCars   = MapExpander\\%s\\%s" % (region, name)
    if not os.path.exists(ini):
        print("\nno %s - add this line to the region's section in MapExpander.ini:\n  %s"
              % (ini, key))
        return
    with open(ini, "r") as f:
        lines = [l.rstrip("\n") for l in f]
    lines = [l for l in lines if not l.strip().lower().startswith("parkedcars")]
    at = max((i for i, l in enumerate(lines) if l.strip().lower().startswith("carnodes")),
             default=len(lines) - 1)
    lines.insert(at + 1, key)
    with open(ini, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote %s and added\n  %s\nto %s" % (os.path.join(out, name), key, ini))


if __name__ == "__main__":
    if "--parked-cars-only" in sys.argv:
        parked_cars_only()
    else:
        main()
