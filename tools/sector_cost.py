#!/usr/bin/env python3
"""Measure what a world-sector scan actually costs, from the real map data.

San Andreas files every static entity into a fixed 120 x 120 grid, so the wider the world the
grid has to cover, the more entities sit in the cell a query walks. docs/known-limitations.md
states that cost as the cell's AREA, which is the right model for a uniform scattering of small
entities but not obviously right for a real city. This reads the actual `inst` placements - San
Andreas' own out of gta3.img and data/maps, each region's out of its converted IPLs - bins them
into the grid a given world span produces, and reports the number of entities in the cell a
player standing on the map is actually in.

Usage:  sector_cost.py <GTA San Andreas dir>
"""
import os, struct, sys, glob, math

SECTOR = 2048


def img_entries(path):
    """(name, bytes) for every entry of a VER2 archive."""
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"VER2":
            return
        count = struct.unpack("<I", head[4:])[0]
        table = f.read(count * 32)
        for i in range(count):
            off, streamed, insize, name = struct.unpack("<IHH24s", table[i * 32:i * 32 + 32])
            name = name.split(b"\0")[0].decode("ascii", "replace")
            size = (insize or streamed) * SECTOR
            f.seek(off * SECTOR)
            yield name, f.read(size)


def binary_ipl_insts(data):
    """(x, y, z, modelId, lod) for every inst in an SA binary IPL."""
    if len(data) < 0x34 or data[:4] != b"bnry":
        return
    num_inst = struct.unpack("<I", data[4:8])[0]
    off_inst = struct.unpack("<I", data[0x1C:0x20])[0]
    for i in range(num_inst):
        at = off_inst + i * 40
        if at + 40 > len(data):
            return
        x, y, z = struct.unpack("<3f", data[at:at + 12])
        model, interior, lod = struct.unpack("<3i", data[at + 28:at + 40])
        yield x, y, z, model, lod


def text_ipl_insts(text):
    """(x, y, z, name, lod) for every inst line, in whichever of the three dialects it is."""
    section = None
    for raw in text.splitlines():
        line = raw.split("#")[0].strip()
        if not line:
            continue
        low = line.lower()
        if low == "end":
            section = None
            continue
        if "," not in line:
            section = low
            continue
        if section != "inst":
            continue
        f = [t.strip() for t in line.split(",")]
        # GTA III: id, model, x, y, z, sx,sy,sz, rx,ry,rz,rw            -> 12 fields, x at 2
        # Vice City: id, model, interior, x, y, z, sx,sy,sz, rx,ry,rz,rw -> 13 fields, x at 3
        # San Andreas: id, model, interior, x, y, z, rx,ry,rz,rw, lod    -> 11 fields, x at 3
        at = 2 if len(f) == 12 else 3
        if len(f) < at + 3:
            continue
        try:
            x, y, z = float(f[at]), float(f[at + 1]), float(f[at + 2])
        except ValueError:
            continue
        lod = -1
        if len(f) == 11:
            try:
                lod = int(f[10])
            except ValueError:
                lod = -1
        yield x, y, z, f[1], lod


def san_andreas_entities(sa_dir):
    """Every static placement San Andreas ships, as (x, y, isLod)."""
    out = []
    img = os.path.join(sa_dir, "models", "gta3.img")
    if os.path.exists(img):
        for name, data in img_entries(img):
            if not name.lower().endswith(".ipl"):
                continue
            for x, y, z, model, lod in binary_ipl_insts(data):
                out.append((x, y, False))
    for root, _dirs, files in os.walk(os.path.join(sa_dir, "data", "maps")):
        for n in files:
            if not n.lower().endswith(".ipl"):
                continue
            with open(os.path.join(root, n), "r", errors="replace") as f:
                for x, y, z, name, lod in text_ipl_insts(f.read()):
                    out.append((x, y, name.lower().startswith("lod")))
    return out


def region_entities(folder, origin_x, origin_y):
    out = []
    for p in sorted(glob.glob(os.path.join(folder, "ipl", "*.ipl"))):
        with open(p, "r", errors="replace") as f:
            for x, y, z, name, lod in text_ipl_insts(f.read()):
                out.append((x + origin_x, y + origin_y, False))
    return out


def grid_counts(entities, lo, hi, n):
    """entities per cell, keyed (col,row), for a grid of n cells spanning lo..hi."""
    scale = n / (hi - lo)
    cells = {}
    for x, y, _lod in entities:
        cx = min(n - 1, max(0, int((x - lo) * scale)))
        cy = min(n - 1, max(0, int((y - lo) * scale)))
        cells[(cx, cy)] = cells.get((cx, cy), 0) + 1
    return cells


def scan_cost(entities, sample, lo, hi, n):
    """Mean entities in the cell a sampled world position falls in.

    The sample is the set of positions a player plausibly stands at. Using the entities' own
    positions weights it by where the map actually is, which is what a frame time cares about:
    nobody spends time in empty ocean cells.
    """
    cells = grid_counts(entities, lo, hi, n)
    scale = n / (hi - lo)
    total = 0
    worst = 0
    for x, y, _lod in sample:
        cx = min(n - 1, max(0, int((x - lo) * scale)))
        cy = min(n - 1, max(0, int((y - lo) * scale)))
        c = cells.get((cx, cy), 0)
        total += c
        worst = max(worst, c)
    return total / max(1, len(sample)), worst, len(cells)


