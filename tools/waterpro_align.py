#!/usr/bin/env python3
"""Check a III/VC waterpro.dat against a radar tile set: what fraction of water cells land on
water-coloured tile pixels for a few candidate X offsets.

Usage: waterpro_align.py <waterpro.dat> <tile folder> <pattern> <n tiles per side> <local min> <local max>
"""
import os
import struct
import sys

from PIL import Image

path, folder, pattern, n, lo, hi = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6])
data = open(path, "rb").read()
fine_at = 4 + 48 * 4 + 48 * 16 + 64 * 64
fine = data[fine_at:fine_at + 128 * 128]

tile = Image.open(os.path.join(folder, pattern % 0)).size[0]
sheet = Image.new("RGB", (n * tile, n * tile))
for i in range(n * n):
    p = os.path.join(folder, pattern % i)
    if os.path.exists(p):
        sheet.paste(Image.open(p).convert("RGB"), ((i % n) * tile, (i // n) * tile))
px = sheet.load()
size = n * tile
units_per_px = (hi - lo) / size


def is_water(r, g, b):
    return b > 150 and b > r + 40 and b > g + 10


water_cells = [(ix, iy) for ix in range(128) for iy in range(128) if fine[ix * 128 + iy] != 0x80 and fine[ix * 128 + iy] < 48]
print(f"{len(water_cells)} water cells of 16384")

for xoff in (0.0, 400.0, -400.0, 200.0, -200.0):
    for yoff in (0.0, 400.0, -400.0):
        hits = total = 0
        for ix, iy in water_cells:
            cx = -2048.0 - xoff + 32.0 * ix + 16.0
            cy = -2048.0 - yoff + 32.0 * iy + 16.0
            sx = int((cx - lo) / units_per_px)
            sy = int((hi - cy) / units_per_px)
            if 0 <= sx < size and 0 <= sy < size:
                total += 1
                if is_water(*px[sx, sy]): hits += 1
        print(f"xoff {xoff:6.0f} yoff {yoff:6.0f}: {hits}/{total} = {hits / max(total, 1):.3f}")
