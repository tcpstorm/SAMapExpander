#!/usr/bin/env python3
"""Resolve every new weapon type through every widened switch and assert it matches its donor.

This is the check that would have found a bad table entry without anybody starting the game, and it
was written because a whole round was spent looking for one that was not there. It reads the
Dispatch table out of src/WeaponTypes.cpp and the donor pairing out of tools/import_city.py,
simulates WidenDispatch and RouteDispatch exactly as the plugin performs them, and compares the
address each new type resolves to against the address its donor resolves to through San Andreas'
own untouched tables. The plugin makes the same comparison at load time and logs an error if it
fails, so this must pass before shipping.

It also decodes what CPed::UpdateCameraWeaponModes pushes as the camera mode for each weapon, and
reads CTaskSimplePlayerOnFoot::ProcessPlayerWeapon's chain - the SECOND place the camera mode is
decided, which overwrites the first every frame and is where the .308 was losing its scope.

Usage: verify_weapon_types.py <gta_sa.exe> [src dir] [tools dir]
"""
import re
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from scan_exe import Pe  # noqa: E402

_exe = sys.argv[1] if len(sys.argv) > 1 else "gta_sa.exe"
_src = sys.argv[2] if len(sys.argv) > 2 else "src"
_tools = sys.argv[3] if len(sys.argv) > 3 else "tools"

pe = Pe(open(_exe, 'rb').read())
src = open(_src + '/WeaponTypes.cpp', encoding='latin-1').read()
imp = open(_tools + '/import_city.py', encoding='latin-1').read()


def u32(addr):
    return struct.unpack("<I", pe.read(addr, 4))[0]


def u8(addr):
    return pe.read(addr, 1)[0]


FIRST_NEW, LAST_NEW = 70, 79

# The San Andreas weapon names, for turning a donor name into a type number.
NAMES = ['UNARMED', 'BRASSKNUCKLE', 'GOLFCLUB', 'NIGHTSTICK', 'KNIFE', 'BASEBALLBAT', 'SHOVEL',
         'POOLCUE', 'KATANA', 'CHAINSAW', 'DILDO1', 'DILDO2', 'VIBE1', 'VIBE2', 'FLOWERS', 'CANE',
         'GRENADE', 'TEARGAS', 'MOLOTOV', 'ROCKET', 'ROCKET_HS', 'FREEFALL_BOMB', 'PISTOL',
         'PISTOL_SILENCED', 'DESERT_EAGLE', 'SHOTGUN', 'SAWNOFF', 'SPAS12', 'MICRO_UZI', 'MP5',
         'AK47', 'M4', 'TEC9', 'COUNTRYRIFLE', 'SNIPERRIFLE', 'RLAUNCHER', 'RLAUNCHER_HS',
         'FTHROWER', 'MINIGUN', 'SATCHEL_CHARGE', 'DETONATOR', 'SPRAYCAN', 'EXTINGUISHER',
         'CAMERA', 'NIGHTVISION', 'INFRARED', 'PARACHUTE']

body = src.split('const Dispatch kDispatch[] = {', 1)[1].split('\n};', 1)[0]
entries = re.findall(
    r'\{\s*"([^"]*)",\s*'
    r'(0x[0-9A-Fa-f]+),\s*\{[^}]*\},\s*(0x[0-9A-Fa-f]+),\s*'
    r'(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*'
    r'(0x[0-9A-Fa-f]+|0),\s*\{[^}]*\},\s*(0x[0-9A-Fa-f]+|0),\s*'
    r'(0x[0-9A-Fa-f]+),\s*\{[^}]*\},\s*(0x[0-9A-Fa-f]+),\s*(\d+)\s*\}', body)

dispatches = []
for what, cmpAt, bound, jaAt, dflt, idxAt, idxTab, jmpAt, jmpTab, first in entries:
    dispatches.append(dict(what=what, bound=int(bound, 0), default=int(dflt, 0),
                           idxAt=int(idxAt, 0), idxTab=int(idxTab, 0),
                           jmpTab=int(jmpTab, 0), first=int(first)))
print('dispatches read from WeaponTypes.cpp: %d' % len(dispatches))

# The donor pairing, out of the importer's own table.
pairs = re.findall(r'\("([A-Za-z0-9]+)",\s*"([A-Z0-9_]+)",\s*(True|False),', imp)
live = [(n, d) for n, d, l in pairs if l == 'True']
print('live weapons in import_city.py: %s' % ', '.join('%s/%s' % p for p in live))


