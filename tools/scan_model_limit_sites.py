#!/usr/bin/env python3
"""Find every instruction in gta_sa.exe (1.0 US HOODLUM) that has to change when the DFF model
id limit is raised, and write the inventory tools/model_limit_sites.txt.

Vanilla id space, contiguous and in this order:
    DFF 0 (20000) TXD 20000 (5000) COL 25000 (255) IPL 25255 (256) DAT 25511 (64)
    IFP 25575 (180) RRR 25755 (475) SCM 26230 (82) + 4 list-head sentinels = 26316 total.
Raising the DFF count by N shifts every base from TXD upwards by N; the two big arrays
(CStreaming::ms_aInfoForModel, CModelInfo::ms_modelInfoPtrs) and CModelInfo's atomic model info
store have to be reallocated, so every disp32/imm32 naming them is rewritten as well.

Site kinds written to the inventory:
    info       address in ms_aInfoForModel (0x8E4CC0, 0x14 bytes x 26316) - disp32 or imm32
    ptr        address in ms_modelInfoPtrs (0xA9B0C8, 4 bytes x 20000)    - disp32 or imm32
    atomic     address in ms_atomicModelInfoStore (0xAAE950, count + 0x20 x 14000)
    res        immediate equal to a resource base (new = old + N)
    resneg     immediate/displacement equal to -(base * scale) (new = old - N * scale)
    count4     immediate equal to the model count * 4 (a byte size)
    sentinel16 imm16 sentinel link index written into an int16 field (new = old + N)

20000 and 25000 are also ordinary literals (20 s / 25 s timers, money, stat thresholds), so a site
holding one is only accepted with evidence that it is an id: an array reference or a call into the
streaming/model-info/txd-store code nearby, and no CTimer read feeding it. The handful of genuine
sites with no such evidence are listed in ALLOW, the false positives that do have some in DENY.

Usage: scan_model_limit_sites.py <gta_sa.exe> [output file]
"""
import bisect
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe, KNOWN  # noqa: E402

INFO_BASE, INFO_ELEM, INFO_N = 0x8E4CC0, 0x14, 26316
PTR_BASE, PTR_ELEM, PTR_N = 0xA9B0C8, 4, 20000
ATOM_BASE, ATOM_ELEM, ATOM_N = 0xAAE950, 0x20, 14000
MODEL_COUNT = 20000
TIMERS = (0xB7CB84, 0xB7CB88, 0xB7CB8C, 0xB7CB94)  # CTimer::m_snTimeInMilliseconds and friends
# Functions whose presence proves the constant next to them is an id, not a literal.
ID_CODE = ((0x406000, 0x411000), (0x4C4000, 0x4C7000), (0x731000, 0x733000))

RESOURCE_BASES = {
    20000: "TXD", 25000: "COL", 25255: "IPL", 25511: "DAT",
    25575: "IFP", 25755: "RRR", 26230: "SCM", 26316: "TOTAL",
}
# A base +/- 1 appears where the game compares against the last id of a range.
RESOURCE_IMMS = {}
for _v, _n in RESOURCE_BASES.items():
    RESOURCE_IMMS[_v] = _n
    RESOURCE_IMMS[_v - 1] = _n + "-1"
    RESOURCE_IMMS[_v + 1] = _n + "+1"
for _i, _n in ((26312, "LOADED_START"), (26313, "LOADED_END"),
               (26314, "REQUESTED_START"), (26315, "REQUESTED_END")):
    RESOURCE_IMMS[_i] = "sentinel " + _n
AMBIGUOUS = {19999, 20000, 20001, 24999, 25000, 25001}

