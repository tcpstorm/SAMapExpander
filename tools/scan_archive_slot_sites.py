#!/usr/bin/env python3
"""Find every instruction in gta_sa.exe (1.0 US HOODLUM) that names CStreaming::ms_files, so the
archive table can be moved somewhere with more than eight slots.

    CStreaming::ms_files   8 x 0x30 at 0x8E48D8, ending at 0x8E4A58
        +0x00  char name[0x28]      the archive path, copied in without a terminator
        +0x28  bool  notPlayerImg
        +0x2C  int   streamHandle   CdStreamOpen's return: the stream index, already << 24

Three shapes reference it and each is rewritten differently:

    base   a dword naming &ms_files[0].<field> - `mov eax, 0x8E48D8`, `mov [esi + 0x8E4904], eax`.
           Rewritten to the new base plus the same field offset.
    end    an IMMEDIATE naming one past the last entry - `cmp eax, 0x8E4A58`, the loop sentinels.
           Rewritten to the new base plus count * 0x30 plus the same field offset.
    (none) a MEMORY reference to that same address. 0x8E4A58 is also a live global in its own
           right - the linker put it directly behind the array and CStreaming reads and writes it
           as a byte. Those must not be touched, and this prints them separately so the table can
           be checked against them.

Anything naming a fixed middle entry (&ms_files[3], say) would need a rule of its own; there are
none, and the scan fails loudly rather than quietly if that ever stops being true.

Only the first .text and .HOODLUM hold live code. HOODLUM's section is stolen bytes with its own
data between them, so a linear sweep loses its framing there: every dword in it that names the
array is also anchored on directly and decoded backwards, exactly as
tools/scan_model_limit_sites.py does.

Usage: scan_archive_slot_sites.py <gta_sa.exe>
"""
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe, KNOWN, label  # noqa: E402

ARRAY = 0x8E48D8
STRIDE = 0x30
VANILLA_COUNT = 8
END = ARRAY + VANILLA_COUNT * STRIDE       # 0x8E4A58

FIELDS = {0x00: "name", 0x28: "notPlayerImg", 0x2C: "streamHandle"}

# Instructions a stub never contains; a decode that started on the wrong byte reaches one at once.
JUNK = {"ljmp", "lcall", "les", "lds", "lfs", "lgs", "lss", "bound", "arpl", "aaa", "aas", "aad",
        "aam", "daa", "das", "into", "salc", "xlatb", "in", "out", "insb", "insd", "outsb", "outsd",
        "loopne", "loope", "jcxz", "hlt", "iret", "iretd", "lock", "sysenter", "sysexit", "ud2"}


def field_name(off):
    return FIELDS.get(off, "+0x%X" % off)


def classify(ins):
    """Yield (kind, operand offset, old value, note) for one instruction, if it names the array."""
    enc = ins.encoding
    for op in ins.operands:
        if op.type == X86_OP_MEM and enc.disp_size == 4:
            v = op.mem.disp & 0xFFFFFFFF
            off, kind = enc.disp_offset, "mem"
        elif op.type == X86_OP_IMM and enc.imm_size == 4:
            v = op.imm & 0xFFFFFFFF
            off, kind = enc.imm_offset, "imm"
        else:
            continue
        if ARRAY <= v < ARRAY + STRIDE:
            yield ("base", off, v, "ms_files[0].%s" % field_name(v - ARRAY))
        elif ARRAY + STRIDE <= v < END:
            elem, field = divmod(v - ARRAY, STRIDE)
            yield ("middle", off, v, "ms_files[%d].%s" % (elem, field_name(field)))
        elif END <= v < END + STRIDE:
            # An end bound is an IMMEDIATE that a `cmp` closes a walk over the array with, and it
            # names a real field of the entry one past the last. Everything else in this window is
            # a different global: 0x8E4A58 is a byte CStreaming reads and writes, and 0x8E4A60 is
            # ms_channels, whose index-to-pointer arithmetic (`add edi, 0x8E4A60`) is an immediate
            # in exactly the same range and must not be moved with the array.
            note = "one past ms_files[%d].%s" % (VANILLA_COUNT - 1, field_name(v - END))
            if kind == "imm" and ins.mnemonic == "cmp" and (v - END) in FIELDS:
                yield ("end", off, v, note)
            else:
                yield ("behind", off, v, "0x%08X, not the array's end" % v)


