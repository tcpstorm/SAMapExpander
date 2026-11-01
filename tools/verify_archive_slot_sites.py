#!/usr/bin/env python3
"""Check src/limits/ArchiveSlots.cpp against gta_sa.exe from both directions at once.

The plugin makes the first of these checks itself at load time and refuses to write a single byte
if one site disagrees, so this must pass 100% before the table ships. The table is parsed out of
the source rather than copied here, so the two can never drift apart.

What is checked:

  VALUE     every operand still holds the address the table says it does - `base + field` for a
            base site, `base + 8 * 0x30 + field` for an end site.
  OFFSET    that dword really is at `operandOffset` inside the instruction, and NOWHERE ELSE in
            it; a wrong offset would silently patch four bytes of something else.
  MISSING   every instruction the scan finds naming the table is in the table. Missing one leaves
            a loop walking from the new block to the old block's end - a sweep over whatever the
            heap put in between.
  EXTRA     nothing is in the table that the scan does not recognise as a reference.
  FORBIDDEN none of the addresses that name a NEIGHBOURING GLOBAL is in either table. 0x8E4A58 is
            one past the last entry and also a byte CStreaming reads and writes; 0x8E4A60 is
            ms_channels, whose `add edi, 0x8E4A60` is an immediate in the same window an end bound
            falls in. Moving either with the array would corrupt game state.

Usage: verify_archive_slot_sites.py <gta_sa.exe> [src/limits/ArchiveSlots.cpp]
"""
import os
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402
from scan_archive_slot_sites import ARRAY, END, STRIDE, VANILLA_COUNT, classify, anchored  # noqa: E402

ROW = re.compile(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}")


def table(text, name):
    start = text.index(name + "[] = {")
    end = text.index("};", start)
    return [(int(a, 16), int(o), int(f, 16)) for a, o, f in ROW.findall(text[start:end])]


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    src_path = sys.argv[2] if len(sys.argv) > 2 else "src/limits/ArchiveSlots.cpp"
    src = open(src_path).read()

    base_sites = table(src, "kBaseSites")
    end_sites = table(src, "kEndSites")
    expected = {}
    for addr, off, field in base_sites:
        expected[addr] = (off, ARRAY + field, "base")
    for addr, off, field in end_sites:
        if addr in expected:
            print("FAIL 0x%X is in both tables" % addr)
            return 1
        expected[addr] = (off, END + field, "end")
    print("read %d base site(s) and %d end site(s) from %s"
          % (len(base_sites), len(end_sites), os.path.basename(src_path)))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True

    bad = 0

    # ---- VALUE and OFFSET
    for addr, (off, want, kind) in sorted(expected.items()):
        raw = pe.read(addr, 24)
        if raw is None:
            print("FAIL 0x%X is not in any mapped section" % addr)
            bad += 1
            continue
        got = struct.unpack_from("<I", raw, off)[0]
        if got != want:
            print("FAIL 0x%X+%d holds 0x%08X, the table says 0x%08X" % (addr, off, got, want))
            bad += 1
            continue
        ins = next(md.disasm(raw, addr), None)
        if ins is None or ins.id == 0:
            print("FAIL 0x%X does not decode" % addr)
            bad += 1
            continue
        if off + 4 > ins.size:
            print("FAIL 0x%X+%d is past the end of a %d-byte instruction" % (addr, off, ins.size))
            bad += 1
            continue
        elsewhere = [i for i in range(ins.size - 3)
                     if i != off and struct.unpack_from("<I", raw, i)[0] == want]
        if elsewhere:
            print("FAIL 0x%X: 0x%08X also appears at offset %s inside `%s %s`"
                  % (addr, want, elsewhere, ins.mnemonic, ins.op_str))
            bad += 1

    # ---- MISSING, EXTRA and FORBIDDEN: run the scan again and compare set against set
    live = []
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if (name == ".text" and not live) or name == ".HOODLUM":
            live.append((pe.image_base + va, min(vsize, rawsize) or rawsize, name))

    found, forbidden = {}, {}
    for sva, ssize, sname in live:
        code = pe.read(sva, ssize)
        for ins in md.disasm(code, sva):
            if ins.id == 0:
                continue
            for kind, off, old, note in classify(ins):
                (found if kind in ("base", "end") else forbidden)[ins.address] = (off, old, kind, note)
        if sname != ".HOODLUM":
            continue
        for at in range(len(code) - 4):
            v = struct.unpack_from("<I", code, at)[0]
            if not (ARRAY <= v < END + STRIDE):
                continue
            ins, hit = anchored(md, code, at, sva)
            if ins is None or ins.address in found or ins.address in forbidden:
                continue
            kind, off, old, note = hit
            (found if kind in ("base", "end") else forbidden)[ins.address] = (off, old, kind, note)

    for addr, (off, old, kind, note) in sorted(found.items()):
        if addr not in expected:
            print("FAIL MISSING 0x%X (%s, %s) names the table and is not in it" % (addr, kind, note))
            bad += 1
        elif expected[addr][2] != kind:
            print("FAIL 0x%X is a %s site in the table and a %s site in the executable"
                  % (addr, expected[addr][2], kind))
            bad += 1
    for addr in sorted(expected):
        if addr not in found:
            print("FAIL EXTRA 0x%X is in the table and the scan does not recognise it" % addr)
            bad += 1
    for addr, (off, old, kind, note) in sorted(forbidden.items()):
        if addr in expected:
            print("FAIL FORBIDDEN 0x%X (%s) is a neighbouring global and is in the table"
                  % (addr, note))
            bad += 1

    # ---- no stored pointer into the table anywhere in the data sections
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if name not in (".rdata", ".data"):
            continue
        at_base, size = pe.image_base + va, min(vsize, rawsize)
        blob = pe.read(at_base, size)
        for at in range(len(blob) - 4):
            v = struct.unpack_from("<I", blob, at)[0]
            if ARRAY <= v < END + STRIDE:
                print("FAIL 0x%X in %s stores 0x%08X, a pointer into the table"
                      % (at_base + at, name, v))
                bad += 1

    print("%d instruction(s) name CStreaming::ms_files; %d neighbouring-global reference(s) left "
          "alone; array 0x%X, %d x 0x%X, ending 0x%X"
          % (len(found), len(forbidden), ARRAY, VANILLA_COUNT, STRIDE, END))
    print("FAILED: %d problem(s)" % bad if bad else "OK: every site verified")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
