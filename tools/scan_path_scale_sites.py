#!/usr/bin/env python3
"""Classify every instruction in gta_sa.exe (1.0 US HOODLUM) that reads the shared 0.125 constant
at 0x858C48, and write the inventory tools/path_scale_sites.txt.

    scan_path_scale_sites.py <path to gta_sa.exe> [out.txt] [out.json]

WHY. San Andreas stores a path node position as three int16 multiplied by that 0.125, so a node
can only describe a point within +/-4095.875. Both shipped regions sit past that, which is why
nothing drives or walks in them without help - see docs/known-limitations.md. Widening the
encoding means giving the path code its own copy of the constant: rewrite the disp32 of every
instruction that decodes a path position so it reads, say, 0.5 instead, and the same node reaches
+/-16383.5 at a coarser step. This scan is the measurement of what "every instruction" means.

The catch, and the reason a pattern match is not enough on its own: 0x858C48 is a generic 0.125
that 89 different functions read. CPickup, CTrainNode, CCarGenerator, COccluder, the audio boxes
and several render pipelines all decompress their own positions through it, and CCarGenerator in
particular compresses at struct offsets +4/+6/+8, so its x load is byte-identical in shape to a
path node's. Repointing one of those would move San Andreas' own car generators.

There are also TWO compressed path structures, not one:
    CPathNode      28 bytes, coordinates at +8 / +0x0A / +0x0C, array ThePaths+0x804 = 0x96F854
    CCarPathLink   14 bytes, coordinates at +0    / +0x02,      array ThePaths+0x924 = 0x96F974
Both scale by the same 0.125 and both must move together, or pedestrians would route correctly
while every car-AI routine read its links unscaled.

Evidence used, in order of strength:
  E1 base pointer provably from ThePaths.m_pPathNodes / m_pCarPathLinks
     ([X*4+0x96F854], [R+X*4+0x804/0x904], add R,0x804, stride imul 0x1C / 0x0E)
  E2 base pointer provably an absolute immediate global  -> not path data
  E3 sibling 16-bit loads off the SAME base register in a +/-24 instruction window: the set of
     struct offsets touched identifies the struct
        {8,A,C}      CPathNode        {0,2}        CCarPathLink
        {0,2,4}      CTrainNode       {4,6,8}      CCarGenerator
        {0x10,12,14} CPickup
  E4 whether the enclosing function ever touches the path arrays at all
  E5 hand-adjudicated overrides (leaf accessors, settled by finding their callers)

The dataflow walk is esp-aware: a push or a sub esp,N between the 16-bit store and the fild
renames the [esp+N] slot, and without that 30 sites look untraceable.
"""
import struct, sys, bisect, json, re
from collections import Counter, defaultdict
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

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
            n, vs, va, rs, rp = struct.unpack_from("<8sIIII", data, sec + i*40)
            self.sections.append((n.rstrip(b"\0").decode(), va, vs, rp, rs))
    def sec(self, name):
        for s in self.sections:
            if s[0] == name: return s
    def off(self, addr):
        rva = addr - self.image_base
        for n, va, vs, rp, rs in self.sections:
            if va <= rva < va + max(vs, rs): return rp + (rva - va)
    def read(self, addr, n):
        o = self.off(addr)
        return self.data[o:o+n] if o is not None else b""

names = {}
for line in open(__file__.rsplit("/", 1)[0] + "/sa_functions.txt", encoding="utf-8", errors="replace"):
    line = line.strip()
    p = line.split(None, 1)
    if len(p) == 2 and p[0].startswith("0x"):
        try: names[int(p[0], 16)] = p[1]
        except ValueError: pass
keys = sorted(names)
def owner(a):
    i = bisect.bisect_right(keys, a) - 1
    return (names[keys[i]], keys[i]) if i >= 0 else ("?", 0)
def nextsym(f):
    i = bisect.bisect_right(keys, f)
    return keys[i] if i < len(keys) else f + 0x2000