def vanilla_target(d, wtype):
    i = wtype - d['first']
    if i < 0 or i > d['bound']:
        return d['default']
    if d['idxAt']:
        case = u8(d['idxTab'] + i)
        return u32(d['jmpTab'] + 4 * case)
    return u32(d['jmpTab'] + 4 * i)


def widened_target(d, wtype, donor):
    """What WeaponTypes.cpp's RouteDispatch + WidenDispatch make the CPU resolve for `wtype`."""
    span = LAST_NEW - d['first'] + 1
    i = wtype - d['first']
    if i < 0 or i >= span:
        return None                      # outside the widened bound: still `ja` to the default
    old = d['bound'] + 1
    if d['idxAt']:
        cases = 1 + max(u8(d['idxTab'] + k) for k in range(old))
        jump = [u32(d['jmpTab'] + 4 * c) for c in range(cases)] + [d['default']]
        index = [u8(d['idxTab'] + k) for k in range(old)] + [cases] * (span - old)
        j = donor - d['first']
        index[i] = u8(d['idxTab'] + j) if 0 <= j <= d['bound'] else cases
        return jump[index[i]]
    jump = [u32(d['jmpTab'] + 4 * k) for k in range(old)] + [d['default']] * (span - old)
    j = donor - d['first']
    jump[i] = u32(d['jmpTab'] + 4 * j) if 0 <= j <= d['bound'] else d['default']
    return jump[i]


def camera_mode_at(target):
    """Each case of UpdateCameraWeaponModes is `push 0; push 0; push mode`, or the clear."""
    if target == 0x6098B6:
        return 'cleared'
    b = pe.read(target, 6)
    if b[0] == 0x6A and b[2] == 0x6A and b[4] == 0x6A:
        return '0x%X' % b[5]
    return '?'


bad = 0
for n, (name, donor_name) in enumerate(live):
    donor = NAMES.index(donor_name)
    wtype = FIRST_NEW + n
    print('\n%-14s type %d, donor %s (%d)' % (name, wtype, donor_name, donor))
    for d in dispatches:
        ours = widened_target(d, wtype, donor)
        theirs = vanilla_target(d, donor)
        ok = ours == theirs
        extra = ''
        if 'UpdateCameraWeaponModes' in d['what']:
            extra = '   camera mode %s (donor %s)' % (camera_mode_at(ours), camera_mode_at(theirs))
        print('   %-45s %-4s 0x%X vs 0x%X%s' % (d['what'], 'ok' if ok else 'BAD', ours, theirs, extra))
        if not ok:
            bad += 1

print('\n%s' % ('every new type resolves exactly where its donor does'
                if not bad else '%d MISMATCH(ES)' % bad))

# The second place the camera mode is decided.
print('\n=== CTaskSimplePlayerOnFoot::ProcessPlayerWeapon, the chain that overwrites it ===')
ARMS = [(0x685B4C, 0x685B55), (0x685B59, 0x685B7A), (0x685B7E, 0x685B83), (0x685B87, 0x685B8C)]
chain = {}
okchain = pe.read(0x685B45, 7) == bytes.fromhex('8b8432a0050000')
print('   reads the ped weapon type at 0x685B45: %s' % ('ok' if okchain else 'BAD'))
for cmpAt, pushAt in ARMS:
    c, p = pe.read(cmpAt, 3), pe.read(pushAt, 2)
    good = c[0] == 0x83 and c[1] == 0xF8 and p[0] == 0x6A
    chain[c[2]] = p[1]
    print('   0x%X cmp eax, 0x%-3X -> 0x%X push 0x%-3X  %s'
          % (cmpAt, c[2], pushAt, p[1], 'ok' if good else 'BAD'))
dflt = pe.read(0x685B90, 2)
print('   0x685B90 default        push 0x%X  %s' % (dflt[1], 'ok' if dflt[0] == 0x6A else 'BAD'))
for n, (name, donor_name) in enumerate(live):
    donor = NAMES.index(donor_name)
    want = chain.get(donor, dflt[1])
    print('   %-14s donor %-14s chain gives 0x%X%s'
          % (name, donor_name, want, '' if want == dflt[1] else '   <- the stand-in must supply this'))
