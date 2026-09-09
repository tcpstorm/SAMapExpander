#!/usr/bin/env python3
"""Check src/limits/SectorGridSites.h against gta_sa.exe from both directions at once, so that a
regenerated table cannot quietly gain a site that is not a sector conversion or lose one that is.

The scan that writes the table recognises a conversion by an `fmul` of the sector scale followed
within a few instructions by an `fadd` of the sector centre. That is a good rule and it has two
known holes, both of which this checks for:

  MISSED - a conversion the pattern cannot see. Two kinds:
      * a DISTANCE converted to a count of sectors. It multiplies by the scale and has no centre to
        add, because a length has no origin, so the pattern never fires. There is exactly one that
        matters, `CStreaming::AddModelsToRequestList` at 0x40D444, and src/limits/StreamRange.cpp
        handles it - see docs/streaming-distance.md for why it cannot simply be added to the table.
      * a conversion written `fld [scale]; fmul st(N); fadd [centre]` rather than `fmul [scale];
        fadd [centre]`. Camera::Process_Editor has four of these and they are not in the table.
  FALSE - a site in the table that is not a conversion at all: an unrelated 0.02 that happens to sit
      within ten instructions of an unrelated 60.0. Both constants are pooled and shared with
      hundreds of unrelated uses, so this is a real risk. A genuine conversion always ends as an
      integer index, so its result must reach the float-to-int helper (_ftol at 0x821B40, sometimes
      through floor at 0x8219F0) a few instructions later. Anything that does not is a coincidence,
      and repointing it changes a number that is not a sector index. The one to keep in mind is
      CEntityScanner::ScanForEntitiesInRange, which clamps a count to 15.0 - the same pooled
      constant the coarse grid uses as its centre - as the last index of a 16-entry array.

Usage: verify_sector_sites.py <gta_sa.exe> [header]
"""
import bisect
import os
import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

FINE_SCALE, FINE_CENTRE = 0x858B38, 0x858B34
COARSE_SCALE, COARSE_CENTRE = 0x858B4C, 0x858B48
NAMES = {FINE_SCALE: "fineScale", FINE_CENTRE: "fineCentre",
         COARSE_SCALE: "coarseScale", COARSE_CENTRE: "coarseCentre"}
ARRAYS = (0xB7D0B8, 0xB99EB8, 0xB992B8)   # ms_aSectors, ms_aLodPtrLists, ms_aRepeatSectors
FTOL, FLOOR = 0x821B40, 0x8219F0
LOOKAHEAD = 12

# Un-repointed references that have been read and are meant to stay that way; see the table in
# docs/streaming-distance.md. Anything outside this set is new and wants looking at.
KNOWN_LEFT_ALONE = {
    0x40D446,   # the radius, a distance - StreamRange.cpp owns it
    0x50F748, 0x50F750, 0x50F788, 0x50F790,   # Camera::Process_Editor's four step loops,
    0x50F7D8, 0x50F7E0, 0x50F828, 0x50F830,   # written fld-then-fmul; editor camera only
    0x546B22, 0x546B33,                       # 0.005 as an epsilon and a floor on a shift
    0x5FFE01, 0x5FFE10,                       # 15.0 as the last index of a 16-entry sort
    0x69992C,                                 # 0.005 as a near-zero length test
}
# CPlayerInfo::Process accumulates eleven statistics at 2% a step; 0.02 there is a rate.
KNOWN_LEFT_ALONE |= {0x56F9DD, 0x56FA71, 0x56FABE, 0x56FBA0, 0x56FBED, 0x56FC5D,
                     0x56FE03, 0x56FE50, 0x56FF9A, 0x56FFF9, 0x570046}


def live_sections(pe):
    out = []
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if (name == ".text" and not out) or name == ".HOODLUM":
            out.append((pe.image_base + va, min(vsize, rawsize) or rawsize, name))
    return out


def disp_of(ins):
    if ins.encoding.disp_size != 4:
        return None
    for op in ins.operands:
        if op.type == X86_OP_MEM:
            return op.mem.disp & 0xFFFFFFFF
    return None


def load_functions():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sa_functions.txt")
    out = []
    for line in open(path):
        line = line.strip()
        if line:
            a, n = line.split(None, 1)
            out.append((int(a, 16), n))
    out.sort()
    return out


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    header = sys.argv[2] if len(sys.argv) > 2 else "src/limits/SectorGridSites.h"
    table = {}
    for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+),\s*(k\w+)\s*\}",
                         open(header).read()):
        table[int(m.group(1), 16)] = m.group(3)

    funcs = load_functions()
    faddrs = [f[0] for f in funcs]

    def fn_of(addr):
        i = bisect.bisect_right(faddrs, addr) - 1
        return funcs[i] if i >= 0 else (0, "?")

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True

    scale_kinds = ("kFineScale", "kCoarseScale")
    refs = []            # (address, operand, constant)
    array_fns = set()
    false_sites = []     # repointed scales whose result never becomes an integer
    checked = 0

    for sva, ssize, _n in live_sections(pe):
        pending = []     # [countdown, address, operand, kind]

        def expire(all_of_them=False):
            keep = []
            for p in pending:
                p[0] -= 1
                if p[0] > 0 and not all_of_them:
                    keep.append(p)
                else:
                    false_sites.append(tuple(p[1:]))
            return keep

        for ins in md.disasm(pe.read(sva, ssize), sva):
            if ins.id == 0:
                pending = expire()
                continue
            reaches_int = False
            if ins.mnemonic == "call":
                try:
                    reaches_int = int(ins.op_str, 16) in (FTOL, FLOOR)
                except ValueError:
                    pass
            pending = [] if reaches_int else expire()

            d = disp_of(ins)
            if d is None:
                continue
            if d in ARRAYS:
                array_fns.add(fn_of(ins.address)[0])
            if d in NAMES:
                operand = ins.address + ins.encoding.disp_offset
                refs.append((ins.address, operand, d))
                if table.get(operand) in scale_kinds:
                    checked += 1
                    pending.append([LOOKAHEAD, ins.address, operand, table[operand]])
        for p in pending:
            false_sites.append(tuple(p[1:]))

    # Every reference the table does not hold, inside a function that touches the grid at all.
    interesting = set(array_fns)
    for addr, operand, _d in refs:
        if operand in table:
            interesting.add(fn_of(addr)[0])
    missed = [r for r in refs
              if r[1] not in table and r[1] not in KNOWN_LEFT_ALONE
              and fn_of(r[0])[0] in interesting]

    print("table entries          %d" % len(table))
    print("constant references    %d" % len(refs))
    print("repointed scale sites  %d" % checked)
    print()
    print("FALSE  repointed but the result never becomes an integer index: %d" % len(false_sites))
    for addr, operand, kind in false_sites:
        f = fn_of(addr)
        print("   %08X op=%08X %-13s in %08X %s" % (addr, operand, kind, f[0], f[1]))
    print("MISSED new un-repointed reference inside a grid function: %d" % len(missed))
    for addr, operand, d in missed:
        f = fn_of(addr)
        print("   %08X op=%08X %-13s in %08X %s" % (addr, operand, NAMES[d], f[0], f[1]))

    ok = not false_sites and not missed
    print("\n%s" % ("OK" if ok else "SOMETHING TO LOOK AT"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
