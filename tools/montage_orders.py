#!/usr/bin/env python3
"""Render a square tile set in all 8 index orderings to find which one yields a coherent map.

Usage: montage_orders.py <folder> <pattern> <n> <out.png>
"""
import os
import sys

from PIL import Image, ImageDraw

folder, pattern, n, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
images = {}
for i in range(n * n):
    path = os.path.join(folder, pattern % i)
    if os.path.exists(path):
        images[i] = Image.open(path).convert("RGBA")
tile = next(iter(images.values())).size[0]

variants = []
for transpose in (False, True):
    for flip_x in (False, True):
        for flip_y in (False, True):
            def index(c, r, transpose=transpose, flip_x=flip_x, flip_y=flip_y):
                cc = n - 1 - c if flip_x else c
                rr = n - 1 - r if flip_y else r
                return (cc * n + rr) if transpose else (rr * n + cc)
            name = f"{'col' if transpose else 'row'}-major{' flipX' if flip_x else ''}{' flipY' if flip_y else ''}"
            variants.append((name, index))

cell = n * tile
pad = 24
sheet = Image.new("RGBA", (4 * (cell + pad), 2 * (cell + pad)), (30, 30, 30, 255))
draw = ImageDraw.Draw(sheet)
for v, (name, index) in enumerate(variants):
    ox, oy = (v % 4) * (cell + pad), (v // 4) * (cell + pad) + pad
    draw.text((ox + 2, oy - pad + 6), f"{v}: {name}", fill=(255, 255, 0, 255))
    for r in range(n):
        for c in range(n):
            i = index(c, r)
            if i in images:
                sheet.paste(images[i], (ox + c * tile, oy + r * tile))
sheet.save(out)
print("wrote", out)