OUT_TXT = sys.argv[2] if len(sys.argv) > 2 else "tools/path_scale_sites.txt"
OUT_JSON = sys.argv[3] if len(sys.argv) > 3 else ""   # per-site provenance dump, only on request
pe = Pe(open(sys.argv[1], "rb").read())
md = Cs(CS_ARCH_X86, CS_MODE_32)
_, va, vsize, rawptr, rawsize = pe.sec(".text")
tbase = pe.image_base + va
blob = pe.data[rawptr:rawptr + min(vsize, rawsize)]

def scanrefs(target):
    needle = struct.pack("<I", target); tag = "0x%x" % target
    hits = {}; at = 0
    while True:
        i = blob.find(needle, at)
        if i < 0: break
        at = i + 1
        for back in range(10, 0, -1):
            s = i - back
            if s < 0: continue
            g = list(md.disasm(blob[s:s+16], tbase + s, count=1))
            if not g: continue
            ins = g[0]
            if ins.size >= back + 4 and tag in ins.op_str:
                hits[ins.address] = (ins.mnemonic, ins.op_str); break
    return hits

BODY = {}
def body_of(f):
    if f not in BODY:
        end = min(nextsym(f), f + 0x6000)
        BODY[f] = [(x.address, x.mnemonic, x.op_str) for x in md.disasm(pe.read(f, end - f), f)]
    return BODY[f]

def back_stream(addr, span=0x180):
    best = None
    for s in range(addr - span, addr):
        out = []
        for x in md.disasm(pe.read(s, addr - s + 16), s):
            out.append((x.address, x.mnemonic, x.op_str))
            if x.address == addr:
                if best is None or len(out) > len(best): best = out
                break
            if x.address > addr: break
    return best

def stream_to(addr):
    fn, f = owner(addr)
    b = body_of(f) if f else []
    for i, (a, m, o) in enumerate(b):
        if a == addr: return b, i, "fn"
    best = None
    for s in range(addr - 64, addr):
        out = []
        for x in md.disasm(pe.read(s, addr - s + 16), s):
            out.append((x.address, x.mnemonic, x.op_str))
            if x.address == addr:
                if best is None or len(out) > len(best): best = out
                break
            if x.address > addr: break
    return (best or [(addr, "?", "?")]), len(best or [1]) - 1, "back"

MEM = re.compile(r"(?:(byte|word|dword|qword) ptr )?\[([^\]]+)\]")
def memparse(ops):
    m = MEM.search(ops)
    if not m: return None
    size, inner = m.group(1), m.group(2)
    regs, disp = [], 0
    for p in [q.strip() for q in re.split(r"(?=[+-])", inner)]:
        sign = -1 if p.startswith("-") else 1
        p = p.lstrip("+-").strip()
        if not p: continue
        if re.fullmatch(r"0x[0-9a-f]+|\d+", p): disp += sign * int(p, 0)
        elif "*" in p: regs.append(p.split("*")[0].strip())
        else: regs.append(p)
    return dict(size=size, regs=regs, disp=disp, raw=inner)

W32 = {"eax","ebx","ecx","edx","esi","edi","ebp","esp"}
SUB = {"ax":"eax","bx":"ebx","cx":"ecx","dx":"edx","si":"esi","di":"edi","bp":"ebp",
       "al":"eax","bl":"ebx","cl":"ecx","dl":"edx","ah":"eax","bh":"ebx","ch":"ecx","dh":"edx"}
def norm(r): return SUB.get(r, r)

def esp_effect(m, o):
    """net change to esp made by this instruction; None = unknown"""
    if m in ("push", "pushfd", "pushal"): return -4
    if m in ("pop", "popfd"): return 4
    if m in ("sub", "add") and o.startswith("esp, 0x"):
        try: n = int(o.split(", ")[1], 16)
        except ValueError: return None
        return -n if m == "sub" else n
    if m == "lea" and o.startswith("esp, [esp]"): return 0
    if o.startswith("esp,") or o.startswith("esp "): return None
    if m in ("ret", "leave", "enter", "int3"): return None
    return 0

