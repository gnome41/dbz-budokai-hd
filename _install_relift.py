import shutil, re
SRC = r"E:\Games\RecompLauncher\ps3recomp-relift\recompiled_relift3"
DST = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled"
shutil.copyfile(SRC + r"\ppu_recomp.c", DST + r"\ppu_recomp.cpp")
shutil.copyfile(SRC + r"\ppu_recomp.h", DST + r"\ppu_recomp.h")
p = DST + r"\ppu_recomp.cpp"
t = open(p, encoding="utf-8", errors="replace").read()

# 0) keep extra_funcs.cpp's hand-patched versions of these (entry hijack etc.) by
#    renaming relift3's generated definitions so they don't conflict and direct
#    calls link to the extra_funcs versions instead.
for keep in ("func_000F205C", "func_000E775C"):
    t = t.replace("void %s(ppu_context* ctx) {" % keep,
                  "void %s_dead(ppu_context* ctx) {" % keep, 1)

# 1) migration shims after the DRAIN_TRAMPOLINE macro
shims = ('} while(0)\n\n'
         '/* ---- Migration shims (re-lift base) ---- */\n'
         'extern "C" void lv2_syscall(ppu_context* ctx);\n'
         'static void func_00000030(ppu_context* ctx) { lv2_syscall(ctx); }\n'
         'static void func_FFFFC28C(ppu_context* ctx) { ctx->gpr[3] = 0; }\n'
         'static void func_FFFFFFFC(ppu_context* ctx) { ctx->gpr[3] = 0; }\n')
assert t.count('} while(0)\n') >= 1
t = t.replace('} while(0)\n', shims, 1)

# 2) generate ppu_resolve_addr from every function definition in this TU
names = set(re.findall(r"^(?:static )?void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\)\s*\{", t, re.M))
ents = sorted((int(n[5:], 16), n) for n in names if not n.startswith("func_FFFF"))
tbl = ["\n/* ---- Generated indirect-call resolution table (migration) ---- */",
       "typedef void (*ppu_fn_t)(ppu_context*);",
       "typedef struct { unsigned int a; ppu_fn_t f; } ppu_fn_ent;",
       "static const ppu_fn_ent g_ppu_fns[] = {"]
tbl += ["  {0x%Xu, %s}," % (a, n) for a, n in ents]
tbl += ["};",
        "static const int g_ppu_fns_n = (int)(sizeof(g_ppu_fns)/sizeof(g_ppu_fns[0]));",
        'extern "C" void (*ppu_resolve_addr(unsigned long long addr))(ppu_context*) {',
        "    unsigned int a = (unsigned int)addr; int lo=0, hi=g_ppu_fns_n-1;",
        "    while (lo<=hi) { int m=(lo+hi)>>1; unsigned int x=g_ppu_fns[m].a;",
        "        if (x==a) return g_ppu_fns[m].f; if (x<a) lo=m+1; else hi=m-1; }",
        "    return 0; }"]
t += "\n".join(tbl) + "\n"
open(p, "w", encoding="utf-8", errors="replace").write(t)
print("installed relift3 + shims + ppu_resolve_addr (%d funcs)" % len(ents))