def main():
    sa_dir = sys.argv[1]
    mx = os.path.join(sa_dir, "MapExpander")

    sa = san_andreas_entities(sa_dir)
    print("San Andreas: %d static placements" % len(sa))
    xs = [e[0] for e in sa]
    ys = [e[1] for e in sa]
    print("  extent x %.0f..%.0f  y %.0f..%.0f" % (min(xs), max(xs), min(ys), max(ys)))

    regions = []
    for name, ox, oy in (("vicecity", 5998.5, 1052.5), ("libertycity", 0.0, 6000.0)):
        folder = os.path.join(mx, name)
        if not os.path.isdir(folder):
            continue
        ents = region_entities(folder, ox, oy)
        rx = [e[0] for e in ents]
        ry = [e[1] for e in ents]
        print("%s: %d placements, placed extent x %.0f..%.0f y %.0f..%.0f "
              "(own footprint %.0f x %.0f)"
              % (name, len(ents), min(rx), max(rx), min(ry), max(ry),
                 max(rx) - min(rx), max(ry) - min(ry)))
        regions.append((name, ents, min(rx) - ox, max(rx) - ox, min(ry) - oy, max(ry) - oy))

    # ---- what the shipped world costs, measured rather than modelled -------------------------
    print()
    print("%-46s %6s %7s %9s %9s %8s" % ("configuration", "span", "cell", "mean/cell", "worst",
                                         "vs 50u"))
    base = None
    cases = [("San Andreas alone (vanilla grid)", sa, -3000.0, 3000.0)]
    both = sa + [e for _n, ents, *_ in regions for e in ents]
    if regions:
        lo = min([-3000.0] + [e[0] for _n, ents, *_ in regions for e in ents]
                 + [e[1] for _n, ents, *_ in regions for e in ents])
        hi = max([3000.0] + [e[0] for _n, ents, *_ in regions for e in ents]
                 + [e[1] for _n, ents, *_ in regions for e in ents])
        cases.append(("San Andreas + Vice City + Liberty City (shipped)", both, lo, hi))
    for label, ents, lo, hi in cases:
        mean, worst, used = scan_cost(ents, ents, lo, hi, 120)
        cell = (hi - lo) / 120.0
        if base is None:
            base = mean
        print("%-46s %6.0f %7.1f %9.1f %9d %8.2fx"
              % (label, hi - lo, cell, mean, worst, mean / base))

    # ---- the same entities, at a range of world spans -----------------------------------------
    print()
    print("San Andreas' own geometry alone, re-binned as the world grows around it:")
    print("%8s %8s %11s %9s %8s" % ("span", "cell", "mean/cell", "worst", "vs 50u"))
    for span in (6000, 7000, 8000, 9000, 10000, 10500, 11000, 12000, 13000, 15348, 19500):
        lo = -3000.0
        hi = lo + span
        mean, worst, used = scan_cost(sa, sa, lo, hi, 120)
        print("%8d %8.1f %11.1f %9d %8.2fx"
              % (span, span / 120.0, mean, worst, mean / base))

    # ---- where San Andreas has no geometry at all: room for a city inside +/-3000 -------------
    print()
    occupied = grid_counts(sa, -3000.0, 3000.0, 120)
    free_rows = []
    for row in range(120):
        line = "".join("#" if occupied.get((col, row), 0) else "." for col in range(120))
        free_rows.append(line)
    empty = sum(line.count(".") for line in free_rows)
    print("San Andreas leaves %d of 14400 50-unit cells with no static entity at all (%.0f%%), "
          "= %.1fM square units" % (empty, 100.0 * empty / 14400, empty * 2500 / 1e6))
    print("largest empty axis-aligned square inside +/-3000: %d cells (%d units) at %s"
          % largest_empty_square(occupied, 120))
    print()
    print("occupancy, 120 x 120 cells of 50 units, row 0 = y -3000 (south):")
    for row in range(119, -1, -2):
        print("  " + "".join("#" if (occupied.get((c, row), 0) or occupied.get((c, row - 1), 0))
                             else "." for c in range(120)))


def largest_empty_square(occupied, n):
    """Side (in cells), side (in units), and the corner of the biggest all-empty square."""
    best, at = 0, (0, 0)
    dp = [[0] * n for _ in range(n)]
    for r in range(n):
        for c in range(n):
            if occupied.get((c, r), 0):
                dp[r][c] = 0
                continue
            if r == 0 or c == 0:
                dp[r][c] = 1
            else:
                dp[r][c] = 1 + min(dp[r - 1][c], dp[r][c - 1], dp[r - 1][c - 1])
            if dp[r][c] > best:
                best, at = dp[r][c], (c, r)
    x = -3000 + (at[0] - best + 1) * 50
    y = -3000 + (at[1] - best + 1) * 50
    return best, best * 50, "(%d, %d)" % (x, y)


if __name__ == "__main__":
    main()