def trace(b, i):
    for j in range(i-1, max(-1, i-9), -1):
        if b[j][1].startswith("fild"): fi = j; break
    else:
        return dict(kind="nofild", detail=",".join(m for _, m, _ in b[max(0,i-6):i] if m.startswith("f")))
    mp = memparse(b[fi][2])
    if mp is None: return dict(kind="unclear", detail="fild " + b[fi][2])
    if mp["size"] == "word":
        return dict(kind="load16", disp=mp["disp"], regs=mp["regs"], at=b[fi][0], idx=fi)
    # find the store that filled the slot, tracking esp so that a `push` or `sub esp`
    # between the store and the fild does not hide it behind a different [esp+N] name
    espslot = mp["regs"] == ["esp"]
    src = mi = None
    for espaware in ((True, False) if espslot else (False,)):
        S = 0
        for j in range(fi-1, -1, -1):
            a2, m2, o2 = b[j]
            if espaware:
                e = esp_effect(m2, o2)
                if e is None: break
                S += e
                d = mp["disp"] + S
                want = ("dword ptr [esp + %#x], " % d) if d else "dword ptr [esp], "
            else:
                want = "dword ptr [" + mp["raw"] + "], "
            if m2 == "mov" and o2.startswith(want):
                src = o2.split("], ", 1)[1].strip(); mi = j; break
        if src is not None: break
    if src is None:
        return dict(kind="unclear", detail="fild %s : no feeding store in function" % b[fi][2])
    if src not in W32: return dict(kind="unclear", detail="slot <- " + src)
    for j in range(mi-1, -1, -1):
        a2, m2, o2 = b[j]
        if "," not in o2: continue
        if norm(o2.split(",")[0].strip()) != src: continue
        if m2 in ("movsx", "movzx"):
            mp2 = memparse(o2)
            if mp2 and mp2["size"] == "word":
                return dict(kind="load16", disp=mp2["disp"], regs=mp2["regs"], at=a2, idx=j)
        return dict(kind="other", detail="%s %s" % (m2, o2), at=a2)
    return dict(kind="unclear", detail="def of %s not found" % src)

GLOBARR = re.compile(r"dword ptr \[(\w+)\*4 \+ (0x[0-9a-f]+)\]")
DISPARR = re.compile(r"dword ptr \[\w+ \+ \w+\*4 \+ (0x[0-9a-f]+)\]")
IMM     = re.compile(r"^(0x[0-9a-f]+)$")
def resolve(b, prov):
    idx = prov["idx"]
    want = set(norm(r) for r in prov["regs"] if r != "esp")
    ev = dict(arr=None, objoff=None, stride=None, absimm=None, addoff=None)
    j = idx - 1; steps = 0
    while j >= 0 and want and steps < 800:
        a2, m2, o2 = b[j]; j -= 1; steps += 1
        p = [x.strip() for x in o2.split(",")]
        if m2 == "imul" and len(p) == 3 and re.fullmatch(r"0x[0-9a-f]+|\d+", p[2]) and norm(p[0]) in want:
            ev["stride"] = int(p[2], 0); continue
        if len(p) < 2: continue
        dst = norm(p[0])
        if dst not in want: continue
        rhs = o2.split(",", 1)[1].strip()
        if m2 == "add" and IMM.match(rhs) and int(rhs, 16) in (0x804, 0x904, 0x924, 0xA24):
            ev["addoff"] = int(rhs, 16); want.discard(dst); continue
        g = DISPARR.search(rhs)
        if g and m2 == "mov":
            ev["objoff"] = int(g.group(1), 16); want.discard(dst); continue
        g = GLOBARR.search(rhs)
        if g and m2 == "mov":
            ev["arr"] = int(g.group(2), 16); want.discard(dst); continue
        if m2 == "mov" and IMM.match(rhs):
            ev["absimm"] = int(rhs, 16); want.discard(dst); continue
        if m2 in ("mov", "lea", "add"):
            want.discard(dst)
            mp = memparse(rhs)
            regs = mp["regs"] if mp else re.findall(r"\b(e[a-z]{2})\b", rhs)
            want |= set(norm(r) for r in regs if r != "esp")
            continue
        want.discard(dst)
    return ev

