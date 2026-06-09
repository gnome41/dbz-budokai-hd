#!/usr/bin/env python3
"""Fix the rA|0 rule for vector indexed loads/stores (lvx/stvx), 2364 sites.

The lifter emits the effective address of lvx/stvx as
    uint64_t ea = (ctx->gpr[rA] + ctx->gpr[rB]) & ~0xFULL;
but for X-form indexed ops, rA field == 0 means the LITERAL value 0, NOT gpr[0]
(r0 can never be a base register here).  So whenever rA==0 the lifter wrongly adds
gpr[0]: EA should be just gpr[rB].  This is latent while gpr[0]==0 (common, since
r0 is a scratch/zero reg) but corrupts the address when r0 holds a value — e.g.
func_00017ED4 does `addi r0,r1,0x290` then `stvx v31,0,r0`; the buggy
(gpr[0]+gpr[0]) doubles sp+0x290 (~0xDFFFF9C0) to 0xBFFFF380 -> AV in the C++
unwind path.

Fix: drop the bogus gpr[0] addend so EA = gpr[rB] & ~0xF.  Only the lvx/stvx EA
pattern (`& ~0xFULL`) is matched, so ordinary loads are untouched; non-zero rA
forms (first operand gpr[1], gpr[3], ...) are not matched either.
"""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
t = open(CPP, encoding="utf-8", errors="replace").read()
pat = re.compile(r"uint64_t ea = \(ctx->gpr\[0\] \+ (ctx->gpr\[\d+\])\) & ~0xFULL;")
t, n = pat.subn(r"uint64_t ea = \1 & ~0xFULL;", t)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(t)
print("vector rA|0 fixed:", n)
