#!/usr/bin/env python3
"""Print the bytes at each given address in gta_sa.exe as a string. Usage: readstr.py <exe> <hex addr>..."""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

pe = Pe(open(sys.argv[1], "rb").read())
for a in sys.argv[2:]:
    addr = int(a, 16)
    raw = pe.read(addr, 48) or b""
    print(f"0x{addr:X}: {raw.split(b'\\0')[0]!r}  raw={raw[:16].hex()}")