def siblings(b, prov):
    """offsets of all 16-bit loads off the same base reg in a +/-24 instruction window"""
    idx = prov["idx"]
    bases = set(norm(r) for r in prov["regs"] if r != "esp")
    if not bases: return set()
    offs = set()
    for j in range(max(0, idx-24), min(len(b), idx+25)):
        a2, m2, o2 = b[j]
        if m2 not in ("movsx", "movzx"): continue
        mp = memparse(o2)
        if not mp or mp["size"] != "word": continue
        if set(norm(r) for r in mp["regs"] if r != "esp") == bases:
            offs.add(mp["disp"])
    return offs

FNEV = {}
def fnev(f):
    if f in FNEV: return FNEV[f]
    node = link = False
    for a, m, o in body_of(f):
        if "0x96f854" in o or "+ 0x804]" in o or "+ 0x904]" in o or o.endswith(", 0x804") or o.endswith(", 0x904"): node = True
        if "0x96f974" in o or "+ 0x924]" in o or "+ 0xa24]" in o or o.endswith(", 0x924") or o.endswith(", 0xa24"): link = True
        if m == "imul" and o.endswith(", 0x1c"): node = True
        if m == "imul" and o.endswith(", 0xe"): link = True
        if m == "call" and o == "0x420ac0": node = True
    FNEV[f] = (node, link)
    return FNEV[f]

# E5 hand adjudications (see report). addr -> (class, reason)
OVERRIDE = {
 0x420A29: ("PATHNODE", "CPathNode::GetPosition, 109 callers all pass m_pPathNodes elements"),
 0x420A3B: ("PATHNODE", "CPathNode::GetPosition"),
 0x420A45: ("PATHNODE", "CPathNode::GetPosition"),
 0x66732F: ("PATHNODE", "2nd copy of GetPosition at 0x667310; all 4 callers in ComputePathNodes pass [X*4+0x96F854]+i*0x1C"),
 0x667347: ("PATHNODE", "same function 0x667310"),
 0x66735C: ("PATHNODE", "same function 0x667310"),
 0x50805D: ("NOT", "leaf getter 0x508050, one of six on offsets +0xC..+0x16 of one object; no path evidence, no direct callers"),
 0x6F2C9D: ("NOT", "leaf getter 0x6F2C90 in the CCarGenerator accessor run (+6,+8) with an 8.0 setter at 0x6F2CB0"),
}
PN_ARR, PN_OFF = {0x96F854}, {0x804, 0x904}
CL_ARR, CL_OFF = {0x96F974}, {0x924, 0xA24}
COORD = (8, 0xA, 0xC)
NODESET, LINKSET = {8, 0xA, 0xC}, {0, 2}

