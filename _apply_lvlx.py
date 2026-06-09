#!/usr/bin/env python3
"""Implement the Cell-BE unaligned-vector-load pair left as `/* TODO */` no-ops:
  op31_x519 = lvlx  vrD, rA, rB  (Load Vector Left Indexed)   341 sites
  op31_x551 = lvrx  vrD, rA, rB  (Load Vector Right Indexed)   17 sites
Together with `vor` these load 16 unaligned bytes (lvlx(p) | lvrx(p+16)).
Leaving them no-ops feeds garbage into vector string/number formatting -> the
func_000E2A3C dtoa convergence spin.

Convention matches the existing aligned lvx/stvx in this file: ctx->vr[N] holds
raw guest bytes in big-endian order (byte 0 = lowest guest address = MSB), copied
straight from vm_base with no byte swap.

EA = (rA|0) + rB  (rA field == 0 means literal 0, not gpr[0]).
  lvlx: eb = EA&0xF; load 16-eb bytes MEM[EA .. EA|0xF] left-justified (vr[0]=MEM[EA]),
        low eb bytes = 0.
  lvrx: eb = EA&0xF; load eb bytes MEM[EA-eb .. EA) right-justified (vr[15]=MEM[EA-1]),
        high 16-eb bytes = 0.
"""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
t = open(CPP, encoding="utf-8", errors="replace").read()

def ea_expr(ra, rb):
    if ra == "0":
        return "(uint32_t)ctx->gpr[%s]" % rb
    return "((uint32_t)ctx->gpr[%s] + (uint32_t)ctx->gpr[%s])" % (ra, rb)

def lvlx(m):
    d, ra, rb = m.group(1), m.group(2), m.group(3)
    return ("{ uint32_t _ea = %s; uint32_t _eb = _ea & 0xFu; uint8_t _t[16] = {0}; "
            "for(uint32_t _i=0;_i<16u-_eb;_i++) _t[_i]=vm_base[_ea+_i]; "
            "memcpy(&ctx->vr[%s], _t, 16); }" % (ea_expr(ra, rb), d))

def lvrx(m):
    d, ra, rb = m.group(1), m.group(2), m.group(3)
    return ("{ uint32_t _ea = %s; uint32_t _eb = _ea & 0xFu; uint8_t _t[16] = {0}; "
            "for(uint32_t _i=0;_i<_eb;_i++) _t[16u-_eb+_i]=vm_base[(_ea-_eb)+_i]; "
            "memcpy(&ctx->vr[%s], _t, 16); }" % (ea_expr(ra, rb), d))

t, n1 = re.subn(r"/\* TODO: op31_x519 r(\d+), r(\d+), r(\d+) \*/;", lvlx, t)
t, n2 = re.subn(r"/\* TODO: op31_x551 r(\d+), r(\d+), r(\d+) \*/;", lvrx, t)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(t)
print("lvlx implemented:", n1, " lvrx implemented:", n2)
