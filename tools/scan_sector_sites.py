#!/usr/bin/env python3
"""Find every instruction in gta_sa.exe (1.0 US HOODLUM) that turns a world coordinate into a
world-sector index, and write the table src/SectorGridSites.h.

San Andreas' static world is a fixed 120 x 120 grid of 50-unit sectors and a 30 x 30 grid of
200-unit sectors for big buildings, both covering +/-3000. Every index is computed inline as

    index = (int)(coord * SCALE + CENTRE)

with the two constants pooled in .rdata: 0.02 / 60 for the fine grid, 0.005 / 15 for the coarse one.
Nothing reads the sector size any other way -- x87 cannot take a float immediate, so a conversion
has to load those globals -- which makes the set findable exhaustively by pattern rather than by
hand. Halving a scale doubles what its grid covers without changing the array: 120 x 50 becomes
120 x 100, so the world reaches +/-6000. The centre is untouched, because index 60 is still the
middle of a 120-entry grid.

The pooled constants cannot simply be edited: the compiler shares one 0.02 and one 60.0 with every
unrelated use in the game (516 and 373 references). So each conversion's operand is repointed at a
private copy instead, the way ModelLimits repoints the streaming arrays.

A conversion is written one of two ways, and both count:

    fmul dword ptr [scale]; fadd dword ptr [centre]        the common one
    fld dword ptr [scale]; fmul st(N); fadd [centre]       when the scale is loaded once and the
                                                           value being converted is already on the
                                                           stack, which is what a loop body does

The second shape exists in exactly one function, Camera::Process_Editor, whose four `while` loops
step a coordinate by 1.0 until its sector index leaves 5..115. Each loop's entry test is written the
first way and its body the second, so a scan that knows only the first shape repoints the test and
leaves the body correcting against vanilla's grid - which is what this did until the two disagreed.

Site kinds:
    fine        scale operand of a 50-unit conversion    -> our fine scale
    coarse      scale operand of a 200-unit conversion   -> our coarse scale
    finecentre  fadd operand of the same conversion     -> our fine centre
    coarsecentre  fadd operand of the same conversion   -> our coarse centre
    low/high    the +/-3000 bound a coordinate is clamped to before conversion, as a disp32
    lowimm      the -3000.0f a clamp stores as an immediate
    highimm     the 2999.5f a clamp stores as an immediate

Only CEntity::Add and CEntity::Remove clamp coordinates before converting them, so the bound sites
are taken from those two functions alone -- +/-3000 is an ordinary number everywhere else (HUD
layout, weather, gang wars) and must stay put there.

Usage: scan_sector_sites.py <gta_sa.exe> [header]
"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

FINE_SCALE, FINE_CENTRE = 0x858B38, 0x858B34      # 0.02, 60
COARSE_SCALE, COARSE_CENTRE = 0x858B4C, 0x858B48  # 0.005, 15
LOW, HIGH = 0x859A90, 0x859A94                    # -3000.0, +3000.0
LOW_IMM, HIGH_IMM = 0xC53B8000, 0x453B7000        # -3000.0f, 2999.5f
# CEntity::Add and CEntity::Remove, the only two that clamp a coordinate into the grid.
CLAMPERS = ((0x5347D0, 0x534AE0), (0x534AE0, 0x534E90))

PAIR_WINDOW = 10   # instructions between the fmul and its fadd


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


def scan(pe):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True
    rows = []

    for sva, ssize, _name in live_sections(pe):
        code = pe.read(sva, ssize)
        window = []
        for ins in md.disasm(code, sva):
            window.append(ins)
            if len(window) > PAIR_WINDOW:
                window.pop(0)
            if ins.id == 0:
                continue

            disp = disp_of(ins)
            if disp in (FINE_CENTRE, COARSE_CENTRE) and ins.mnemonic.startswith("fadd"):
                scale = FINE_SCALE if disp == FINE_CENTRE else COARSE_SCALE
                kind = "fine" if disp == FINE_CENTRE else "coarse"
                for i in range(len(window) - 2, -1, -1):
                    prev = window[i]
                    operand = None
                    if prev.mnemonic.startswith("fmul"):
                        if disp_of(prev) == scale:
                            operand = prev.address + prev.encoding.disp_offset
                        elif prev.op_str.startswith("st(") and i > 0 and \
                                window[i - 1].mnemonic == "fld" and \
                                disp_of(window[i - 1]) == scale:
                            # `fld [scale]; fmul st(N)` - the same conversion, other shape. The
                            # site to repoint is the fld's operand, not the register multiply's.
                            operand = window[i - 1].address + window[i - 1].encoding.disp_offset
                    if operand is not None:
                        rows.append((operand, kind, scale))
                        # The centre too. Repointing only the scale keeps the grid centred on 0, so
                        # it has to reach as far the wrong way as the right way and the cells come
                        # out twice as big as they need to be. With the centre ours as well the grid
                        # can be shifted onto the ground that is actually used, and a cell stays as
                        # small as the span allows - which is what every sector scan costs.
                        rows.append((ins.address + ins.encoding.disp_offset,
                                     kind + "centre", disp))
                        break

            if disp in (LOW, HIGH) and any(lo <= ins.address < hi for lo, hi in CLAMPERS):
                rows.append((ins.address + ins.encoding.disp_offset,
                             "low" if disp == LOW else "high", disp))

            # `mov dword ptr [esp + N], imm32` is how the clamp writes the value back.
            if ins.mnemonic == "mov" and ins.encoding.imm_size == 4 and \
                    any(lo <= ins.address < hi for lo, hi in CLAMPERS):
                imm = ins.operands[-1].imm & 0xFFFFFFFF
                if imm in (LOW_IMM, HIGH_IMM):
                    rows.append((ins.address + ins.encoding.imm_offset,
                                 "lowimm" if imm == LOW_IMM else "highimm", imm))

    # A conversion can be reached twice by the sweep only if the same bytes decode twice; the
    # operand address is the identity, so collapse duplicates.
    seen, unique = set(), []
    for row in sorted(rows):
        if row[0] not in seen:
            seen.add(row[0])
            unique.append(row)
    return unique


KINDS = {"fine": "kFineScale", "coarse": "kCoarseScale",
         "finecentre": "kFineCentre", "coarsecentre": "kCoarseCentre", "low": "kLowBound",
         "high": "kHighBound", "lowimm": "kLowImmediate", "highimm": "kHighImmediate"}


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    rows = scan(pe)
    counts = {}
    for _a, kind, _old in rows:
        counts[kind] = counts.get(kind, 0) + 1

    path = sys.argv[2] if len(sys.argv) > 2 else "src/SectorGridSites.h"
    with open(path, "w") as out:
        for line in __doc__.split("Usage:")[0].rstrip().splitlines():
            out.write(("// " + line).rstrip() + "\n")
        out.write("//\n// Generated by tools/scan_sector_sites.py; do not edit by hand.\n")
        out.write("#pragma once\n#include <cstdint>\n\nnamespace WorldSectors {\n\n")
        out.write("enum SiteKind : uint8_t {\n")
        for name in ("kFineScale", "kCoarseScale", "kLowBound", "kHighBound",
                     "kLowImmediate", "kHighImmediate", "kFineCentre",
                     "kCoarseCentre", "kSiteKindCount"):
            out.write("    %s,\n" % name)
        out.write("};\n\nstruct Site {\n    uint32_t operand;   // address of the 4 bytes to rewrite\n"
                  "    uint32_t oldValue;  // what it must still hold\n    uint8_t  kind;\n};\n\n")
        out.write("// %d sites: %s\n" % (len(rows), ", ".join(
            "%s=%d" % (k, counts[k]) for k in sorted(counts))))
        out.write("constexpr Site kSites[] = {\n")
        for addr, kind, old in rows:
            out.write("    { 0x%X, 0x%08X, %s },\n" % (addr, old, KINDS[kind]))
        out.write("};\n\n}  // namespace WorldSectors\n")

    print("%d sites -> %s" % (len(rows), path))
    for k in sorted(counts):
        print("  %-8s %d" % (k, counts[k]))


if __name__ == "__main__":
    main()