sites = []
for addr, (mn, ops) in sorted(scanrefs(0x858C48).items()):
    b, i, how = stream_to(addr)
    prov = trace(b, i)
    if prov["kind"] == "unclear":
        b2 = back_stream(addr)
        if b2:
            p2 = trace(b2, len(b2) - 1)
            if p2["kind"] != "unclear":
                b, i, prov, how = b2, len(b2) - 1, p2, how + "+back"
    fn, f = owner(addr)
    fe = fnev(f) if f else (False, False)
    ev = resolve(b, prov) if prov["kind"] == "load16" else dict(arr=None, objoff=None, stride=None, absimm=None, addoff=None)
    sib = siblings(b, prov) if prov["kind"] == "load16" else set()
    d = prov.get("disp")
    if addr in OVERRIDE:
        k, r = OVERRIDE[addr]
        cls, why = ("A_PATHNODE" if k == "PATHNODE" else "B_ADJUDICATED_NOT"), "hand-adjudicated: " + r
    elif prov["kind"] == "nofild":
        cls, why = "B_FLOAT", "operand is already a float (%s)" % prov["detail"]
    elif prov["kind"] == "other":
        cls, why = "B_OTHERSRC", "integer source: " + prov["detail"]
    elif prov["kind"] == "unclear":
        cls, why = "C_UNCLEAR", prov["detail"]
    else:
        hard_pn = ev["arr"] in PN_ARR or ev["objoff"] in PN_OFF or ev["addoff"] in (0x804, 0x904) or ev["stride"] == 0x1C
        hard_cl = ev["arr"] in CL_ARR or ev["objoff"] in CL_OFF or ev["addoff"] in (0x924, 0xA24) or ev["stride"] == 0xE
        # CPathNode's first 8 bytes are two pointers, never read as words. Any sibling
        # 16-bit load at +0/+2/+4/+6 off the same base means a different struct.
        bad_sib = bool(sib & {0, 2, 4, 6})
        if d in COORD and hard_pn:
            cls, why = "A_PATHNODE", "node+%#x; base from path node array (arr=%s objoff=%s add=%s stride=%s)" % (
                d, hex(ev["arr"]) if ev["arr"] else None, hex(ev["objoff"]) if ev["objoff"] else None,
                hex(ev["addoff"]) if ev["addoff"] else None, ev["stride"])
        elif d in (0, 2) and hard_cl:
            cls, why = "A_CARPATHLINK", "link+%#x; base from car-path-link array" % d
        elif d in COORD and ev["absimm"]:
            cls, why = "B_FIXEDGLOBAL", "+%#x off fixed global %#x" % (d, ev["absimm"])
        elif d in COORD and bad_sib:
            cls, why = "B_WRONGSTRUCT", "+%#x but sibling loads on the same base touch %s - not the CPathNode triple" % (
                d, sorted(hex(x) for x in sib))
        elif d in COORD and fe[0]:
            cls, why = "A2_PATHNODE", "node+%#x; base unresolved, sibling set %s consistent, function touches path node array" % (
                d, sorted(hex(x) for x in sib))
        elif d in (0, 2) and (fe[1] or fe[0]) and not (sib - LINKSET):
            cls, why = "A2_CARPATHLINK", "link+%#x; base unresolved, sibling set %s, function touches path arrays" % (
                d, sorted(hex(x) for x in sib))
        elif d in COORD:
            cls, why = "B_NOTPATHFN", "+%#x but function never touches the path arrays" % d
        elif d in (0x10, 0x12, 0x14):
            cls, why = "B_PICKUP", "CPickup +%#x" % d
        else:
            cls, why = "B_OTHEROFF", "16-bit load at +%#x" % d
    sites.append(dict(addr=addr, mn=mn, ops=ops, fn=fn, off=addr-f, how=how, kind=prov["kind"],
                      disp=d, sib=sorted(sib), ev={k: (hex(v) if isinstance(v, int) and k != "stride" else v)
                      for k, v in ev.items()}, fnev=fe, cls=cls, why=why,
                      ctx=["%08X  %-6s %s" % t for t in b[max(0,i-8):i+1]]))
if OUT_JSON:
    json.dump(sites, open(OUT_JSON, "w"), indent=1, default=str)

print("=== %d instructions reference the shared 0.125 at 0x858C48 ===\n" % len(sites))
for k, v in Counter(s["cls"] for s in sites).most_common():
    print("  %-20s %d" % (k, v))

PN = [s for s in sites if s["cls"] in ("A_PATHNODE", "A2_PATHNODE")]
CL = [s for s in sites if s["cls"] in ("A_CARPATHLINK", "A2_CARPATHLINK")]
print("\n--- CPathNode coordinate decodes: %d ---" % len(PN))
bf = defaultdict(list)
for s in PN: bf[s["fn"]].append(s)
for fn in sorted(bf, key=lambda x: bf[x][0]["addr"]):
    print("  %-56s %d" % (fn, len(bf[fn])))
print("\n--- CCarPathLink coordinate decodes: %d ---" % len(CL))
bf = defaultdict(list)
for s in CL: bf[s["fn"]].append(s)
for fn in sorted(bf, key=lambda x: bf[x][0]["addr"]):
    print("  %-56s %d" % (fn, len(bf[fn])))

