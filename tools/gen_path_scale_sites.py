#!/usr/bin/env python3
"""Turn tools/path_scale_sites.txt into the constexpr table src/limits/PathScaleSites.h.

    gen_path_scale_sites.py <path to gta_sa.exe> [inventory] [header]

The decode sites come from the inventory, which tools/scan_path_scale_sites.py writes and whose
docstring explains how each one was classified. The ENCODE sites are found here rather than read
from a list, because a list was wrong once: the first pass concluded CPathFind::CompleteNewInterior
was the only place a path position is compressed, and it is not - 0x44CF40 is an out-of-line
CPathNode position setter that writes the same +8/+0x0A/+0x0C triple. So this scans for the shape
instead: every `fmul dword ptr [0x859000]` (the shared 8.0) followed within a few instructions by a
16-bit store to one of those three offsets. Anything else that reads that 8.0 - CPickup, the audio
boxes, CTrainNode, CCarGenerator, the car generator inside CVehicle's constructor - stores to
different offsets and is left out, and the header records how many were rejected so a change in the
count is noticed.

Every site, decode and encode alike, is checked to hold the exact six bytes of the instruction it
is supposed to be before it is written into the header. A mismatch here is an error rather than a
warning: the header is only useful if every entry in it is a `D8 0D <disp32>` whose last four bytes
can be rewritten.
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

DECODE_CONST = 0x858C48        # 0.125, shared with CPickup, CTrainNode, COccluder, the audio boxes
ENCODE_CONST = 0x859000        # 8.0, shared with the same
FMUL_DECODE = bytes.fromhex("d80d488c8500")
FMUL_ENCODE = bytes.fromhex("d80d00908500")
DISP32_OFFSET = 2              # `fmul dword ptr [imm32]` is D8 0D then the address
NODE_COORDS = (8, 0x0A, 0x0C)  # CPathNode x, y, z


class Pe:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt = struct.unpack_from("<H", data, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        sec = pe + 24 + opt
        self.sections = []
        for i in range(nsec):
            n, vs, va, rs, rp = struct.unpack_from("<8sIIII", data, sec + i * 40)
            self.sections.append((n.rstrip(b"\0").decode(), va, vs, rp, rs))

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s

    def off(self, addr):
        rva = addr - self.image_base
        for n, va, vs, rp, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)

    def read(self, addr, n):
        o = self.off(addr)
        return self.data[o:o + n] if o is not None else b""


def read_inventory(path):
    """[(address, kind, label)] for the two decode sections of the inventory."""
    import re
    out, section = [], None
    for line in open(path, encoding="latin-1"):
        if line.startswith("CPathNode coordinate decode sites"):
            section = "kPathNode"
            continue
        if line.startswith("CCarPathLink coordinate decode sites"):
            section = "kCarPathLink"
            continue
        if re.match(r"^[A-Z_]+ - \d+$", line.strip()):
            section = None            # the rejected and unclear sections, which are not patched
            continue
        m = re.match(r"^([0-9A-F]{8})\s+\+\S+\s+(\S+)", line)
        if m and section:
            out.append((int(m.group(1), 16), section, m.group(2)))
    return out


def store_target(op_str):
    """(base register, offset) of a `word ptr [reg + N], ax`, or None if it is not that shape."""
    if not op_str.startswith("word ptr [") or not op_str.endswith(", ax"):
        return None
    inside = op_str[op_str.index("[") + 1:op_str.index("]")]
    if "*" in inside:
        return None                    # an indexed store, so not a fixed struct field
    if "+" not in inside:
        return inside.strip(), 0
    reg, tail = inside.split("+", 1)
    try:
        return reg.strip(), int(tail.strip(), 0)
    except ValueError:
        return None


def find_encode_sites(pe):
    """[(address, store)] - every 8.0 multiply that compresses a coordinate INTO a CPathNode.

    A store to +8 alone is not enough: the audio boxes write +0x0C, +0x0E, +0x10, +0x12, +0x14 and
    the car generator inside CVehicle's constructor writes +4, +6, +8, so each of those collides
    with one third of the triple. What identifies a CPathNode is the whole triple written off the
    SAME base register within one short window - {8, 0x0A, 0x0C} and nothing else - which is the
    same sibling-offset test tools/scan_path_scale_sites.py uses on the decode side.
    """
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    _n, va, vs, rp, rs = pe.section(".text")
    blob = pe.data[rp:rp + min(vs, rs)]
    base = pe.image_base + va
    at, kept, rejected = 0, [], 0
    while True:
        i = blob.find(FMUL_ENCODE, at)
        if i < 0:
            break
        at = i + 1
        addr = base + i
        window = list(md.disasm(blob[max(0, i - 48):i + 120], base + max(0, i - 48)))
        mine, siblings = None, {}
        for x in window:
            if x.mnemonic != "mov":
                continue
            hit = store_target(x.op_str)
            if not hit:
                continue
            siblings.setdefault(hit[0], set()).add(hit[1])
            if mine is None and x.address > addr:
                mine = (hit, x.op_str)
        if mine is None:
            continue                   # nothing is stored, so this is not a compression at all
        (reg, _offset), store = mine
        if siblings.get(reg, set()) == set(NODE_COORDS):
            kept.append((addr, store))
        else:
            rejected += 1
    return kept, rejected


def main():
    exe = sys.argv[1]
    inventory = sys.argv[2] if len(sys.argv) > 2 else "tools/path_scale_sites.txt"
    header = sys.argv[3] if len(sys.argv) > 3 else "src/limits/PathScaleSites.h"
    pe = Pe(open(exe, "rb").read())

    decode = read_inventory(inventory)
    if not decode:
        sys.exit("no decode sites read from %s" % inventory)
    encode, rejected = find_encode_sites(pe)

    bad = [hex(a) for a, _k, _l in decode if pe.read(a, 6) != FMUL_DECODE]
    bad += [hex(a) for a, _l in encode if pe.read(a, 6) != FMUL_ENCODE]
    if bad:
        sys.exit("these sites do not hold the instruction they should: %s" % ", ".join(bad))
    if len(set(a for a, _k, _l in decode)) != len(decode):
        sys.exit("the inventory lists an address twice")

    nodes = [d for d in decode if d[1] == "kPathNode"]
    links = [d for d in decode if d[1] == "kCarPathLink"]
    with open(header, "w") as out:
        out.write(
            "// Generated by tools/gen_path_scale_sites.py from tools/path_scale_sites.txt.\n"
            "// Every instruction in gta_sa.exe 1.0 US that scales a path coordinate by the shared\n"
            "// 0.125 at 0x858C48, or compresses one back through the shared 8.0 at 0x859000.\n"
            "// Do not edit by hand; re-run tools/scan_path_scale_sites.py and this generator.\n"
            "//\n"
            "// Every one of these is the same six bytes - D8 0D <disp32> - so patching a site is\n"
            "// rewriting four bytes at address + 2, and verifying one is comparing six.\n"
            "#pragma once\n#include <cstdint>\n\nnamespace PathScale {\n\n"
            "// The two shared constants. Both are read by a great deal of code that has nothing to\n"
            "// do with paths - CPickup, CCarGenerator, CTrainNode, COccluder, the audio boxes and\n"
            "// several render pipelines - and those sites keep pointing at them.\n"
            "constexpr uint32_t kVanillaDecode = 0x%06X;   // 0.125\n"
            "constexpr uint32_t kVanillaEncode = 0x%06X;   // 8.0\n"
            "constexpr uint8_t kDispOffset = %d;           // bytes from the instruction to the disp32\n"
            "constexpr uint8_t kInstructionBytes[] = { 0xD8, 0x0D };\n\n"
            % (DECODE_CONST, ENCODE_CONST, DISP32_OFFSET))
        out.write("// %d sites decode a CPathNode coordinate (int16 at +8, +0x0A, +0x0C).\n"
                  "constexpr uint32_t kNodeDecodeSites[] = {\n" % len(nodes))
        last = None
        for addr, _kind, label in nodes:
            fn = label.rsplit("+", 1)[0]
            if fn != last:
                out.write("    // %s\n" % fn)
                last = fn
            out.write("    0x%06X,\n" % addr)
        out.write("};\n\n")
        out.write("// %d sites decode a CCarPathLink coordinate (int16 at +0 and +2). These are the\n"
                  "// ones a scan for the CPathNode offsets alone misses, and missing them would\n"
                  "// leave every car-AI routine reading its links at the old scale.\n"
                  "constexpr uint32_t kLinkDecodeSites[] = {\n" % len(links))
        last = None
        for addr, _kind, label in links:
            fn = label.rsplit("+", 1)[0]
            if fn != last:
                out.write("    // %s\n" % fn)
                last = fn
            out.write("    0x%06X,\n" % addr)
        out.write("};\n\n")
        out.write("// %d sites compress a coordinate back into a CPathNode, found by shape rather\n"
                  "// than from a list: an 8.0 multiply whose result is stored 16-bit to +8, +0x0A\n"
                  "// or +0x0C. %d other readers of that 8.0 store somewhere else and are left alone.\n"
                  "constexpr uint32_t kEncodeSites[] = {\n" % (len(encode), rejected))
        for addr, store in encode:
            out.write("    0x%06X,   // %s\n" % (addr, store))
        out.write("};\n\n}   // namespace PathScale\n")

    print("%s: %d node decode, %d link decode, %d encode (%d other 8.0 readers left alone)"
          % (header, len(nodes), len(links), len(encode), rejected))
    for addr, store in encode:
        print("   encode 0x%06X  %s" % (addr, store))


if __name__ == "__main__":
    main()
