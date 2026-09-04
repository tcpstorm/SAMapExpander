#!/usr/bin/env python3
"""Print the float32 stored at each given address in gta_sa.exe. Usage: readfloat.py <exe> <hex addr>..."""
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

pe = Pe(open(sys.argv[1], "rb").read())
for a in sys.argv[2:]:
    addr = int(a, 16)
    raw = pe.read(addr, 4)
    print(f"0x{addr:X} = {struct.unpack('<f', raw)[0]!r}  (int {struct.unpack('<i', raw)[0]})")