HEADER = """\
Every instruction in gta_sa.exe (1.0 US HOODLUM) that decodes a compressed PATH position through
the shared 0.125 at 0x858C48, written by tools/scan_path_scale_sites.py. Read that script's
docstring first; it says what the classes mean and why a pattern match alone is not enough.

This is a measurement, not a patch list in use. Nothing in the plugin repoints any of these yet.
It exists because "widening the path encoding is too risky to attempt blind" was the right call
without it and is a different question with it: every one of the %d confident sites is the same
six bytes, D8 0D 48 8C 85 00, an fmul with its disp32 at instruction offset +2, so each change is
mechanically "expect those bytes at A, write the new address into the last four" and every one is
checkable against the file before and after with no game running.

What a widening would have to change, all together or not at all:
    %3d  CPathNode coordinate decodes   (listed first below)
    %3d  CCarPathLink coordinate decodes (listed second - the OTHER compressed path struct, 14
         bytes with its coordinates at +0 and +2; miss these and traffic drives to the wrong
         places while pedestrian routing looks correct)
      3  encode sites, all in CPathFind::CompleteNewInterior, through 8.0 at 0x859000
      1  load-time rescale of San Andreas' own shipped nodes. CPathFind::LoadPathFindData copies
         the 28-byte records out of nodes<N>.dat verbatim with no transform, so one hook after
         the read - the plain call at 0x452F75, not the HOODLUM-stolen entry at 0x4529F0 - can
         rescale every one of them without editing a data file
      0  region constants. An earlier note said the four at 0x859A8C/90/94/98 (1/750, -3000, 3000,
         750) had to move with the scale. They do not, and moving them would be a mistake.
         CPathFind::FindRegionForCoors maps a world FLOAT to one of 64 areas; nothing about it
         depends on how a node's position is compressed. What it does decide is which of the 64
         nodes<N>.dat a square of ground belongs to - and San Andreas' own 64 files are filed by
         exactly these boundaries, so redefining them would mean redistributing every shipped node
         across a new grid, with cross-area link fixups, at load time. Left alone, a region's
         ground simply clamps into the edge areas (Vice City into column 7, Liberty City into row
         7), which is survivable for the reason the original finding gave: nothing validates a
         node against its area's box. The only readers of the inverse helpers
         FindXCoorsForRegion/FindYCoorsForRegion are one call each in
         CCarCtrl::FindNodesThisCarIsNearestTo, and FindStartPointOfRegion has no callers at all.

And what it would still not settle: %d sites are honestly unclear (their feeding store is in
another basic block), and the A2 evidence is inference from struct-offset sets and function
behaviour rather than a proven pointer chain. A missed decode reads a widened node through the old
0.125 and silently produces a position four times too small. Only running the game answers that.

"""
with open(OUT_TXT, "w") as fh:
    fh.write(HEADER % (len(PN) + len(CL), len(PN), len(CL),
                       sum(1 for s in sites if s["cls"] == "C_UNCLEAR")))
    fh.write("CPathNode coordinate decode sites (fmul dword ptr [0x858C48]) - %d\n" % len(PN))
    fh.write("addr      off  function                                             evidence\n")
    for s in PN:
        fh.write("%08X  +%-4X %-52s %s\n" % (s["addr"], s["disp"], s["fn"] + "+0x%X" % s["off"], s["why"]))
    fh.write("\nCCarPathLink coordinate decode sites - %d\n" % len(CL))
    for s in CL:
        fh.write("%08X  +%-4X %-52s %s\n" % (s["addr"], s["disp"], s["fn"] + "+0x%X" % s["off"], s["why"]))
    for name in ("B_WRONGSTRUCT", "B_FIXEDGLOBAL", "B_NOTPATHFN", "B_ADJUDICATED_NOT", "C_UNCLEAR"):
        g = [s for s in sites if s["cls"] == name]
        fh.write("\n%s - %d\n" % (name, len(g)))
        for s in g:
            fh.write("%08X  %-52s %s\n" % (s["addr"], s["fn"] + "+0x%X" % s["off"], s["why"]))

print("\n--- pattern matched at +8/+A/+C but rejected ---")
for name in ("B_WRONGSTRUCT", "B_FIXEDGLOBAL", "B_NOTPATHFN", "B_ADJUDICATED_NOT"):
    for s in sites:
        if s["cls"] == name:
            print("  %08X  %-50s %s" % (s["addr"], s["fn"] + "+0x%X" % s["off"], s["why"]))
print("\n--- C_UNCLEAR ---")
for s in sites:
    if s["cls"] == "C_UNCLEAR":
        print("  %08X  %-50s %s" % (s["addr"], s["fn"] + "+0x%X" % s["off"], s["why"]))
