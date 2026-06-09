#!/usr/bin/env python3
"""Implement stfiwx (op31 xo=983) — store the low 32 bits of FPR[frS] to mem[(rA?gpr[rA]:0)+gpr[rB]].
The lifter left it a `/* TODO */` no-op ("reverted -- causes crash in SPURS init"), but that was
BEFORE the 64-bit add + subfc fixes; the crash was a downstream symptom of those.  Without stfiwx
the float-to-string formatter never stores the extracted integer digit -> infinite loop
(func_000E2A3C spin). Operand comment form: `op31_x983 r<frS>, r<rA>, r<rB>`."""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
t = open(CPP, encoding="utf-8", errors="replace").read()
def repl(m):
    frs, ra, rb = m.group(1), m.group(2), m.group(3)
    ea = "(uint32_t)ctx->gpr[%s]" % rb if ra == "0" else "((uint32_t)ctx->gpr[%s] + (uint32_t)ctx->gpr[%s])" % (ra, rb)
    return ("{ uint64_t _fb; memcpy(&_fb, &ctx->fpr[%s], 8); vm_write32(%s, (uint32_t)_fb); }" % (frs, ea))
pat = re.compile(r"/\* TODO: op31_x983 r(\d+), r(\d+), r(\d+) \*/;(?: /\* stfiwx:[^*]*\*/)?")
t, n = pat.subn(repl, t)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(t)
print("stfiwx implemented:", n)
