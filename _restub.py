#!/usr/bin/env python3
"""Revert specific off-by-4 wrappers back to plain stubs (deny-list).  Use for functions
that are NOT on the game-world/thread-manager path and AV on un-set-up state when un-stubbed
(e.g. bnusCore).  Pass hex addresses on the command line."""
import re, sys
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
deny = [x.lower().lstrip("0x").upper().zfill(5) for x in sys.argv[1:]]
text = open(CPP, encoding="utf-8", errors="replace").read()
n = 0
for a in deny:
    # match the multi-line gated wrapper for func_000A and replace with a plain stub
    pat = re.compile(
        r"(static )?void func_000" + a + r"\(ppu_context\* ctx\) \{\n"
        r"#ifdef GAMEWORLD_REAL_INIT\n"
        r".*?\n"
        r"    while \(g_trampoline_fn\).*?\n"
        r"#else\n"
        r"    ctx->gpr\[3\] = 0;\n"
        r"#endif\n"
        r"\}", re.DOTALL)
    new = f"static void func_000{a}(ppu_context* ctx) {{ ctx->gpr[3] = 0; }}  /* DENY: kept stubbed (off the game-world path) */"
    text, c = pat.subn(new, text)
    n += c
    print(f"  func_000{a}: {'re-stubbed' if c else 'NOT FOUND (already stub or different form)'}")
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(text)
print(f"re-stubbed {n} functions")
