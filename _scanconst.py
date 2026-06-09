#!/usr/bin/env python3
"""Find where a 32-bit constant is materialized via lis + addi/ori in EBOOT.elf."""
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

target = int(sys.argv[1], 16)
hi = (target >> 16) & 0xFFFF
lo = target & 0xFFFF
# account for addi sign-extension: if lo >= 0x8000, hi must be hi+1 in the lis
hi_adj = (hi + 1) & 0xFFFF if lo >= 0x8000 else hi

hits = 0
for po, pv, pf in segs:
    # track last lis per register
    last_lis = {}  # reg -> (imm, vaddr)
    for off in range(0, pf - 3, 4):
        raw = struct.unpack_from(endi+"I", data, po+off)[0]
        va = pv + off
        opc = raw >> 26
        rt = (raw >> 21) & 0x1F
        ra = (raw >> 16) & 0x1F
        imm = raw & 0xFFFF
        if opc == 15 and ra == 0:  # addis/lis rT, imm
            last_lis[rt] = (imm, va)
        elif opc == 14:  # addi rT, rA, imm
            if imm == lo and ra in last_lis and last_lis[ra][0] in (hi, hi_adj):
                print(f"0x{va:08X}: addi (after lis@0x{last_lis[ra][1]:08X}) materializes 0x{target:08X}")
                hits += 1
        elif opc == 24:  # ori rA, rS, imm
            if imm == lo and rt in last_lis and last_lis[rt][0] == hi:
                print(f"0x{va:08X}: ori (after lis@0x{last_lis[rt][1]:08X}) materializes 0x{target:08X}")
                hits += 1
print(f"total: {hits}")
