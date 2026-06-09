#!/usr/bin/env python3
"""Find every 4-byte BE occurrence of given pointer values in EBOOT.elf LOAD segments."""
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

targets = [int(x, 16) for x in sys.argv[1:]]
for t in targets:
    print(f"=== searching for 0x{t:08X} ===")
    tb = struct.pack(endi+"I", t)
    for po, pv, pf in segs:
        seg = data[po:po+pf]
        start = 0
        while True:
            idx = seg.find(tb, start)
            if idx < 0: break
            if idx % 4 == 0:
                print(f"  found at vaddr 0x{pv+idx:08X}")
            start = idx + 1
