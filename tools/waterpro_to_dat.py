#!/usr/bin/env python3
"""Convert a GTA III / Vice City waterpro.dat into a San Andreas water.dat.

waterpro.dat layout (identical in III and VC, see re3/reVC src/render/WaterLevel.cpp):

    int32 nNoOfWaterLevels
    float aWaterZs[48]
    CRect aWaterRects[48]                  (left, bottom, right, top; unused here)
    int8  aWaterBlockList[64][64]          (coarse grid; unused here)
    int8  aWaterFineBlockList[128][128]    (fine grid, [xSector][ySector])

The fine grid has 32-unit cells starting at WATER_START = -2048. A cell holds an index into
aWaterZs, or NO_WATER (-128). Vice City's own lookup adds WATER_X_OFFSET (400) before indexing,
so cell x really covers world X 32*x - 2048 - 400; pass --x-offset 400 for VC, 0 for III.

Cells sharing a level are greedy-merged into rectangles (widest run first, then grown downward
as long as the whole width matches) and emitted as flat SA quads with no flow and no waves.

Usage:
    waterpro_to_dat.py waterpro.dat [-o water.dat] [--origin X Y] [--x-offset 400] [--z-offset 0]
"""
import argparse
import struct
import sys

FINE_CELLS = 128
CELL_SIZE = 32
WATER_START = -2048
NO_WATER = -128
HEADER = 4 + 48 * 4 + 48 * 16 + 64 * 64
EXPECTED_SIZE = HEADER + FINE_CELLS * FINE_CELLS


def read_waterpro(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < EXPECTED_SIZE:
        sys.exit(f"{path}: {len(data)} bytes, expected at least {EXPECTED_SIZE}")
    count = struct.unpack_from("<i", data, 0)[0]
    levels = struct.unpack_from("<48f", data, 4)
    fine = struct.unpack_from(f"<{FINE_CELLS * FINE_CELLS}b", data, HEADER)
    return count, levels, fine


def merge(fine):
    """Greedy-merge equal-level cells into (ix, iy, w, h, level index) rectangles."""
    used = bytearray(FINE_CELLS * FINE_CELLS)
    rects = []
    for ix in range(FINE_CELLS):
        for iy in range(FINE_CELLS):
            here = ix * FINE_CELLS + iy
            idx = fine[here]
            if used[here] or idx == NO_WATER or idx < 0 or idx >= 48:
                continue

            w = 1
            while (ix + w < FINE_CELLS and not used[(ix + w) * FINE_CELLS + iy]
                   and fine[(ix + w) * FINE_CELLS + iy] == idx):
                w += 1

            h = 1
            while iy + h < FINE_CELLS and all(
                not used[(ix + k) * FINE_CELLS + iy + h] and fine[(ix + k) * FINE_CELLS + iy + h] == idx
                for k in range(w)
            ):
                h += 1

            for a in range(w):
                for b in range(h):
                    used[(ix + a) * FINE_CELLS + iy + b] = 1
            rects.append((ix, iy, w, h, idx))
    return rects


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("waterpro")
    ap.add_argument("-o", "--out", default="-", help="output water.dat (default: stdout)")
    ap.add_argument("--origin", nargs=2, type=float, default=[0.0, 0.0], metavar=("X", "Y"),
                    help="world position of the source map's (0,0); added to every vertex")
    ap.add_argument("--x-offset", type=float, default=0.0,
                    help="WATER_X_OFFSET of the source game: 400 for Vice City, 0 for GTA III")
    ap.add_argument("--z-offset", type=float, default=0.0,
                    help="added to every water level (III uses WATER_Z_OFFSET 1.5, VC 0.5)")
    ap.add_argument("--flag", type=int, default=1, help="SA quad flag word (1 = visible)")
    args = ap.parse_args()

    count, levels, fine = read_waterpro(args.waterpro)
    rects = merge(fine)

    base_x = args.origin[0] + WATER_START - args.x_offset
    base_y = args.origin[1] + WATER_START

    lines = []
    for ix, iy, w, h, idx in rects:
        x1 = base_x + ix * CELL_SIZE
        x2 = base_x + (ix + w) * CELL_SIZE
        y1 = base_y + iy * CELL_SIZE
        y2 = base_y + (iy + h) * CELL_SIZE
        z = levels[idx] + args.z_offset
        cols = []
        for vx, vy in ((x1, y1), (x2, y1), (x1, y2), (x2, y2)):
            cols += [f"{vx:.0f}", f"{vy:.0f}", f"{z:.3f}", "0", "0", "0", "0"]
        cols.append(str(args.flag))
        lines.append(" ".join(cols))

    header = (f"; converted from {args.waterpro}: {count} water level(s), "
              f"{len(rects)} quad(s), origin {args.origin[0]:.0f} {args.origin[1]:.0f}, "
              f"x-offset {args.x_offset:.0f}\n")
    text = header + "\n".join(lines) + "\n"
    if args.out == "-":
        sys.stdout.write(text)
    else:
        with open(args.out, "w") as f:
            f.write(text)
        print(f"{args.out}: {len(rects)} quads from {count} water levels", file=sys.stderr)


if __name__ == "__main__":
    main()
