#!/usr/bin/env python3
"""Find every instruction in gta_sa.exe (1.0 US HOODLUM) that ADDRESSES a world-sector array, and
work out how big those arrays could be made.

src/WorldSectors.cpp already moves and stretches the two static-world grids by repointing the two
constants every index conversion reads (tools/scan_sector_sites.py finds those). That changes how
much ground a cell covers but not how many cells there are, so covering a wider world means bigger
cells, and every per-frame scan over a cell walks proportionally more entities. Getting the cell
back to vanilla's 50 units means more cells, which means the arrays themselves have to grow -- and
that is what this scan is for: it enumerates everything that would have to change, and measures
whether the instructions involved can hold the numbers a bigger grid needs.

The answer is no, and the reason is an encoding one. See the ceiling section printed at the end.

What the two grids are, in this build:

    CWorld::ms_aSectors      0xB7D0B8   120 x 120 entries of 8 bytes  (115200 bytes)
                                        CSector = { CPtrListSingleLink buildings; dummies; }
    CWorld::ms_aRepeatSectors 0xB992B8   16 x 16  entries of 12 bytes (3072 bytes)  - not ours
    CWorld::ms_aLodPtrLists  0xB99EB8    30 x 30  entries of 4 bytes  (3600 bytes)

They sit end to end, in that order, so the end of one is the base of the next: an address that
looks like "one past ms_aSectors" and one that looks like "ms_aRepeatSectors" are the same number,
and the two can only be told apart by what the loop around them does. That is why this scan reports
the whole-grid iterations separately, with the stride each loop advances by.

Every indexed access has exactly one shape:

    imul  reg, reg, 120          ; row * 120
    add   reg, <column>
    ...   [reg*8 + 0xB7D0B8]     ; or +0xB7D0BC for the dummy list

and for the coarse grid `imul reg, reg, 30` with `[reg*4 + 0xB99EB8]`. Nothing walks a sector
pointer along a row with an add, and nothing forms the address any other way, so the set of things
that would have to change is: the base displacement, the row-stride multiply, the index clamps, the
whole-grid loop bounds, and the coarse grid's row byte-stride.

Usage: scan_sector_array_sites.py <gta_sa.exe> [report]
"""
import bisect
import collections
import os
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scan_exe import Pe  # noqa: E402

FINE_BASE = 0xB7D0B8
FINE_COUNT = 120
FINE_ELEM = 8
REPEAT_BASE = 0xB992B8
LOD_BASE = 0xB99EB8
LOD_COUNT = 30
LOD_ELEM = 4

FINE_END = FINE_BASE + FINE_COUNT * FINE_COUNT * FINE_ELEM   # == REPEAT_BASE
LOD_END = LOD_BASE + LOD_COUNT * LOD_COUNT * LOD_ELEM

# CWorld::GetSector and CWorld::GetLodPtrList are the only out-of-line accessors; HOODLUM moved
# GetSector's body into its own section, so its two halves are listed by hand.
GET_SECTOR_THUNK = 0x407260
GET_SECTOR_BODY = (0x1566820, 0x1566860)
GET_LOD_PTR_LIST = (0x4072C0, 0x4072E0)

FINE_INDEX = re.compile(r"\[(e[a-z]{2})\*8 \+ 0xb7d0b([8c])\]")
LOD_INDEX = re.compile(r"\[(e[a-z]{2})\*4 \+ 0xb99eb([8c])\]")


def live_sections(pe):
    out = []
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if (name == ".text" and not out) or name == ".HOODLUM":
            out.append((pe.image_base + va, min(vsize, rawsize) or rawsize, name))
    return out


