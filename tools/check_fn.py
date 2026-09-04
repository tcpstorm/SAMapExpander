#!/usr/bin/env python3
"""Disassemble address ranges in gta_sa.exe.

Usage: check_fn.py <gta_sa.exe> <hex start>[-<hex end>] ...   (no end: ~28 instructions or until ret)
"""
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    for arg in sys.argv[2:]:
        if "-" in arg:
            start, end = (int(a, 16) for a in arg.split("-"))
            limit = None
        else:
            start, end, limit = int(arg, 16), int(arg, 16) + 0x100, 28
        code = pe.read(start, end - start)
        print(f"\n== 0x{start:X}-0x{end:X} ==")
        n = 0
        for ins in md.disasm(code, start):
            print(f"  0x{ins.address:X}: {ins.mnemonic} {ins.op_str}")
            n += 1
            if limit and (ins.mnemonic == "ret" or n >= limit):
                break


if __name__ == "__main__":
    main()
