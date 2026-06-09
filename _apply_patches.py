#!/usr/bin/env python3
"""Re-apply game-logic patches to the re-lifted ppu_recomp.cpp by replacing the named
functions' bodies with master's patched versions (matched by name).  master works, so its
versions of these specific functions are correct; everywhere else keeps relift3's fixes."""
import subprocess, re, sys

DST = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
# Documented game-logic patches that lived in the generated ppu_recomp.cpp (CLAUDE.md).
PATCHED = [
    "func_00032E58",   # SDU pool bump allocator
    "func_000D9108",   # slab-free guard
    "func_000D9128",   # slab bump allocator
    "func_0004370C",   # cellGcmInit force-success
    "func_00040C0C",   # cellGcmGetContextSize force-success
    "func_00040BD4",   # cellGcmGetMemorySize force-success (+ dummy IO size)
    "func_000379BC",   # SPURS state machine (missing stwu + state 6/15 completion)
    "func_0003AAC8",   # SPURS dispatch: call func_000379BC directly
    "func_000F10FC",   # SDU worker spawner A
    "func_000F16DC",   # SDU worker spawner B
    "func_000F211C",   # SDU thread join
    "func_000510E4",   # bnusCore real init (gated BNUSCORE_REAL_INIT)
]

def extract(text, name):
    """Return the full text of the definition of `name` (brace-matched), or None."""
    m = re.search(r"(?m)^(?:static )?void %s\(ppu_context\* ctx\)\s*\{" % re.escape(name), text)
    if not m:
        return None
    i = text.index("{", m.start())
    depth = 0
    j = i
    while j < len(text):
        c = text[j]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():j+1]
        j += 1
    return None

master = subprocess.run(["git", "show", "master:recompiled/ppu_recomp.cpp"],
                        cwd=r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd",
                        capture_output=True).stdout.decode("utf-8", "replace")
dst = open(DST, encoding="utf-8", errors="replace").read()

applied, missing = [], []
for name in PATCHED:
    mbody = extract(master, name)
    dbody = extract(dst, name)
    if mbody is None:
        missing.append(name + "(not in master)"); continue
    if dbody is None:
        missing.append(name + "(not in relift)"); continue
    dst = dst.replace(dbody, mbody, 1)
    applied.append(name)
open(DST, "w", encoding="utf-8", errors="replace").write(dst)
print("applied:", applied)
print("missing:", missing)