ALLOW = {  # genuine, but with no array/call evidence in reach
    0x4C487C: "CModelInfo::FindOtherTimeModel model id bound",
    0x4C4884: "CModelInfo::FindOtherTimeModel model id bound",
    0x566516: "CWorld::TestForUnusedModels stack buffer, 4 bytes per model",
    0x566532: "CWorld::TestForUnusedModels clears one dword per model",
    0x5B384D: "loading screen model loop bound",
    0x1564630: "CStreaming model loop bound (HOODLUM copy)",
    0x156651E: "txd id bound next to the [id - 20000] lookup below it (HOODLUM copy)",
}
DENY = {  # has evidence, but the constant is a literal
    0x687424: "20 second idle timer, the nearby call is unrelated",
    0x48FF33: "20 second task timer clamp, the nearby call is unrelated",
}
# Constants that land inside one of the arrays by coincidence. The two 0xAA0000 sites are bit
# masks; 0x8F8378/0x8FD198 bound the 16-entry special-model array walked from 0x8E4AF8 (both
# comparisons are always false, and would stop being false if they were moved to the heap);
# the other two are dwords read at unaligned interior offsets, i.e. decoded junk.
ADDRESS_DENY = {
    0x7AABD3: "or eax, 0xaa0000 - a bit mask",
    0x7B3D52: "or eax, 0xaa0000 - a bit mask",
    0x1566597: "bound of the special-model array at 0x8E4AF8, not of ms_aInfoForModel",
    0x156659F: "bound of the special-model array at 0x8E4AF8, not of ms_aInfoForModel",
    0x15665DA: "bound of the special-model array at 0x8E4AF8, not of ms_aInfoForModel",
    0x15665E2: "bound of the special-model array at 0x8E4AF8, not of ms_aInfoForModel",
    0x155A94A: "sub of an unaligned interior address; decoded junk",
    0x155D498: "dword read at an unaligned interior offset; decoded junk",
}


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    out = open(sys.argv[2] if len(sys.argv) > 2 else "tools/model_limit_sites.txt", "w")
    try:
        with open(__file__.rsplit("/", 1)[0] + "/sa_functions.txt") as f:
            for line in f:
                parts = line.split(None, 1)
                if len(parts) == 2:
                    KNOWN.setdefault(int(parts[0], 16), parts[1].strip())
    except OSError:
        pass
    starts = sorted(KNOWN)

    def label(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return "?"
        s = starts[i]
        return "?" if addr - s > 0x3000 else KNOWN[s] + (f"+0x{addr - s:X}" if addr != s else "")

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    md.skipdata = True

    # Only the first .text and .HOODLUM hold live code; the packed copies (_TEXT_HA, the second
    # .text at 0xCB1000) decode into plausible-looking garbage.
    live = []
    for i, (name, va, vsize, rawptr, rawsize) in enumerate(pe.sections):
        if (name == ".text" and not live) or name == ".HOODLUM":
            live.append((pe.image_base + va, min(vsize, rawsize) or rawsize, name))

    rows, skipped = [], []
    for sva, ssize, sname in live:
        code = pe.read(sva, ssize)
        window = []
        pending = []
        for ins in md.disasm(code, sva):
            window.append(ins)
            if len(window) > 15:
                window.pop(0)
            for site in list(pending):
                if ins.address - site[0].address > 0x40:
                    decide(site, rows, skipped, label)
                    pending.remove(site)
                else:
                    site[3].append(ins)
            if ins.id == 0:
                continue
            for kind, off, size, old, scale, note in classify(ins):
                if ins.address in ADDRESS_DENY and kind in ("info", "ptr", "atomic"):
                    skipped.append((ins.address, ADDRESS_DENY[ins.address],
                                    "%s %s | %s" % (ins.mnemonic, ins.op_str, note)))
                elif (kind == "res" and old in AMBIGUOUS) or kind == "count4" or \
                        (kind == "resneg" and abs(sign(old)) // scale in AMBIGUOUS):
                    pending.append((ins, (kind, off, size, old, scale, note), list(window), []))
                else:
                    rows.append((ins.address, kind, off, size, old, scale, note, label(ins.address)))
        for site in pending:
            decide(site, rows, skipped, label)

    rows.sort()
    for line in __doc__.split("Usage:")[0].rstrip().splitlines():
        out.write(("# " + line).rstrip() + "\n")
    out.write("#\n# Generated by tools/scan_model_limit_sites.py; do not edit by hand.\n")
    out.write("# address | kind | operand offset | operand size | old value | scale | note\n")
    for addr, kind, off, size, old, scale, note, lbl in rows:
        out.write("0x%X | %-10s | +%d | %d | 0x%08X | %d | %s | %s\n"
                  % (addr, kind, off, size, old & 0xFFFFFFFF, scale, lbl, note))
    out.write("#\n# Candidates left alone (the constant is not an id):\n")
    for addr, why, note in sorted(skipped):
        out.write("# 0x%X | %s | %s\n" % (addr, why, note))
    counts = {}
    for r in rows:
        counts[r[1]] = counts.get(r[1], 0) + 1
    out.write("#\n# %d sites: %s\n" % (len(rows), ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))))
    out.close()
    print("%d sites, %d candidates left alone" % (len(rows), len(skipped)))
    for k, v in sorted(counts.items()):
        print("  %-10s %d" % (k, v))