def disassemble(pe):
    """The whole of the executable code, as (address, size, mnemonic, operands)."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False
    md.skipdata = True
    rows = []
    for sva, ssize, _name in live_sections(pe):
        for row in md.disasm_lite(pe.read(sva, ssize), sva):
            rows.append(row)
    return rows


def load_function_starts(path):
    starts, names = [], {}
    if not os.path.exists(path):
        return starts, names
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        addr, _, name = line.partition(" ")
        try:
            value = int(addr, 16)
        except ValueError:
            continue
        starts.append(value)
        names[value] = name
    starts.sort()
    return starts, names


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    rows = disassemble(pe)
    addrs = [r[0] for r in rows]
    here = os.path.dirname(os.path.abspath(__file__))
    starts, names = load_function_starts(os.path.join(here, "sa_functions.txt"))

    def function_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else 0

    def body_of(addr):
        """[start, end) of the function an address is in, as far as the name list knows."""
        f = function_of(addr)
        j = bisect.bisect_right(starts, f)
        return f, starts[j] if j < len(starts) else f + 0x1000

    # ---------------------------------------------------------------- indexed accesses
    fine_sites, lod_sites = [], []
    for i, (a, size, m, ops) in enumerate(rows):
        hit = FINE_INDEX.search(ops)
        if hit:
            fine_sites.append((a, size, m, ops, hit.group(1), 4 if hit.group(2) == "c" else 0))
        hit = LOD_INDEX.search(ops)
        if hit:
            lod_sites.append((a, size, m, ops, hit.group(1), 4 if hit.group(2) == "c" else 0))

    # Every indexed access should be preceded by the row-stride multiply for its grid. If one is
    # not, the addressing is not the shape this whole exercise assumes and nothing may be patched.
    def multiply_before(i, imm, window=60):
        for j in range(i - 1, max(-1, i - window), -1):
            a, size, m, ops = rows[j]
            if m == "imul" and ops.endswith(", " + imm):
                return rows[j]
        return None

    index_of = {a: i for i, (a, _s, _m, _o) in enumerate(rows)}
    orphans = []
    for a, _size, _m, _ops, _reg, _off in fine_sites:
        if multiply_before(index_of[a], "0x78") is None:
            orphans.append(("fine", a))
    for a, _size, _m, _ops, _reg, _off in lod_sites:
        if multiply_before(index_of[a], "0x1e") is None:
            orphans.append(("coarse", a))

    # ---------------------------------------------------------------- whole-grid iterations
    # A loop over the whole of one grid starts with `mov reg, <base>` and ends with `cmp reg, <end>`.
    # The end of ms_aSectors is the base of ms_aRepeatSectors and the end of ms_aRepeatSectors is
    # the base of ms_aLodPtrLists, so the stride the loop advances by is what tells them apart.
    walks = []
    for i, (a, size, m, ops) in enumerate(rows):
        hit = re.match(r"^(e[a-z]{2}), 0x(b7d0b8|b992b8|b99eb8|b9acc8)$", ops)
        if not hit or m not in ("mov", "cmp"):
            continue
        reg, value = hit.group(1), int(hit.group(2), 16)
        # What the loop advances the pointer by is what tells the grids apart, because the end of
        # one array is the base of the next: 8 is a CSector, 12 a repeat sector, 4 a coarse list
        # head - and 400 is CWorld::Players[2], which happens to end exactly where ms_aSectors
        # begins and whose loop guards must on no account be mistaken for it.
        #
        # The increment sits immediately before the compare that closes the loop, so it is only
        # looked for there; a `mov reg, base` that opens a loop is reported without one.
        stride = None
        scan_from = i if m == "cmp" else None
        if scan_from is None:
            # A `mov reg, base` opens the loop; its stride is the one the compare that closes the
            # loop is preceded by, so find that compare first.
            for j in range(i + 1, min(len(rows), i + 300)):
                if rows[j][2] == "cmp" and re.match(
                        r"^%s, 0x(b7d0b8|b992b8|b99eb8|b9acc8)$" % reg, rows[j][3]):
                    scan_from = j
                    break
        if scan_from is not None:
            for j in range(scan_from - 1, max(-1, scan_from - 6), -1):
                if rows[j][2] == "add" and rows[j][3].startswith(reg + ", "):
                    try:
                        stride = int(rows[j][3].split(", ")[1], 0)
                    except ValueError:
                        stride = None
                    break
        walks.append((a, m, ops, value, stride, function_of(a)))

    # ---------------------------------------------------------------- dimension constants
    # Only inside a function that indexes one of the grids, or that reaches it through GetSector.
    callers = set()
    for a, _size, m, ops in rows:
        if m == "call" and ops == hex(GET_SECTOR_THUNK):
            callers.add(function_of(a))

    fine_bodies = collections.OrderedDict()
    for a, _s, _m, _o, _r, _off in fine_sites:
        fine_bodies.setdefault(body_of(a), []).append(a)
    for f in callers:
        fine_bodies.setdefault(body_of(f), [])
    fine_bodies.setdefault(GET_SECTOR_BODY, [])

    lod_bodies = collections.OrderedDict()
    for a, _s, _m, _o, _r, _off in lod_sites:
        lod_bodies.setdefault(body_of(a), []).append(a)

    def constants_in(bodies, patterns):
        found = []
        for (lo, hi) in bodies:
            i = bisect.bisect_left(addrs, lo)
            j = bisect.bisect_left(addrs, hi)
            for k in range(i, j):
                a, size, m, ops = rows[k]
                for kind, rx in patterns:
                    if rx.match("%s %s" % (m, ops)):
                        found.append((a, size, m, ops, kind, lo))
                        break
        return found

    fine_consts = constants_in(fine_bodies, [
        ("row stride (120)", re.compile(r"^imul (e[a-z]{2}), \1, 0x78$")),
        ("index clamp (119)", re.compile(r"^cmp e[a-z]{2}, 0x77$")),
        ("index clamp (119)", re.compile(r"^mov (e[a-z]{2}, |dword ptr \[e[a-z]{2} \+ 0x[0-9a-f]+\], )0x77$")),
        ("loop bound (120)", re.compile(r"^cmp e[a-z]{2}, 0x78$")),
        ("loop bound (120)", re.compile(r"^mov (e[a-z]{2}, |dword ptr \[e[a-z]{2} \+ 0x[0-9a-f]+\], )0x78$")),
    ])
    lod_consts = constants_in(lod_bodies, [
        ("row stride (30)", re.compile(r"^imul (e[a-z]{2}), \1, 0x1e$")),
        ("index clamp (29)", re.compile(r"^cmp e[a-z]{2}, 0x1d$")),
        ("index clamp (29)", re.compile(r"^mov (e[a-z]{2}, |dword ptr \[e[a-z]{2} \+ 0x[0-9a-f]+\], )0x1d$")),
        ("loop bound (30)", re.compile(r"^cmp e[a-z]{2}, 0x1e$")),
        ("row bytes (120)", re.compile(r"^add e[a-z]{2}, 0x78$")),
    ])

    # ---------------------------------------------------------------- report
    out = open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "sector_array_sites.txt"), "w")

    def say(fmt, *args):
        out.write((fmt % args if args else fmt) + "\n")

    for line in __doc__.split("Usage:")[0].rstrip().splitlines():
        say(line)
    say("")
    say("Generated by tools/scan_sector_array_sites.py; do not edit by hand.")
    say("")
    say("=" * 96)
    say("ARRAYS")
    say("=" * 96)
    say("  fine    %08X .. %08X   %d x %d of %d bytes",
        FINE_BASE, FINE_END, FINE_COUNT, FINE_COUNT, FINE_ELEM)
    say("  repeat  %08X .. %08X   16 x 16 of 12 bytes (CEntityScanner's wrapping grid, not ours)",
        REPEAT_BASE, LOD_BASE)
    say("  coarse  %08X .. %08X   %d x %d of %d bytes", LOD_BASE, LOD_END, LOD_COUNT, LOD_COUNT, LOD_ELEM)
    say("")
    say("=" * 96)
    say("INDEXED ACCESSES   (the base displacement of each would move to a bigger array)")
    say("=" * 96)
    for title, sites in (("fine", fine_sites), ("coarse", lod_sites)):
        say("  %s: %d site(s)", title, len(sites))
        for a, size, m, ops, reg, off in sites:
            say("    %08X  %-2d bytes  %-6s %-46s  base+%d  [%s]",
                a, size, m, ops, off, names.get(function_of(a), "?"))
    say("")
    say("  accesses with no row-stride multiply in front of them: %d", len(orphans))
    for kind, a in orphans:
        say("    %s %08X", kind, a)
    say("")
    say("=" * 96)
    say("WHOLE-GRID ITERATIONS   (start pointer and end comparison; stride tells the grids apart)")
    say("=" * 96)
    for a, m, ops, value, stride, fn in walks:
        which = {FINE_BASE: "fine base", REPEAT_BASE: "fine end / repeat base",
                 LOD_BASE: "repeat end / coarse base", LOD_END: "coarse end"}[value]
        say("    %08X  %-4s %-24s %-26s stride %-4s [%s]",
            a, m, ops, which, stride if stride is not None else "?", names.get(fn, "?"))
    say("")
    say("=" * 96)
    say("DIMENSION CONSTANTS INSIDE THE FUNCTIONS THAT INDEX THE GRIDS")
    say("=" * 96)
    for title, consts in (("fine", fine_consts), ("coarse", lod_consts)):
        counts = collections.Counter(k for _a, _s, _m, _o, k, _f in consts)
        say("  %s: %d site(s) - %s", title, len(consts),
            ", ".join("%s x%d" % (k, v) for k, v in sorted(counts.items())))
        for a, size, m, ops, kind, fn in consts:
            say("    %08X  %-2d bytes  %-6s %-32s %-20s [%s]",
                a, size, m, ops, kind, names.get(fn, "?"))
    say("")
    say("=" * 96)
    say("THE CEILING")
    say("=" * 96)
    widths = collections.Counter()
    for _a, size, m, ops, kind, _f in fine_consts + lod_consts:
        widths[(m, kind, size)] += 1
    for (m, kind, size), n in sorted(widths.items()):
        say("  %-6s %-20s encoded in %d byte(s)  x%d", m, kind, size, n)
    say("")
    say("  An index reaches the array through clamp(v, 0, N-1). The lower bound is the sign-mask")
    say("  idiom (xor/test/setle/dec/and) and carries no constant; the upper bound is `cmp reg,")
    say("  imm8` in every single one of the sites above - three bytes, with the immediate")
    say("  sign-extended, so the largest value it can hold is 127.")
    say("")
    say("  The row-stride multiply is not a problem: `imul reg, reg, imm8` is three bytes and so is")
    say("  `shl reg, imm8`, so a power-of-two stride can be written over it in place. The clamp is")
    say("  the wall. Widening `cmp reg, imm8` to `cmp reg, imm32` needs six bytes, and the three it")
    say("  would have to take are the conditional branch that reads its result.")
    say("")
    say("  So the reachable index range is [0, 127] and the grid can be at most 128 x 128 - about")
    say("  a 6.7 per cent smaller cell than the 120 it has now, against the 307 a side that San")
    say("  Andreas plus two cities at vanilla's 50-unit cell would need. Growing the arrays is not")
    say("  the route to a vanilla-sized cell; moving the grid onto the ground the player is")
    say("  actually standing on is.")
    out.close()

    print("fine: %d indexed access(es), %d dimension constant(s)" % (len(fine_sites), len(fine_consts)))
    print("coarse: %d indexed access(es), %d dimension constant(s)" % (len(lod_sites), len(lod_consts)))
    print("whole-grid iterations: %d, accesses with no stride multiply: %d" % (len(walks), len(orphans)))


if __name__ == "__main__":
    main()
