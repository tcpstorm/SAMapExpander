#!/usr/bin/env python3
"""Walk a GTA GXT file (III bare TKEY, VC TABL+named keys, SA hashed keys) and report its layout.

Usage: gxt_dump.py <file.gxt> [max entries to print]
"""
import struct
import sys

data = open(sys.argv[1], "rb").read()
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 5
print(f"{len(data)} bytes, first 8: {data[:8]!r}")


def u32(at):
    return struct.unpack_from("<I", data, at)[0]


def table_pair(at, entry_size, bits, label):
    if data[at:at + 4] != b"TKEY" and data[at + 8:at + 12] == b"TKEY":
        print(f"  {label}: table name {data[at:at+8]!r}")
        at += 8
    if data[at:at + 4] != b"TKEY":
        print(f"  {label}: expected TKEY at {at}, found {data[at:at+4]!r}")
        return None
    key_size = u32(at + 4)
    key_block = at + 8
    dat_header = key_block + key_size
    if data[dat_header:dat_header + 4] != b"TDAT":
        print(f"  {label}: expected TDAT at {dat_header}, found {data[dat_header:dat_header+4]!r}")
        return None
    dat_size = u32(dat_header + 4)
    dat_block = dat_header + 8
    n = key_size // entry_size
    print(f"  {label}: TKEY@{at} size {key_size} ({n} entries of {entry_size}), TDAT@{dat_header} size {dat_size}, ends {dat_block + dat_size}")
    bad = 0
    shown = 0
    for i in range(n):
        e = key_block + i * entry_size
        off = u32(e)
        if off >= dat_size:
            bad += 1
            continue
        if entry_size == 12:
            name = data[e + 4:e + 12].split(b"\0")[0].decode("latin-1")
        else:
            name = f"#{u32(e + 4):08X}"
        if shown < limit:
            p = dat_block + off
            if bits == 16:
                chars = []
                while p + 1 < dat_block + dat_size:
                    c = data[p] | (data[p + 1] << 8)
                    if c == 0:
                        break
                    chars.append(chr(c) if c < 0x10000 else "?")
                    p += 2
                text = "".join(chars)
            else:
                end = data.find(b"\0", p, dat_block + dat_size)
                text = data[p:end].decode("latin-1")
            print(f"    {name!r} -> {text!r}")
            shown += 1
    if bad:
        print(f"  {label}: {bad} entries point past TDAT")
    return dat_block + dat_size


if data[:4] == b"TKEY":
    table_pair(0, 12, 16, "III main")
elif data[:4] == b"TABL":
    size = u32(4)
    entries = size // 12
    print(f"VC format: TABL size {size} ({entries} tables)")
    for i in range(entries):
        e = 8 + i * 12
        name = data[e:e + 8].split(b"\0")[0].decode("latin-1")
        off = u32(e + 8)
        print(f" table {name!r} @ {off}")
        table_pair(off, 12, 16, name)
else:
    version, bits = struct.unpack_from("<HH", data, 0)
    print(f"SA format: version {version}, {bits} bit")
    at = 4
    if data[at:at + 4] == b"TABL":
        size = u32(at + 4)
        for i in range(size // 12):
            e = at + 8 + i * 12
            name = data[e:e + 8].split(b"\0")[0].decode("latin-1")
            off = u32(e + 8)
            print(f" table {name!r} @ {off}")
            table_pair(off, 8, bits, name)
