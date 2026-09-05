#!/usr/bin/env python3
"""Turn a GTA III / Vice City install into a MapExpander region folder.

    import_city.py <source game dir> <SA game dir> [--region=vicecity] [--lite] [--no-rename]

Writes <SA game dir>/MapExpander/<region>/ with

    <region>.img   every .dff/.txd from the source gta3.img, repacked as an SA "VER2" archive
    col/           every .col from the archive plus the COLFILE entries of the source .dat
    ide/, ipl/     the IDE and IPL files the source .dat lists, in its order
    tiles/         radar00.png .. radarNN.png decoded from the radar TXDs
    american.gxt, waterpro.dat, navig.zon, info.zon (whatever the source has)
    region.ini     the [region] keys to paste into MapExpander.ini, lists in .dat order

--lite writes only the last three groups (tiles, text/water/zones, region.ini): what a region
needs when something else - a server, a map mod - already places the buildings.

Model and texture names that already exist in SA's gta3.img / gta_int.img lose to SA's copy at
run time, so those get a "vc_" (or "iii_") prefix here - in the archive, the IDE model/txd
columns, the IPL inst lines and the collision entry names, all together. --no-rename skips that.
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


def read_dat(root):
    """IDE / IPL / COLFILE lines of gta_vc.dat or gta3.dat, in order."""
    for name in ("data/gta_vc.dat", "data/gta3.dat"):
        path = find_file(root, name)
        if path:
            break
    else:
        sys.exit("no data/gta_vc.dat or data/gta3.dat under %s" % root)
    ides, ipls, cols = [], [], []
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
                ipls.append(tok[1])
            elif key == "COLFILE" and len(tok) > 2:
                cols.append(tok[2])
    return os.path.basename(path), ides, ipls, cols


# ---------------------------------------------------------------- renaming clashes

def rename_ide(text, renames):
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
        if section in ("objs", "tobj", "anim") and not s.startswith("#"):
            tok = [t.strip() for t in re.split(r"[,\s]+", line.strip()) if t.strip()]
            if len(tok) >= 3:
                tok[1] = renames.get(tok[1].lower() + ".dff", tok[1])
                tok[2] = renames.get(tok[2].lower() + ".txd", tok[2])
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


def rename_col(data, renames):
    """A .col archive is a run of COLL/COL2/COL3 entries: fourcc, u32 size, char name[22], u16 id."""
    out = bytearray(data)
    at = 0
    while at + 8 <= len(out):
        fourcc = bytes(out[at:at + 4])
        if fourcc not in (b"COLL", b"COL2", b"COL3", b"COL4"):
            break
        size = struct.unpack_from("<I", out, at + 4)[0]
        name = bytes(out[at + 8:at + 30]).split(b"\0", 1)[0].decode("latin-1")
        new = renames.get(name.lower() + ".dff")
        if new:
            out[at + 8:at + 30] = new.encode("latin-1")[:21].ljust(22, b"\0")
        at += 8 + size
    return bytes(out)


def make_renames(names, taken, prefix):
    renames = {}
    for name in names:
        low = name.lower()
        if low not in taken:
            continue
        base, ext = os.path.splitext(name)
        new_base = (prefix + base)[:23 - len(ext)]
        renames[low] = new_base
    return renames


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

    dat_name, ide_rel, ipl_rel, col_rel = read_dat(src)
    prefix = "iii_" if dat_name.lower() == "gta3.dat" else "vc_"
    out = os.path.join(sa, "MapExpander", region)
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
    print("archive: %d entries" % len(entries))

    taken = set()
    if do_rename:
        for name in ("models/gta3.img", "models/gta_int.img"):
            p = find_file(sa, name)
            if p:
                taken |= read_img_v2_names(p)
        if not taken:
            print("warning: SA's models/gta3.img not found under %s; name clashes not checked" % sa)
    renames = make_renames([e[0] for e in entries], taken, prefix) if do_rename else {}

    def renamed(name):
        new_base = renames.get(name.lower())
        return new_base + os.path.splitext(name)[1] if new_base else name

    # Split the archive: models/textures into the new IMG, collision into loose files,
    # radar tiles into PNGs. Everything else (audio, misc) is dropped.
    packed, cols, tiles, skipped = [], 0, 0, 0
    txd_parts = {}
    tex_done, tex_failed, txds = 0, 0, 0
    for name, off, size in entries:
        blob = archive[off:off + size]
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
                f.write(rename_col(blob, renames))
            cols += 1
        elif ext == ".txd":
            txds += 1
            chunks, done, bad = txd_texture_chunks(blob)
            tex_done += done
            tex_failed += bad
            key = renamed(name)
            if chunks is None:
                packed.append((key, blob))          # not a dictionary; pass it through untouched
            else:
                part = txd_parts.setdefault(key.lower(), [key, [], 0])
                part[1].extend(chunks)
                part[2] += 1
        elif ext == ".dff":
            packed.append((renamed(name), blob))
        else:
            skipped += 1
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
        print("textures: %d raster(s) in %d TXD(s) rewritten from D3D8 to San Andreas' D3D9%s"
              % (tex_done, txds, "" if not tex_failed else
                 "; %d left alone and will render white" % tex_failed))
    if renames:
        print("renamed %d entries that also exist in SA's archives (prefix %s): %s%s"
              % (len(renames), prefix, ", ".join(sorted(renames)[:12]), ", ..." if len(renames) > 12 else ""))

    for rel in ([] if lite else col_rel):
        p = find_file(src, rel)
        if not p:
            print("  COLFILE %s not found" % rel)
            continue
        with open(p, "rb") as f:
            blob = f.read()
        with open(os.path.join(out, "col", os.path.basename(p)), "wb") as f:
            f.write(rename_col(blob, renames))
        cols += 1

    ide_out, ipl_out = [], []
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
            f.write(rename_ide(text, renames) if renames else text)
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

    copied = []
    for rel, dst in (("text/american.gxt", "american.gxt"), ("data/waterpro.dat", "waterpro.dat"),
                     ("data/navig.zon", "navig.zon"), ("data/info.zon", "info.zon")):
        p = find_file(src, rel)
        if p:
            with open(p, "rb") as f, open(os.path.join(out, dst), "wb") as g:
                g.write(f.read())
            copied.append(dst)
    print("copied: %s" % ", ".join(copied))

    base = "MapExpander\\%s\\" % region
    lines = ["[%s]" % region,
             "TileFolder   = %stiles" % base,
             "FilePattern  = radar%02d.png",
             "TileSize     = 500",
             "TilesX       = 8",
             "TilesY       = 8",
             "GridMinX     = -2000",
             "GridMaxY     = 2000",
             "WorldOriginX = 5998.5", "WorldOriginY = 1052.5", "WorldOriginZ = -6"]
    if "waterpro.dat" in copied:
        lines += ["Water        = %swaterpro.dat" % base, "WaterXOffset = %d" % (400 if prefix == "vc_" else 0)]
    if "navig.zon" in copied:
        lines.append("Zones        = %snavig.zon" % base)
    lines += ["RegionName   = %s" % ("Vice City" if prefix == "vc_" else "Liberty City")]
    if not lite:
        lines += ["Img          = %s%s.img" % (base, region),
                  "Ide          = " + ", ".join(base + "ide\\" + n for n in ide_out),
                  "Ipl          = " + ", ".join(base + "ipl\\" + n for n in ipl_out),
                  "Col          = %scol\\*.col" % base]
    lines += ["", "; [World]", "; Gxt = %samerican.gxt" % base]
    with open(os.path.join(out, "region.ini"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote %s - paste it into MapExpander.ini (origin values are Vice City's; adjust for another map)"
          % os.path.join(out, "region.ini"))


if __name__ == "__main__":
    main()
