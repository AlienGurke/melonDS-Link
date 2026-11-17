#!/usr/bin/env python3
"""Find the function at a crash offset in a linker map file.

Usage: lookup_map.py <map file> <offset> [<offset> ...]
<offset> is the "Fehleroffset"/"Fault offset" from the Windows event log
(relative to the start of melonDS.exe), e.g. 0x32d7cf.
Works with lld/ld map files (addresses in hex at the start of a line).
"""
import re, sys

def main():
    path = sys.argv[1]
    targets = [int(a, 16) for a in sys.argv[2:]]
    syms = []
    base = None
    for line in open(path, errors="replace"):
        m = re.match(r"\s*([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]+)\s+.*?\s(\S+)\s*$", line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        name = m.group(3)
        if addr == 0:
            continue
        syms.append((addr, line.rstrip()))
        if base is None or addr < base:
            base = addr
    if not syms:
        # MSVC link.exe map: " 0001:00361234  ?name@@...  0000000140362234 f  lib:object.obj"
        for line in open(path, errors="replace"):
            m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$", line)
            if m:
                addr = int(m.group(2), 16)
                if addr:
                    syms.append((addr, m.group(1) + "   " + m.group(3).strip()))
        if syms:
            base = 0x140000000
    syms.sort()
    if not syms:
        print("no symbols parsed"); return
    imagebase = 0x140000000 if base >= 0x140000000 else (0x400000 if base >= 0x400000 else 0)
    print("image base guess: 0x%x, %d entries" % (imagebase, len(syms)))
    for t in targets:
        va = imagebase + t
        before = [s for s in syms if s[0] <= va][-12:]
        print("== offset 0x%x (address 0x%x) ==" % (t, va))
        for a, l in before:
            print(l)
        after = [s for s in syms if s[0] > va][:2]
        print("-- next --")
        for a, l in after:
            print(l)

main()
