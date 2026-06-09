#!/usr/bin/env python3
"""Find off-by-4 dropped-stdu stub pairs: lifted stub at A returning a constant, real
function at A+4, where ELF insn(A) is `stdu/stwu r1,d(r1)` and insn(A+4) is `mflr r0`.
These are lifter-dropped prologues — the OPD/bl points to A but the body starts at A+4."""
import struct, re, sys
ELF = r"E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf"
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
data = open(ELF, "rb").read()
endi = ">"
e_phoff = struct.unpack_from(endi+"Q", data, 0x20)[0]
e_phnum = struct.unpack_from(endi+"H", data, 0x38)[0]
e_phentsize = struct.unpack_from(endi+"H", data, 0x36)[0]
segs = []
for i in range(e_phnum):
    ph = e_phoff + i*e_phentsize
    pt = struct.unpack_from(endi+"I", data, ph)[0]
    po = struct.unpack_from(endi+"Q", data, ph+0x08)[0]
    pv = struct.unpack_from(endi+"Q", data, ph+0x10)[0]
    pf = struct.unpack_from(endi+"Q", data, ph+0x20)[0]
    if pt == 1: segs.append((po, pv, pf))
def insn(va):
    for po, pv, pf in segs:
        if pv <= va < pv+pf: return struct.unpack_from(endi+"I", data, po+(va-pv))[0]
    return None
def is_stwdu_r1(w):
    if w is None: return None
    op = w >> 26
    rs = (w >> 21) & 0x1F; ra = (w >> 16) & 0x1F
    if rs != 1 or ra != 1: return None
    if op == 37:  # stwu rS,d(rA)
        d = w & 0xFFFF
        if d & 0x8000: d -= 0x10000
        return ("stwu", d)
    if op == 62 and (w & 3) == 1:  # stdu rS,ds(rA)
        d = w & 0xFFFC
        if d & 0x8000: d -= 0x10000
        return ("stdu", d)
    return None
MFLR = 0x7C0802A6
text = open(CPP, encoding="utf-8", errors="replace").read()
# all lifted function entry addresses
defined = set(int(m, 16) for m in re.findall(r"func_000([0-9A-Fa-f]{5,6})\(ppu_context", text))
# stub one-liners returning a constant
stub_re = re.compile(r"(?:static )?void func_000([0-9A-Fa-f]{5,6})\(ppu_context\* ctx\) \{ ctx->gpr\[3\] = (\w+); \}")
pairs = []
for m in stub_re.finditer(text):
    a = int(m.group(1), 16)
    ret = m.group(2)
    if (a+4) in defined:
        pr = is_stwdu_r1(insn(a)); nxt = insn(a+4)
        if pr and nxt == MFLR:
            pairs.append((a, ret, pr[0], pr[1]))
pairs.sort()
print(f"found {len(pairs)} off-by-4 dropped-stdu stub pairs:")
for a, ret, kind, d in pairs:
    print(f"  func_000{a:05X} (stub ret {ret}) -> real func_000{a+4:05X}  [{kind} r1,{d}(r1)  size 0x{-d:X}]")
