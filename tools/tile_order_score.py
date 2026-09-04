#!/usr/bin/env python3
"""Score the 8 index orderings of a square tile set by edge continuity (lower = more coherent).

Usage: tile_order_score.py <folder> <pattern> <n>
"""
import os
import sys

from PIL import Image

folder, pattern, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
tiles = {}
for i in range(n * n):
    p = os.path.join(folder, pattern % i)
    if os.path.exists(p):
        tiles[i] = Image.open(p).convert("RGB")
size = next(iter(tiles.values())).size[0]


def column(img, x):
    return [img.getpixel((x, y)) for y in range(size)]


def row(img, y):
    return [img.getpixel((x, y)) for x in range(size)]


def diff(a, b):
    return sum(abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]) for p, q in zip(a, b)) / len(a)


results = []
for tile_flip in ("none", "flipV", "flipH", "rot180"):
  src = tiles
  if tile_flip == "flipV":
      tiles = {k: v.transpose(Image.FLIP_TOP_BOTTOM) for k, v in src.items()}
  elif tile_flip == "flipH":
      tiles = {k: v.transpose(Image.FLIP_LEFT_RIGHT) for k, v in src.items()}
  elif tile_flip == "rot180":
      tiles = {k: v.transpose(Image.ROTATE_180) for k, v in src.items()}
  for transpose in (False, True):
    for flip_x in (False, True):
        for flip_y in (False, True):
            def index(c, r):
                cc = n - 1 - c if flip_x else c
                rr = n - 1 - r if flip_y else r
                return (cc * n + rr) if transpose else (rr * n + cc)
            total, edges = 0.0, 0
            for r in range(n):
                for c in range(n):
                    a = tiles.get(index(c, r))
                    if a is None:
                        continue
                    if c + 1 < n and index(c + 1, r) in tiles:
                        total += diff(column(a, size - 1), column(tiles[index(c + 1, r)], 0)); edges += 1
                    if r + 1 < n and index(c, r + 1) in tiles:
                        total += diff(row(a, size - 1), row(tiles[index(c, r + 1)], 0)); edges += 1
            name = f"{'col' if transpose else 'row'}-major{' flipX' if flip_x else ''}{' flipY' if flip_y else ''} / tiles {tile_flip}"
            results.append((total / max(edges, 1), name))
  tiles = src
for score, name in sorted(results)[:12]:
    print(f"{score:8.2f}  {name}")
