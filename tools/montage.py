#!/usr/bin/env python3
"""Stitch numbered tiles into one labelled image to check grid order.

Usage: montage.py <folder> <pattern e.g. radar%02d.png> <cols> <rows> <out.png> [scale]
Index = row * cols + col (row 0 at the top).
"""
import os
import sys

from PIL import Image, ImageDraw

folder, pattern, cols, rows, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
scale = int(sys.argv[6]) if len(sys.argv) > 6 else 2

tile = None
images = {}
for i in range(cols * rows):
    path = os.path.join(folder, pattern % i)
    if os.path.exists(path):
        img = Image.open(path).convert("RGBA")
        images[i] = img
        tile = tile or img.size[0]
tile = (tile or 128) * scale

sheet = Image.new("RGBA", (cols * tile, rows * tile), (40, 40, 40, 255))
draw = ImageDraw.Draw(sheet)
for r in range(rows):
    for c in range(cols):
        i = r * cols + c
        x, y = c * tile, r * tile
        if i in images:
            sheet.paste(images[i].resize((tile, tile)), (x, y))
        draw.rectangle([x, y, x + tile - 1, y + tile - 1], outline=(255, 0, 0, 255))
        draw.text((x + 4, y + 4), f"{i} ({c},{r})", fill=(255, 255, 0, 255))
sheet.save(out)
print(f"wrote {out}: {cols}x{rows} tiles of {tile}px, {len(images)} images found")
