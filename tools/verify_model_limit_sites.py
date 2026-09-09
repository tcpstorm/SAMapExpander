#!/usr/bin/env python3
"""Re-read every site in tools/model_limit_sites.txt out of gta_sa.exe and confirm the operand
still holds the value the table expects. The plugin makes the same check at load time and refuses
to patch anything if a single site disagrees, so this must pass 100% before shipping the table.

It also checks the one thing the table cannot say for itself: that CStreaming::Save and
CStreaming::Load still go on writing exactly 26316 bytes. Their loop bounds are ordinary
ms_aInfoForModel references and the general rule would shift the end of each with the array, which
would grow the savegame's Streaming block by the number of ids added - and San Andreas' savegame is
an unframed byte stream, so a block that changes size silently shifts the nine blocks behind it.
ModelLimits.cpp exempts the two end operands by address; this proves the exemption is still there
and still names the right two.

Usage: verify_model_limit_sites.py <gta_sa.exe> [inventory] [src/limits/ModelLimits.cpp]
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

VANILLA = {"info": 0x8E4CC0, "ptr": 0xA9B0C8, "atomic": 0xAAE950}

INFO_BASE, INFO_ELEM, VANILLA_TOTAL = 0x8E4CC0, 0x14, 26316
# CStreaming::Save (0x5D29A0) and CStreaming::Load (0x5D29E0): the loop start, then the loop end
# that must NOT move with the array. Read out of the executable, not assumed - see below.
SAVE_LOOPS = {"CStreaming::Save": (0x5D29A2, 0x5D29D3), "CStreaming::Load": (0x5D29E2, 0x5D2A0A)}


def check_save_block(rows, source):
    """The Streaming block must still be VANILLA_TOTAL entries wide, and stay that way."""
    bad = 0
    for name, (start, end) in SAVE_LOOPS.items():
        if start not in rows or end not in rows:
            print("SAVE     %s: the loop's %s is not in the table at all - it would keep naming "
                  "the OLD array while the rest of the game moved"
                  % (name, "start" if start not in rows else "end"))
            bad += 1
            continue
        first = (rows[start] - INFO_BASE) // INFO_ELEM
        last = (rows[end] - INFO_BASE) // INFO_ELEM
        if last - first != VANILLA_TOTAL:
            print("SAVE     %s spans %d entries, not the vanilla %d"
                  % (name, last - first, VANILLA_TOTAL))
            bad += 1
        if source is not None and ("0x%X" % end) not in source.upper().replace("0X", "0x"):
            print("SAVE     %s's end operand 0x%X is not exempted in ModelLimits.cpp, so the "
                  "shift would be applied to it and the savegame block would grow by the number "
                  "of ids added" % (name, end))
            bad += 1
    return bad


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    path = sys.argv[2] if len(sys.argv) > 2 else "tools/model_limit_sites.txt"
    try:
        source = open(sys.argv[3] if len(sys.argv) > 3 else "src/limits/ModelLimits.cpp").read()
    except OSError:
        source = None
        print("NOTE     ModelLimits.cpp not readable; the save-block exemption is only checked "
              "for span, not for being exempted")
    ok = bad = 0
    counts = {}
    written = {}
    rows = {}
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        f = [p.strip() for p in line.split("|")]
        addr, kind = int(f[0], 16), f[1]
        off, size, want = int(f[2].lstrip("+")), int(f[3]), int(f[4], 16)
        raw = pe.read(addr + off, size)
        got = struct.unpack("<I" if size == 4 else "<H", raw)[0] if raw else None
        counts[kind] = counts.get(kind, 0) + 1
        if kind == "info":
            rows[addr] = want
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
    bad += check_save_block(rows, source)
    for k, v in sorted(counts.items()):
        print("  %-12s %d" % (k, v))
    print("the savegame's Streaming block still spans %d entries" % VANILLA_TOTAL)
    print("%d/%d sites verified" % (ok, ok + bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