def sign(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v >= 0x80000000 else v


def classify(ins):
    """Yield (kind, operand offset, operand size, old value, scale, note) for one instruction."""
    enc = ins.encoding
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.disp and enc.disp_size == 4:
            for hit in match_address(op.mem.disp, False):
                yield (hit[0], enc.disp_offset, 4, hit[1], hit[2], hit[3])
            for hit in match_negative(op.mem.disp):
                yield (hit[0], enc.disp_offset, 4, hit[1], hit[2], hit[3])
            # `lea esi, [edi + 25000]` folds a base into a displacement (CColStore::LoadAllCollision
            # builds its col ids that way); only register-relative forms, an absolute address
            # this small cannot be data.
            v = op.mem.disp & 0xFFFFFFFF
            if (op.mem.base or op.mem.index) and v in RESOURCE_IMMS and v >= MODEL_COUNT - 1:
                yield ("res", enc.disp_offset, 4, v, 1, RESOURCE_IMMS[v] + " as displacement")
        if op.type == X86_OP_IMM and enc.imm_size == 4:
            for hit in match_address(op.imm, True):
                yield (hit[0], enc.imm_offset, 4, hit[1], hit[2], hit[3])
            for hit in match_negative(op.imm):
                yield (hit[0], enc.imm_offset, 4, hit[1], hit[2], hit[3])
            v = op.imm & 0xFFFFFFFF
            if v in RESOURCE_IMMS and v >= MODEL_COUNT - 1:
                yield ("res", enc.imm_offset, 4, v, 1, RESOURCE_IMMS[v])
            elif v == MODEL_COUNT * 4:
                yield ("count4", enc.imm_offset, 4, v, 1, "model count * 4")
        if op.type == X86_OP_IMM and enc.imm_size == 2:
            v = op.imm & 0xFFFF
            if 26312 <= v <= 26315:
                yield ("sentinel16", enc.imm_offset, 2, v, 1, RESOURCE_IMMS[v])


def match_address(value, is_imm):
    v = value & 0xFFFFFFFF
    # The info array's last element ends at 0x9654B0, where ms_disableStreaming lives: an
    # immediate there is the array's end pointer, a displacement is the unrelated bool.
    if INFO_BASE <= v < INFO_BASE + INFO_ELEM * (INFO_N + 1):
        elem, field = divmod(v - INFO_BASE, INFO_ELEM)
        if elem < INFO_N or is_imm:
            return [("info", v, 1, "ms_aInfoForModel[%d]+0x%X" % (elem, field))]
    if PTR_BASE <= v < PTR_BASE + PTR_ELEM * (PTR_N + 1) and (v - PTR_BASE) % PTR_ELEM == 0:
        return [("ptr", v, 1, "ms_modelInfoPtrs[%d]" % ((v - PTR_BASE) // PTR_ELEM))]
    if v == ATOM_BASE:
        return [("atomic", v, 1, "ms_atomicModelInfoStore.m_nCount")]
    if ATOM_BASE + 4 <= v < ATOM_BASE + 4 + ATOM_ELEM:
        return [("atomic", v, 1, "ms_atomicModelInfoStore.m_aObjects[0]+0x%X" % (v - ATOM_BASE - 4))]
    return []


def match_negative(value):
    v = sign(value)
    if v >= 0:
        return []
    for base, name in RESOURCE_BASES.items():
        for scale in (1, 3):  # the only strides the game folds a base into
            if v == -base * scale:
                return [("resneg", v & 0xFFFFFFFF, scale,
                         "-%s%s" % (name, "" if scale == 1 else " * %d" % scale))]
    return []


def decide(site, rows, skipped, label):
    """Accept an ambiguous 20000/25000 constant only with evidence that it is an id."""
    ins, hit, before, after = site
    kind, off, size, old, scale, note = hit
    text = "%s %s | %s" % (ins.mnemonic, ins.op_str, note)
    if ins.address in ALLOW:
        rows.append((ins.address, kind, off, size, old, scale,
                     note + " (" + ALLOW[ins.address] + ")", label(ins.address)))
        return
    if ins.address in DENY:
        skipped.append((ins.address, DENY[ins.address], text))
        return
    if ins.mnemonic == "push":
        skipped.append((ins.address, "pushed as a call argument", text))
        return
    evidence = []
    for other in before + after:
        if other.id == 0:
            continue
        for op in other.operands:
            if op.type == X86_OP_MEM and (op.mem.disp & 0xFFFFFFFF) in TIMERS:
                skipped.append((ins.address, "fed by a CTimer read", text))
                return
            v = op.mem.disp if op.type == X86_OP_MEM else (op.imm if op.type == X86_OP_IMM else 0)
            if match_address(v, op.type == X86_OP_IMM) and other.address != ins.address:
                evidence.append("array reference at 0x%X" % other.address)
        if other.mnemonic in ("call", "jmp") and other.operands and \
                other.operands[0].type == X86_OP_IMM and \
                any(lo <= other.operands[0].imm < hi for lo, hi in ID_CODE):
            evidence.append("call %s" % label(other.operands[0].imm))
    if evidence:
        rows.append((ins.address, kind, off, size, old, scale,
                     note + " (" + evidence[0] + ")", label(ins.address)))
    else:
        skipped.append((ins.address, "no id evidence nearby", text))


if __name__ == "__main__":
    main()
