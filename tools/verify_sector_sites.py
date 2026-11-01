#!/usr/bin/env python3
"""Check src/limits/SectorGridSites.h against gta_sa.exe from both directions at once, so that a
regenerated table cannot quietly gain a site that is not a sector conversion or lose one that is.

The scan that writes the table recognises a conversion by a multiply of the sector scale followed
within a few instructions by an `fadd` of the sector centre. That is a good rule, and this checks
the two ways it can be wrong:

  MISSED - a conversion the pattern cannot see: a DISTANCE converted to a count of sectors. It
      multiplies by the scale and has no centre to add, because a length has no origin, so the
      pattern never fires. There is exactly one that matters, `CStreaming::AddModelsToRequestList`
      at 0x40D444, and src/limits/StreamRange.cpp handles it - see docs/streaming-distance.md for
      why it cannot simply be added to the table.
  FALSE - a site in the table that is not a conversion at all: an unrelated 0.02 that happens to sit
      within ten instructions of an unrelated 60.0. Both constants are pooled and shared with
      hundreds of unrelated uses, so this is a real risk. The one to keep in mind is
      CEntityScanner::ScanForEntitiesInRange, which clamps a count to 15.0 - the same pooled
      constant the coarse grid uses as its centre - as the last index of a 16-entry array.

      A genuine conversion is recognised by what its result is then USED for, and there are exactly
      three uses in this executable. Every one of the repointed scale sites ends in one of them, and
      each is checked by following the value forward from the multiply:

        index     the result reaches the float-to-int helper (_ftol at 0x821B40, sometimes through
                  floor at 0x8219F0) within a dozen instructions. 258 sites. This is the ordinary
                  `sector = (int)(coord * SCALE + CENTRE)`.
        polygon   the result is stored to a stack slot and the block ends in a call to
                  CRenderer::ScanSectorPoly at 0x72CAE0, which takes a polygon whose vertices are
                  already in grid space, floors them itself (it holds 68 floor and _ftol calls) and
                  runs a per-sector callback over every cell the polygon covers. 32 sites, all of
                  them the renderer converting a frustum: ScanWorld converts a 5-gon for the fine
                  grid and another for the coarse one, GetObjectsInFrustum and
                  RequestObjectsInFrustum a triangle each. The result is an integer index, just not
                  in this function, which is why the `index` rule alone called all 32 false.
        bound     the result is compared against a float that could be a sector index (0..256)
                  without ever being floored. 8 sites, all in Camera::Process_Editor, whose four
                  `while` loops step a coordinate by 1.0 while its index is outside 5..115 - the
                  bounds are floats and the comparison is exact, so nothing is converted. Four of
                  the eight are the loop entry tests and four the loop bodies.

      The three together are still a real test: they demand the value be used as a grid coordinate.
      A pooled 0.02 that lands near a pooled 60.0 by accident is dropped on the floor, passed to
      something else, or stored in a field, and none of those pass. Adding the three coincidental
      uses this executable actually contains - 0x546B22 and 0x546B33 in Entity::ProcessShiftSectorList
      and 0x69992C in CCover::Update - to the table as scale sites is the negative control, and all
      three are still reported. Run it if you widen any of the three rules.

      Only the scale half of each conversion is followed; the scan never emits a centre without the
      scale beside it, so a bad centre can only get in by hand and the table is generated.

Usage: verify_sector_sites.py <gta_sa.exe> [header]
"""
import bisect
import os
import re
import struct
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
SCAN_SECTOR_POLY = 0x72CAE0   # CRenderer::ScanSectorPoly(CVector2D const*, int32, callback)
LOOKAHEAD = 12                # to the float-to-int helper, or to the compare against a bound
POLY_LOOKAHEAD = 64           # to the ScanSectorPoly the whole block of vertices is built for
MAX_INDEX = 256.0             # the largest float a sector-index bound could plausibly be

# Un-repointed references that have been read and are meant to stay that way; see the table in
# docs/streaming-distance.md. Anything outside this set is new and wants looking at.
KNOWN_LEFT_ALONE = {
    0x40D446,   # the radius, a distance - StreamRange.cpp owns it
    0x50F4A9,   # Camera::Process_Editor turning a mouse delta into an angle: `fild; fmul [pi/180];
                # fmul 0.02; fadd [esi+0xAC]; fstp [esi+0xAC]`. 0.02 is the editor camera's
                # sensitivity and the addend is a camera field, not the sector centre.
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

    def float_at(addr):
        raw = pe.read(addr, 4)
        return struct.unpack("<f", raw)[0] if raw and len(raw) == 4 else None

    def index_bound(ins):
        """An x87 compare of the value against a float that could be a sector index."""
        if not ins.mnemonic.startswith(("fcom", "fucom")):
            return False
        d = disp_of(ins)
        if d is None:
            return False
        v = float_at(d)
        return v is not None and 0.0 <= v <= MAX_INDEX

    def call_to(ins, *targets):
        if ins.mnemonic != "call":
            return False
        try:
            return int(ins.op_str, 16) in targets
        except ValueError:
            return False

    scale_kinds = ("kFineScale", "kCoarseScale")
    refs = []            # (address, operand, constant)
    array_fns = set()
    false_sites = []     # repointed scales whose result is never used as a grid coordinate
    endings = {"index": 0, "polygon": 0, "bound": 0}
    checked = 0

    for sva, ssize, _n in live_sections(pe):
        pending = []     # [near, far, address, operand, kind]

        def tick():
            keep = []
            for p in pending:
                p[0] -= 1
                p[1] -= 1
                if p[1] > 0:
                    keep.append(p)
                else:
                    false_sites.append(tuple(p[2:]))
            return keep

        def settle(how, still_open):
            """Accept every pending site whose budget for `how` has not run out."""
            keep = []
            for p in pending:
                if still_open(p):
                    endings[how] += 1
                else:
                    keep.append(p)
            return keep

        for ins in md.disasm(pe.read(sva, ssize), sva):
            if ins.id == 0:
                pending = tick()
                continue
            if call_to(ins, FTOL, FLOOR):
                pending = settle("index", lambda p: p[0] > 0)
            elif index_bound(ins):
                pending = settle("bound", lambda p: p[0] > 0)
            elif call_to(ins, SCAN_SECTOR_POLY):
                pending = settle("polygon", lambda p: p[1] > 0)
            pending = tick()

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
                    pending.append([LOOKAHEAD, POLY_LOOKAHEAD,
                                    ins.address, operand, table[operand]])
        for p in pending:
            false_sites.append(tuple(p[2:]))

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
    print("   used as an index     %d   the result reaches _ftol/floor" % endings["index"])
    print("   used as a polygon    %d   the result is a vertex handed to ScanSectorPoly"
          % endings["polygon"])
    print("   used as a bound      %d   the result is compared against a sector index"
          % endings["bound"])
    print()
    print("FALSE  repointed but the result is never used as a grid coordinate: %d"
          % len(false_sites))
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
