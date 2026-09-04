#!/usr/bin/env python3
"""Generate tools/sa_functions.txt (GTA SA 1.0 US "0xADDR Class::Func" table).

Sources: gta-reversed-modern (RH_Scoped* hook registrations + "// 0xADDR" function comments)
and plugin-sdk (plugin_sa/game_sa "Converted from ... 0xADDR" comments and Call<0xADDR> bodies).

Usage: gen_functions.py <gta-reversed dir> <plugin-sdk dir> <output file>
"""
import os
import re
import sys

table = {}


def add(addr, name):
    if 0x401000 <= addr < 0x900000 and addr not in table:
        table[addr] = name


HEX = r"0x([0-9A-Fa-f]{6,7})"
RE_CLASS = re.compile(r"RH_Scoped(?:Virtual|Named)?Class\(\s*([\w:]+)")
RE_CATEGORY = re.compile(r'RH_ScopedCategory(?:Global)?\(\s*"?([\w:/ ]+?)"?\s*\)')
RE_NAMESPACE = re.compile(r'RH_ScopedNamespace(?:Name)?\(\s*"?([\w:]+)"?\s*\)')
RE_INSTALL = re.compile(
    r"RH_Scoped(?P<kind>Global|VMT|Virtual|Named|NamedGlobal)?(?:Overloaded)?Install\(\s*(?P<name>[\w~]+)\s*,"
    r"(?P<rest>[^;]*)"
)
RE_ADDR_COMMENT = re.compile(r"^\s*//\s*" + HEX + r"\b")
RE_QUALIFIED_DEF = re.compile(r"^[^/;=]*?\b((?:[\w~]+::)+[\w~]+)\s*\(")
RE_FREE_DEF = re.compile(r"^(?:[\w:<>*&]+\s+)+\*?([\w~]+)\s*\(")
RE_CONVERTED = re.compile(r"Converted from\s+\w+\s+(.+?)\s*\(.*?\)\s*" + HEX)
RE_SDK_DEF = re.compile(r"^[\w:<>*&~ ]*?\b((?:[\w~]+::)+[\w~]+)\s*\(")
RE_SDK_ADDR = re.compile(r"(?:Call\w*<|\)\s*)" + HEX)


def parse_reversed(root):
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(dirpath, fn)
            stem = os.path.splitext(fn)[0]
            try:
                lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
            except OSError:
                continue
            scope = stem
            for i, line in enumerate(lines):
                m = RE_CLASS.search(line)
                if m:
                    scope = m.group(1)
                m = RE_CATEGORY.search(line)
                if m:
                    scope = m.group(1).strip()
                m = RE_NAMESPACE.search(line)
                if m:
                    scope = m.group(1)
                m = RE_INSTALL.search(line)
                if m:
                    am = re.search(HEX, m.group("rest"))
                    if am:
                        kind = m.group("kind") or ""
                        name = m.group("name")
                        qual = name if "Global" in kind else f"{scope}::{name}"
                        add(int(am.group(1), 16), qual)
                m = RE_ADDR_COMMENT.match(line)
                if m:
                    addr = int(m.group(1), 16)
                    j = i + 1
                    while j < len(lines) and (not lines[j].strip() or lines[j].strip().startswith("//")):
                        j += 1
                    if j < len(lines):
                        q = RE_QUALIFIED_DEF.match(lines[j])
                        if q:
                            add(addr, q.group(1))
                        else:
                            f = RE_FREE_DEF.match(lines[j])
                            if f and f.group(1) not in ("if", "for", "while", "switch", "return"):
                                add(addr, f"{stem}::{f.group(1)}")


def parse_sdk(root):
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
            except OSError:
                continue
            current = None
            for line in lines:
                m = RE_CONVERTED.search(line)
                if m:
                    add(int(m.group(2), 16), m.group(1).split()[-1])
                    continue
                if not line.startswith((" ", "\t", "//", "}")):
                    d = RE_SDK_DEF.match(line)
                    if d:
                        current = d.group(1)
                if current:
                    for am in RE_SDK_ADDR.finditer(line):
                        add(int(am.group(1), 16), current)


def main():
    reversed_dir, sdk_dir, out = sys.argv[1:4]
    parse_reversed(reversed_dir)
    parse_sdk(sdk_dir)
    with open(out, "w", newline="\n") as f:
        for addr in sorted(table):
            f.write(f"0x{addr:X} {table[addr]}\n")
    print(f"{len(table)} functions -> {out}")


if __name__ == "__main__":
    main()
