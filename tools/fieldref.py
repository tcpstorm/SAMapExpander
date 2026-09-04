#!/usr/bin/env python3
"""List instructions in an address range that access [reg + disp] for the given displacements.

Usage: fieldref.py <gta_sa.exe> <start hex> <end hex> <disp hex> [<disp hex> ...]
"""
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe, KNOWN, label  # noqa: E402


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
    disps = {int(a, 16) for a in sys.argv[4:]}
    try:
        with open("/tools/sa_functions.txt") as f:
            for line in f:
                parts = line.split(None, 1)
                if len(parts) == 2:
                    KNOWN.setdefault(int(parts[0], 16), parts[1].strip())
    except OSError:
        pass
    starts = sorted(KNOWN)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True
    for ins in md.disasm(pe.read(start, end - start), start):
        if ins.id == 0:
            continue
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base != 0 and op.mem.disp in disps:
                print(f"0x{ins.address:X}  {label(ins.address, starts):45s} {ins.mnemonic} {ins.op_str}")
                break


if __name__ == "__main__":
    main()
