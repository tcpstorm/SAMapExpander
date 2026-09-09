#!/usr/bin/env python3
"""Field census of San Andreas' shipped CPathNode (28 bytes) and CCarPathLink (14 bytes) records.

Two questions this answers. First, whether the int16 words being rescaled carry anything but
position - if a flag or a lane count shared those bytes, rescaling would be a behaviour change and
not a rounding one. Second, what the other fields look like, which is what a converter has to fill.

Every byte offset is histogrammed across all 64 shipped areas. A field that is a position looks
like a wide spread of signed values; a flag looks like two or three values; an index looks like a
dense run from 0 upwards.
"""
import glob
import os
import struct
import sys
from collections import Counter

NODE_SIZE = 0x1C
LINK_SIZE = 0x0E


def census(records, size, label, wordfields):
    print("=== %s, %d bytes, %d records ===" % (label, size, len(records)))
    for off in range(size):
        vals = Counter(r[off] for r in records)
        distinct = len(vals)
        common = ", ".join("%d x%d" % (v, n) for v, n in vals.most_common(4))
        print("  +0x%02X  %4d distinct  %s%s"
              % (off, distinct, common, "" if distinct > 8 else "   <- few values, a flag or count"))
    print()
    for name, off in wordfields:
        vals = [struct.unpack_from("<h", r, off)[0] for r in records]
        even = sum(1 for v in vals if v % 2 == 0)
        print("  %-6s at +0x%02X: signed %d..%d, %d distinct, %.1f%% even"
              % (name, off, min(vals), max(vals), len(set(vals)), 100.0 * even / len(vals)))
    print()


def main():
    root = sys.argv[1]
    nodes, links = [], []
    for path in sorted(glob.glob(os.path.join(root, "*.[Dd][Aa][Tt]"))):
        if not os.path.basename(path).lower().startswith("nodes"):
            continue
        blob = open(path, "rb").read()
        numNodes, numVeh, numPed, numLinks14, numLinks = struct.unpack_from("<5i", blob, 0)
        at = 20
        if numNodes < 0 or at + numNodes * NODE_SIZE > len(blob):
            continue
        if numNodes != numVeh + numPed:
            print("NOTE %s: numNodes %d != vehicle %d + ped %d"
                  % (os.path.basename(path), numNodes, numVeh, numPed))
        for i in range(numNodes):
            nodes.append(blob[at + i * NODE_SIZE:at + (i + 1) * NODE_SIZE])
        at += numNodes * NODE_SIZE
        if numLinks14 > 0 and at + numLinks14 * LINK_SIZE <= len(blob):
            for i in range(numLinks14):
                links.append(blob[at + i * LINK_SIZE:at + (i + 1) * LINK_SIZE])

    census(nodes, NODE_SIZE, "CPathNode", [("x", 8), ("y", 0x0A), ("z", 0x0C)])
    census(links, LINK_SIZE, "CCarPathLink", [("x", 0), ("y", 2)])


main()
