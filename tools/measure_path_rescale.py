#!/usr/bin/env python3
"""What re-expressing San Andreas' own path nodes at a coarser scale actually costs, measured
against all 64 shipped node files rather than argued from the arithmetic.

Vanilla stores a coordinate as int16 s, world = s * 0.125. At scale 4 the same point becomes
round(s/2) * 0.25. A coordinate whose s is even survives exactly; an odd one moves by 0.125, which
is precisely one step of the format the game shipped with. This prints how many of each there
really are, and how that compares with how far apart the nodes are in the first place.
"""
import glob
import math
import os
import struct
import sys

NODE_SIZE = 0x1C
LINK_SIZE = 0x0E
NODE_COORDS = (8, 0x0A, 0x0C)
LINK_COORDS = (0, 2)


def rescale(s, factor):
    """round-half-to-even, which is what lrintf does in the default rounding mode."""
    v = s * factor
    r = math.floor(v)
    frac = v - r
    if frac > 0.5:
        r += 1
    elif frac == 0.5 and (r % 2) != 0:
        r += 1
    return int(r)


def main():
    root = sys.argv[1]
    factor = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5   # scale 4 / vanilla 8
    newstep = 0.125 / factor

    files = sorted(glob.glob(os.path.join(root, "*.[Dd][Aa][Tt]")))
    total = odd = 0
    linktotal = linkodd = 0
    nodes_all = 0
    worst = 0.0
    nearest = []
    for path in files:
        base = os.path.basename(path).lower()
        if not base.startswith("nodes"):
            continue
        blob = open(path, "rb").read()
        numNodes, numVeh, numPed, numLinks14, numLinks = struct.unpack_from("<5i", blob, 0)
        at = 20
        if numNodes < 0 or at + numNodes * NODE_SIZE > len(blob):
            print("  %s: header does not fit the file, skipped" % base)
            continue
        nodes_all += numNodes
        pts = []
        for i in range(numNodes):
            rec = at + i * NODE_SIZE
            xyz = []
            for off in NODE_COORDS:
                s = struct.unpack_from("<h", blob, rec + off)[0]
                total += 1
                if s % 2:
                    odd += 1
                new = rescale(s, factor)
                err = abs(new * newstep - s * 0.125)
                worst = max(worst, err)
                xyz.append(s * 0.125)
            pts.append(xyz)
        # How finely the graph is sampled: for a sample of nodes, the distance to the closest
        # other node in the same file. The introduced error only matters next to this number.
        step = max(1, len(pts) // 60)
        for i in range(0, len(pts), step):
            ax, ay, az = pts[i]
            best = None
            for j, (bx, by, bz) in enumerate(pts):
                if j == i:
                    continue
                d = (ax - bx) ** 2 + (ay - by) ** 2 + (az - bz) ** 2
                if best is None or d < best:
                    best = d
            if best:
                nearest.append(math.sqrt(best))
        at += numNodes * NODE_SIZE
        if numLinks14 > 0 and at + numLinks14 * LINK_SIZE <= len(blob):
            for i in range(numLinks14):
                rec = at + i * LINK_SIZE
                for off in LINK_COORDS:
                    s = struct.unpack_from("<h", blob, rec + off)[0]
                    linktotal += 1
                    if s % 2:
                        linkodd += 1

    nearest.sort()

    def pct(q):
        return nearest[min(len(nearest) - 1, q * len(nearest) // 100)] if nearest else 0.0

    print("San Andreas' own path data, all %d node file(s)" % len(files))
    print("  %d nodes, %d node coordinates, %d car path link coordinates"
          % (nodes_all, total, linktotal))
    print()
    print("  rescaling int16 x 0.125 -> int16 x %.3g" % newstep)
    print("  node coordinates that survive exactly (even s): %d of %d = %.1f%%"
          % (total - odd, total, 100.0 * (total - odd) / max(1, total)))
    print("  node coordinates that move: %d = %.1f%%, each by exactly %.4g units"
          % (odd, 100.0 * odd / max(1, total), newstep / 2))
    print("  link coordinates that survive exactly: %d of %d = %.1f%%"
          % (linktotal - linkodd, linktotal, 100.0 * (linktotal - linkodd) / max(1, linktotal)))
    print("  worst error on any single axis: %.4g units" % worst)
    print("  worst possible 3D displacement: %.4g units" % (worst * math.sqrt(3)))
    print("  mean absolute error per axis: %.4g units" % (worst * odd / max(1, total)))
    print()
    print("  distance from a node to its nearest neighbour (%d sampled):" % len(nearest))
    for q in (0, 1, 5, 25, 50, 75, 95, 100):
        print("     %3d%%  %8.3f units" % (q, pct(q)))
    if nearest:
        print()
        print("  the worst error is %.3f%% of the CLOSEST pair of nodes in the whole game"
              % (100.0 * worst * math.sqrt(3) / pct(0) if pct(0) else 0.0))
        print("  and %.3f%% of a typical node spacing" % (100.0 * worst * math.sqrt(3) / pct(50)))


main()
