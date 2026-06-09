#!/usr/bin/env python3
"""Apply the upstream lifter's 64-bit add/subf correctness fix to our existing generated
recompiled/ppu_recomp.cpp, in place.  PPC add/subf are full 64-bit ops; the old lifter
truncated to 32-bit ((int64_t)(int32_t)((uint32_t)a +/- (uint32_t)b)), which breaks genuine
64-bit arithmetic (e.g. the SWAR word-at-a-time strlen scans forever).  Replace the reg-reg
add/subf pattern with the full 64-bit form the new lifter emits.  Low-32 result is identical
for <4GB guests, so this is safe and strictly more correct."""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
text = open(CPP, encoding="utf-8", errors="replace").read()

add_re = re.compile(
    r"ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)ctx->gpr\[(\d+)\] \+ \(uint32_t\)ctx->gpr\[(\d+)\]\);")
subf_re = re.compile(
    r"ctx->gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)ctx->gpr\[(\d+)\] - \(uint32_t\)ctx->gpr\[(\d+)\]\);")

text, n_add = add_re.subn(r"ctx->gpr[\1] = ctx->gpr[\2] + ctx->gpr[\3];", text)
text, n_sub = subf_re.subn(r"ctx->gpr[\1] = ctx->gpr[\2] - ctx->gpr[\3];", text)

open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(text)
print(f"add (reg-reg) -> 64-bit: {n_add}")
print(f"subf (reg-reg) -> 64-bit: {n_sub}")
