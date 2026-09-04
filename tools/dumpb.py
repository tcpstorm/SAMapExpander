#!/usr/bin/env python3
"""Print raw bytes (and, for E8/E9, the branch target) at each address. Usage: dumpb.py <exe> <hex addr>:<len>..."""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

pe = Pe(open(sys.argv[1], "rb").read())
for arg in sys.argv[2:]:
    addr, n = arg.split(":")
    addr, n = int(addr, 16), int(n)
    raw = pe.read(addr, n)
    print(f"0x{addr:X}: " + " ".join(f"{b:02X}" for b in raw))
    if raw[0] in (0xE8, 0xE9):
        target = (addr + 5 + struct.unpack("<i", raw[1:5])[0]) & 0xFFFFFFFF
        print(f"   -> 0x{target:X}")