def anchored(md, code, at, sva):
    """The instruction whose 4-byte operand sits exactly at `at`, or (None, None)."""
    for back in range(1, 16):
        start = at - back
        if start < 0:
            break
        got = list(md.disasm(code[start:start + 48], sva + start, count=3))
        if len(got) < 3 or any(i.id == 0 or i.mnemonic in JUNK for i in got):
            continue
        ins = got[0]
        if ins.address + ins.size < sva + at + 4:
            continue
        for hit in classify(ins):
            if ins.address + hit[1] == sva + at:
                return ins, hit
    return None, None


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    try:
        with open(__file__.rsplit("/", 1)[0] + "/sa_functions.txt") as f:
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

    live = []
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if (name == ".text" and not live) or name == ".HOODLUM":
            live.append((pe.image_base + va, min(vsize, rawsize) or rawsize, name))

    rows, behind, middle = [], [], []
    seen = set()

    def record(ins, hit, sname, extra=""):
        kind, off, old, note = hit
        key = (ins.address, off)
        if key in seen:
            return
        seen.add(key)
        text = "%s  %s %s" % (label(ins.address, starts), ins.mnemonic, ins.op_str)
        row = (ins.address, off, old, kind, note, text + extra, sname)
        (rows if kind in ("base", "end") else behind if kind == "behind" else middle).append(row)

    for sva, ssize, sname in live:
        code = pe.read(sva, ssize)
        for ins in md.disasm(code, sva):
            if ins.id == 0:
                continue
            for hit in classify(ins):
                record(ins, hit, sname)
        if sname != ".HOODLUM":
            continue
        for at in range(len(code) - 4):
            v = struct.unpack_from("<I", code, at)[0]
            if not (ARRAY <= v < END + STRIDE):
                continue
            if (sva + at) in {a + o for a, o, *_ in rows + behind + middle}:
                continue
            ins, hit = anchored(md, code, at, sva)
            if ins is not None:
                record(ins, hit, sname, "   (anchored on the constant)")

    # .rdata and .data are checked too: a pointer to the array stored in data would survive the
    # move and go on naming the old block.
    for name, va, vsize, rawptr, rawsize in pe.sections:
        if name not in (".rdata", ".data"):
            continue
        base, size = pe.image_base + va, min(vsize, rawsize)
        blob = pe.read(base, size)
        for at in range(0, len(blob) - 4):
            v = struct.unpack_from("<I", blob, at)[0]
            if ARRAY <= v < END + STRIDE:
                print("DATA 0x%X in %s holds 0x%08X - a stored pointer into the array"
                      % (base + at, name, v))

    rows.sort()
    print("// %d site(s): %d base, %d end"
          % (len(rows), sum(1 for r in rows if r[3] == "base"),
             sum(1 for r in rows if r[3] == "end")))
    for addr, off, old, kind, note, text, sname in rows:
        if kind == "base":
            print("    { 0x%07X, %d, 0x%02X },   // %s   %s"
                  % (addr, off, old - ARRAY, text, note))
    print()
    for addr, off, old, kind, note, text, sname in rows:
        if kind == "end":
            print("    { 0x%07X, %d, 0x%02X },   // %s   %s"
                  % (addr, off, old - END, text, note))

    print("\n// Left alone - the globals behind the array, named as memory rather than as a bound:")
    for addr, off, old, kind, note, text, sname in sorted(behind):
        print("//   0x%X  %s   (0x%08X)" % (addr, text, old))
    if middle:
        print("\n// UNEXPECTED - a fixed middle entry, which needs a rule of its own:")
        for addr, off, old, kind, note, text, sname in sorted(middle):
            print("//   0x%X  %s   %s" % (addr, text, note))
        sys.exit(1)


if __name__ == "__main__":
    main()
