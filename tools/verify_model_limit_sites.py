#!/usr/bin/env python3
"""Re-read every site in tools/model_limit_sites.txt out of gta_sa.exe and confirm the operand
still holds the value the table expects. The plugin makes the same check at load time and refuses
to patch anything if a single site disagrees, so this must pass 100% before shipping the table.

Usage: verify_model_limit_sites.py <gta_sa.exe> [inventory]
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

VANILLA = {"info": 0x8E4CC0, "ptr": 0xA9B0C8, "atomic": 0xAAE950}


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    path = sys.argv[2] if len(sys.argv) > 2 else "tools/model_limit_sites.txt"
    ok = bad = 0
    counts = {}
    written = {}
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        f = [p.strip() for p in line.split("|")]
        addr, kind = int(f[0], 16), f[1]
        off, size, want = int(f[2].lstrip("+")), int(f[3]), int(f[4], 16)
        raw = pe.read(addr + off, size)
        got = struct.unpack("<I" if size == 4 else "<H", raw)[0] if raw else None
        counts[kind] = counts.get(kind, 0) + 1
        if got == want:
            ok += 1
        else:
            bad += 1
            print("MISMATCH 0x%X (+%d, %s): reads 0x%X, table says 0x%X" %
                  (addr, off, kind, got if got is not None else 0, want))
        base = VANILLA.get(kind)
        if base is not None and not base <= want < base + 0x100000:
            print("SUSPECT  0x%X (%s): 0x%X is outside the vanilla array" % (addr, kind, want))
        # Two table rows that patch overlapping bytes would fight over the same operand.
        for byte in range(addr + off, addr + off + size):
            if byte in written and written[byte] != (addr, off):
                bad += 1
                print("OVERLAP  0x%X (+%d) and 0x%X (+%d) share byte 0x%X"
                      % (addr, off, written[byte][0], written[byte][1], byte))
            written[byte] = (addr, off)
    for k, v in sorted(counts.items()):
        print("  %-12s %d" % (k, v))
    print("%d/%d sites verified" % (ok, ok + bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
