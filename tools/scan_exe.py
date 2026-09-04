#!/usr/bin/env python3
"""Find every code site in gta_sa.exe (1.0 US) that touches int16/8 "compressed large vectors".

CPickup::SetPosn / GetPosn multiply by 8.0 / 0.125 through .rdata constants. Every other user of
the same compression (car generators, whatever breaks melee at +/-4096, ...) references the same
constants or calls the same accessors, so we list all of those sites.

Usage: scan_exe.py <path to gta_sa.exe>
"""
import struct
import sys
from collections import defaultdict

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

IMAGE_BASE = 0x400000
SET_POSN, GET_POSN, GET_X, GET_Y, GET_Z = 0x454960, 0x4549A0, 0x4549F0, 0x454A10, 0x454A30
ACCESSORS = {
    SET_POSN: "CPickup::SetPosn", GET_POSN: "CPickup::GetPosn",
    GET_X: "CPickup::GetXCoord", GET_Y: "CPickup::GetYCoord", GET_Z: "CPickup::GetZCoord",
}

# Known function starts (gta-reversed, 1.0 US) used to label hits. Extend as needed.
KNOWN = {
    0x454960: "CPickup::SetPosn", 0x4549A0: "CPickup::GetPosn", 0x4549F0: "CPickup::GetXCoord",
    0x454A10: "CPickup::GetYCoord", 0x454A30: "CPickup::GetZCoord", 0x454C70: "CPickup::IsVisible",
    0x4567E0: "CPickup::GiveUsAPickUpObject", 0x457410: "CPickup::Update", 0x458DE0: "CPickups::Update",
    0x4556C0: "CPickup::Remove", 0x4588B0: "CPickup::ProcessGunShot",
    0x629920: "CTaskSimpleFight::ProcessPed", 0x61C5E0: "CTaskSimpleFight::ControlFight",
    0x568B80: "CWorld::FindObjectsKindaColliding",
}


class Pe:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt_size = struct.unpack_from("<H", data, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
        sec = pe + 24 + opt_size
        self.sections = []
        for i in range(nsec):
            name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", data, sec + i * 40)
            self.sections.append((name.rstrip(b"\0").decode(), va, vsize, rawptr, rawsize))

    def section(self, name):
        for s in self.sections:
            if s[0] == name:
                return s
        raise KeyError(name)

    def read(self, addr, size):
        rva = addr - self.image_base
        for _, va, vsize, rawptr, rawsize in self.sections:
            if va <= rva < va + max(vsize, rawsize):
                off = rawptr + (rva - va)
                return self.data[off:off + size]
        return None

    def contains(self, name, addr):
        _, va, vsize, _, rawsize = self.section(name)
        rva = addr - self.image_base
        return va <= rva < va + max(vsize, rawsize)


def label(addr, starts):
    best = None
    for s in starts:
        if s <= addr and (best is None or s > best):
            best = s
    if best is None or addr - best > 0x8000:
        return "?"
    return f"{KNOWN[best]}+0x{addr - best:X}"


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    data = open(sys.argv[1], "rb").read()
    pe = Pe(data)
    if pe.image_base != IMAGE_BASE:
        print(f"warning: image base 0x{pe.image_base:X} != 0x{IMAGE_BASE:X}")
    text = pe.section(".text")
    text_va = pe.image_base + text[1]
    text_bytes = pe.read(text_va, text[4])
    print(f".text at 0x{text_va:X}, {len(text_bytes):#x} bytes; sections: {[s[0] for s in pe.sections]}")

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True

    # Optional function table: "0xADDR Name" per line, generated from plugin-sdk sources.
    try:
        with open("/tools/sa_functions.txt") as f:
            for line in f:
                parts = line.split(None, 1)
                if len(parts) == 2:
                    KNOWN.setdefault(int(parts[0], 16), parts[1].strip())
    except OSError:
        pass

    # 1) Constants referenced by the known accessors.
    consts = {}
    for fn in (SET_POSN, GET_POSN, GET_X):
        code = pe.read(fn, 0x60)
        for ins in md.disasm(code, fn):
            if ins.mnemonic == "ret":
                break
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp:
                    raw = pe.read(op.mem.disp, 4)
                    if raw and len(raw) == 4 and not pe.contains(".text", op.mem.disp):
                        consts[op.mem.disp] = struct.unpack("<f", raw)[0]
    print("\nCompression constants used by CPickup accessors:")
    for a, v in sorted(consts.items()):
        print(f"  0x{a:X} = {v}")
    scale_consts = {a for a, v in consts.items() if v in (8.0, 0.125)}

    # 2) Linear sweep of .text for references to those constants and calls to the accessors.
    const_hits = defaultdict(list)
    call_hits = defaultdict(list)
    for ins in md.disasm(text_bytes, text_va):
        if ins.id == 0:
            continue
        if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM:
            tgt = ins.operands[0].imm
            if tgt in ACCESSORS:
                call_hits[tgt].append(ins.address)
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and op.mem.disp in scale_consts:
                const_hits[op.mem.disp].append((ins.address, f"{ins.mnemonic} {ins.op_str}"))

    starts = sorted(KNOWN)
    print("\nCalls to CPickup position accessors:")
    for tgt, name in ACCESSORS.items():
        sites = call_hits.get(tgt, [])
        print(f"  {name} (0x{tgt:X}): {len(sites)} call site(s)")
        for a in sites:
            print(f"      0x{a:X}  {label(a, starts)}")

    print("\nDirect references to the 8.0 / 0.125 constants (inline compress/decompress sites):")
    for c in sorted(const_hits):
        print(f"  constant 0x{c:X} = {consts[c]}: {len(const_hits[c])} site(s)")
        for a, txt in const_hits[c]:
            print(f"      0x{a:X}  {label(a, starts):40s} {txt}")


if __name__ == "__main__":
    main()
