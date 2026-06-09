#!/usr/bin/env python3
"""Fix the systematic floating-compare NaN-handling bug (fcmp/fcmpu/fcmpo).

The lifter emits float compares as:
    double a = X; double b = Y; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2;
i.e. FL(<)=8, FG(>)=4, else FE(=)=2.  For a NaN operand, a<b and a>b are both
false, so it falls through to 2 (EQUAL) — but the PPC fcmpu sets the FU
(unordered) bit = 1, NOT equal.  Result: every NaN-detection branch in the game
is defeated.  Concretely, func_00068998 does `fcmpu f0,f0` after a sqrt to detect
a NaN (sqrt of a negative) and call its NaN handler func_000E14E4; the buggy
compare reports "equal", the handler is skipped, the NaN propagates, gets
float-truncated/double-read into a finite ~1.4e306, and hangs the number formatter
func_000E2A3C (~10^16-iteration digit loop).

Fix: a NaN must yield 1 (unordered), so change the trailing `: 2;` to
`: (a == b) ? 2 : 1;`.  For non-NaN values a==b is true in the else, so behaviour
is unchanged; only NaN now correctly returns FU=1.  Integer compares use
`int64_t a = ...` and are NOT matched, so they are untouched.
"""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
t = open(CPP, encoding="utf-8", errors="replace").read()
pat = re.compile(
    r"(double a = [^;]+; double b = [^;]+; uint32_t cr_val = \(a < b\) \? 8 : \(a > b\) \? 4 :) 2;")
t, n = pat.subn(r"\1 (a == b) ? 2 : 1;", t)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(t)
print("fcmp NaN-handling fixed:", n)
