#!/usr/bin/env python3
"""Re-read every site src/PickupSlots.cpp patches out of gta_sa.exe and confirm the operand still
holds the value the table expects.

The plugin makes the same check at load time and refuses to write a single byte if one site
disagrees, so this must pass 100% before the table ships. The tables are parsed out of the source
rather than copied here, so the two can never drift apart.

It also re-checks the two things a table cannot say on its own:

  * that every operand really is where the table says it is - the expected dword must appear at
    that offset inside the instruction and NOWHERE else in it, or a wrong operandOffset would
    silently patch the wrong four bytes;
  * that the instructions this deliberately leaves alone - the globals the linker put directly
    behind the array, which share the array's end address - are not in any of the tables.

Usage: verify_pickup_sites.py <gta_sa.exe> [src/PickupSlots.cpp]
"""
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

ARRAY = 0x9788C0
STRIDE = 0x20
VANILLA_COUNT = 620
ARRAY_END = ARRAY + VANILLA_COUNT * STRIDE          # 0x97D640

# Instructions that name 0x97D640 / 0x97D644 as MEMORY rather than as a loop sentinel. These are
# the globals the linker put behind the array; patching one would corrupt game state, so the plugin
# must not have them in any table.
MUST_NOT_TOUCH = {
    0x457ACE: "CPickup::Update    mov dword ptr [0x97D640], 6",
    0x457CC3: "CPickup::Update    mov dword ptr [0x97D640], 6",
    0x458EE1: "CPickups::Update   mov eax, dword ptr [0x97D640]",
    0x458EF5: "CPickups::Update   mov dword ptr [0x97D640], edx",
    0x457BB3: "CPickup::Update    mov eax, dword ptr [0x97D644]",
    0x458181: "CPickup::Update    mov eax, dword ptr [0x97D644]",
    0x458EBF: "CPickups::Update   mov dword ptr [0x97D644], 6",
    0x458ECB: "CPickups::Update   mov eax, dword ptr [0x97D644]",
    0x458EDB: "CPickups::Update   mov dword ptr [0x97D644], ecx",
    0x458F10: "CPickups::Update   mov dword ptr [0x97D644], 0",
}

ROW = re.compile(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*([^},]+?)\s*(?:,\s*(true|false)\s*)?\}")
COUNT_VALUE = {"kCount": VANILLA_COUNT,
               "kCountLessOne": VANILLA_COUNT - 1,
               "kCountBytes": VANILLA_COUNT * STRIDE}


def table(text, name):
    """The rows of `const <T> name[] = { ... };`."""
    start = text.index(name + "[] = {")
    end = text.index("};", start)
    return ROW.findall(text[start:end])


def main():
    pe = Pe(open(sys.argv[1], "rb").read())
    src = open(sys.argv[2] if len(sys.argv) > 2 else "src/PickupSlots.cpp").read()

    sites = []   # (address, operandOffset, expected, what)
    for addr, off, value, _flag in table(src, "kBaseSites"):
        sites.append((int(addr, 16), int(off), int(value, 16), "base"))
    for addr, off, field, flag in table(src, "kEndSites"):
        sites.append((int(addr, 16), int(off), ARRAY_END + int(field, 16),
                      "end" + (" (save path)" if flag == "true" else "")))
    for addr, off, kind, _flag in table(src, "kCountSites"):
        sites.append((int(addr, 16), int(off), COUNT_VALUE[kind.strip()], "count " + kind.strip()))
    # The one operand naming the last entry rather than the first.
    last = dict(re.findall(r"constexpr \w+ (kLastEntry\w*)\s*=\s*([^;]+);", src))
    sites.append((int(last["kLastEntrySite"], 16), int(last["kLastEntryOperandOffset"]),
                  int(last["kLastEntryOldValue"], 16), "last entry"))

    ok = bad = 0
    seen = set()
    for addr, off, want, what in sites:
        if addr in seen:
            print("DUPLICATE 0x%X" % addr)
            bad += 1
        seen.add(addr)
        raw = pe.read(addr + off, 4)
        got = struct.unpack("<I", raw)[0] if raw and len(raw) == 4 else None
        if got != want:
            print("BAD  0x%X +%d  %-22s got 0x%08X want 0x%08X" % (addr, off, what,
                                                                   got or 0, want))
            bad += 1
            continue
        # The operand must be at that offset and only there, inside the twelve bytes the
        # instruction can occupy - otherwise the offset is guesswork.
        window = pe.read(addr, 12)
        where = [i for i in range(9) if struct.unpack_from("<I", window, i)[0] == want]
        if where != [off]:
            print("AMBIGUOUS 0x%X  %-22s 0x%08X appears at offsets %s, table says %d"
                  % (addr, what, want, where, off))
            bad += 1
            continue
        ok += 1

    for addr, why in sorted(MUST_NOT_TOUCH.items()):
        if addr in seen:
            print("PATCHES A GLOBAL  0x%X  %s" % (addr, why))
            bad += 1

    # And prove the arithmetic the plugin will do, for a pretend base and a pretend count: what
    # matters is that every loop still spans the right number of entries afterwards.
    base, count = 0x10000000, 980
    def base_new(old):
        return base + (old - ARRAY)
    def end_new(field, save):
        return base + (VANILLA_COUNT if save else count) * STRIDE + field

    spans = {
        "CPickups::Save":  (base_new(0x9788C0), end_new(0x00, True),  VANILLA_COUNT),
        "CPickups::Load":  (base_new(0x9788C4), end_new(0x04, True),  VANILLA_COUNT),
        "CPickups::ReInit":(base_new(0x9788C4), end_new(0x04, False), count),
        "CPickups::Init":  (base_new(0x9788DA), end_new(0x1A, False), count),
        "DetonateMines":   (base_new(0x9788C0), end_new(0x00, False), count),
        "PassTime":        (base_new(0x9788CC), end_new(0x0C, False), count),
        "GenerateNewOne":  (base_new(0x9788FC) - STRIDE, end_new(0x3C, False) - STRIDE, count),
    }
    for name, (start, end, want) in spans.items():
        got = (end - start) // STRIDE
        if got != want:
            print("SPAN  %s walks %d entries, expected %d" % (name, got, want))
            bad += 1
    if count % 10:
        print("SPAN  the count %d is not a multiple of ten; GenerateNewOne's unrolled search "
              "would read past the last entry" % count)
        bad += 1

    print("\n%d site(s) verified, %d bad" % (ok, bad))
    print("%d of them are the save path, which keeps the vanilla count of %d"
          % (sum(1 for s in sites if "save path" in s[3]) +
             sum(1 for s in sites if s[0] in (0x5D3542, 0x5D35A2)), VANILLA_COUNT))
    print("with a %d entry array at 0x%X, Save and Load still walk %d entries each"
          % (count, base, (end_new(0x00, True) - base_new(0x9788C0)) // STRIDE))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
