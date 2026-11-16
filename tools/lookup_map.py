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
        # MSVC/lld-link style: " 0001:000a1234       ?name@@...    00000001400a2234 f   obj.obj"
        m = re.match(r"\s*([0-9a-fA-F]{4}):[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\b", line)
        if m and m.group(1) == "0000":
            continue    # absolute / linker-defined values, not code
        if m:
            m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\b", line)
            addr = int(m.group(2), 16)
            name = m.group(1)
        else:
            # GNU ld/lld style: "<address> <size> ... <symbol>"
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
    syms.sort()
    if not syms:
        print("no symbols parsed; first lines of the map:")
        for i, line in enumerate(open(path, errors="replace")):
            if i >= 60: break
            print(repr(line.rstrip()))
        lines = open(path, errors="replace").read().splitlines()
        print("... lines around the middle:")
        for line in lines[len(lines)//2:len(lines)//2+25]:
            print(repr(line))
        return
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
