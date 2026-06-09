#!/usr/bin/env python3
"""Implement the 657 unimplemented `subfc rD,rA,rB` (subtract-from-carrying) no-ops in
recompiled/ppu_recomp.cpp.  PPC subfc: rD = rB - rA (it also sets XER[CA], but subfe/adde —
the only consumers of CA — are themselves unimplemented, so result-only is sufficient and
safe).  Leaving it a no-op corrupts arithmetic (e.g. the std::string/vector growth/max-size
calc -> spurious 'vector<T> too long')."""
import re
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
t = open(CPP, encoding="utf-8", errors="replace").read()
# /* TODO: subfc r5, r3, r30 */;  ->  ctx->gpr[5] = ctx->gpr[30] - ctx->gpr[3];
pat = re.compile(r"/\* TODO: subfc r(\d+), r(\d+), r(\d+) \*/;")
t, n = pat.subn(r"ctx->gpr[\1] = ctx->gpr[\3] - ctx->gpr[\2];", t)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(t)
print("subfc implemented:", n)
