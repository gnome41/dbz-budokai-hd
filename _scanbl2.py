#!/usr/bin/env python3
"""Find every `bl`/`b` instruction targeting given addresses in EBOOT.elf LOAD segs."""
import struct, sys
ELF = r"E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf"
data = open(ELF, "rb").read()
endi = ">"
e_phoff = struct.unpack_from(endi+"Q", data, 0x20)[0]
e_phentsize = struct.unpack_from(endi+"H", data, 0x36)[0]
e_phnum = struct.unpack_from(endi+"H", data, 0x38)[0]
segs = []
for i in range(e_phnum):
    ph = e_phoff + i*e_phentsize
    pt = struct.unpack_from(endi+"I", data, ph)[0]
    po = struct.unpack_from(endi+"Q", data, ph+0x08)[0]
    pv = struct.unpack_from(endi+"Q", data, ph+0x10)[0]
    pf = struct.unpack_from(endi+"Q", data, ph+0x20)[0]
    if pt == 1: segs.append((po, pv, pf))

targets = set(int(x, 16) for x in sys.argv[1:])
for po, pv, pf in segs:
    for off in range(0, pf - 3, 4):
        raw = struct.unpack_from(endi+"I", data, po+off)[0]
        opc = raw >> 26
        if opc != 18:  # b / bl / ba / bla
            continue
        li = raw & 0x03FFFFFC
        if li & 0x02000000:  # sign extend 26-bit
            li -= 0x04000000
        aa = (raw >> 1) & 1
        lk = raw & 1
        va = pv + off
        tgt = (li if aa else va + li) & 0xFFFFFFFF
        if tgt in targets:
            kind = ("bl" if lk else "b") + ("a" if aa else "")
            print(f"  0x{va:08X}: {kind} -> 0x{tgt:08X}")
