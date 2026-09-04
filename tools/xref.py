#!/usr/bin/env python3
"""List every .text instruction referencing the given absolute addresses (memory operands or immediates).

Usage: xref.py <gta_sa.exe> <hex addr> [<hex addr> ...]
"""
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe, KNOWN, label  # noqa: E402


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    targets = {int(a, 16) for a in sys.argv[2:]}
    try:
        with open("/tools/sa_functions.txt") as f:
            for line in f:
                parts = line.split(None, 1)
                if len(parts) == 2:
                    KNOWN.setdefault(int(parts[0], 16), parts[1].strip())
    except OSError:
        pass
    starts = sorted(KNOWN)

    text = pe.section(".text")
    text_va = pe.image_base + text[1]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True
    for ins in md.disasm(pe.read(text_va, text[4]), text_va):
        if ins.id == 0:
            continue
        hit = False
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.disp in targets:
                hit = True
            if op.type == X86_OP_IMM and op.imm in targets:
                hit = True
        if hit:
            print(f"0x{ins.address:X}  {label(ins.address, starts):45s} {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
