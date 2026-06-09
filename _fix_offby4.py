#!/usr/bin/env python3
"""Convert the 686 off-by-4 dropped-stdu stub one-liners into gated missing-stdu wrappers.
Each `(static )?void func_000A(ppu_context* ctx) { ctx->gpr[3] = 0; }` becomes:
    <prefix>void func_000A(ppu_context* ctx) {
    #ifdef GAMEWORLD_REAL_INIT
        vm_write64(ctx->gpr[1] - 0xSZ, ctx->gpr[1]); ctx->gpr[1] -= 0xSZ; func_000B(ctx);
        while (g_trampoline_fn) { void(*_tf)(void*)=g_trampoline_fn; g_trampoline_fn=nullptr; _tf((void*)ctx); }
    #else
        ctx->gpr[3] = 0;
    #endif
    }
where B = A+4 and SZ is the stdu frame size from the ELF.  Flag OFF => identical to baseline.
"""
import struct, re
ELF = r"E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf"
CPP = r"E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd\recompiled\ppu_recomp.cpp"
DENY = {0x1241C}  # game main OPD entry: keep stubbed (func_00012420 has no re-entrancy guard)

data = open(ELF, "rb").read()
endi = ">"
e_phoff = struct.unpack_from(endi+"Q", data, 0x20)[0]
e_phnum = struct.unpack_from(endi+"H", data, 0x38)[0]
e_phentsize = struct.unpack_from(endi+"H", data, 0x36)[0]
segs = []
for i in range(e_phnum):
    ph = e_phoff + i*e_phentsize
    if struct.unpack_from(endi+"I", data, ph)[0] == 1:
        po = struct.unpack_from(endi+"Q", data, ph+0x08)[0]
        pv = struct.unpack_from(endi+"Q", data, ph+0x10)[0]
        pf = struct.unpack_from(endi+"Q", data, ph+0x20)[0]
        segs.append((po, pv, pf))
def insn(va):
    for po, pv, pf in segs:
        if pv <= va < pv+pf: return struct.unpack_from(endi+"I", data, po+(va-pv))[0]
def frame_size(va):
    w = insn(va); op = w >> 26
    if (w>>21)&0x1F != 1 or (w>>16)&0x1F != 1: return None
    if op == 37: d = w & 0xFFFF
    elif op == 62 and (w & 3) == 1: d = w & 0xFFFC
    else: return None
    if d & 0x8000: d -= 0x10000
    return -d
MFLR = 0x7C0802A6

text = open(CPP, encoding="utf-8", errors="replace").read()
defined = set(int(m, 16) for m in re.findall(r"func_000([0-9A-Fa-f]{5,6})\(ppu_context", text))

stub_re = re.compile(
    r"^(?P<indent>[ \t]*)(?P<pfx>static )?void func_000(?P<a>[0-9A-Fa-f]{5,6})\(ppu_context\* ctx\) \{ ctx->gpr\[3\] = 0; \}[ \t]*$",
    re.MULTILINE)

count = 0
def repl(m):
    global count
    a = int(m.group("a"), 16)
    if (a+4) not in defined or a in DENY: return m.group(0)
    sz = frame_size(a)
    if sz is None or insn(a+4) != MFLR: return m.group(0)
    count += 1
    ind = m.group("indent"); pfx = m.group("pfx") or ""
    A = "%05X" % a; B = "%05X" % (a+4)
    return (f"{ind}{pfx}void func_000{A}(ppu_context* ctx) {{\n"
            f"{ind}#ifdef GAMEWORLD_REAL_INIT\n"
            f"{ind}    vm_write64(ctx->gpr[1] - 0x{sz:X}, ctx->gpr[1]); ctx->gpr[1] -= 0x{sz:X}; func_000{B}(ctx);\n"
            f"{ind}    while (g_trampoline_fn) {{ void(*_tf)(void*)=g_trampoline_fn; g_trampoline_fn=nullptr; _tf((void*)ctx); }}\n"
            f"{ind}#else\n"
            f"{ind}    ctx->gpr[3] = 0;\n"
            f"{ind}#endif\n"
            f"{ind}}}")

text = stub_re.sub(repl, text)
open(CPP, "w", encoding="utf-8", errors="replace", newline="").write(text)
print(f"converted {count} off-by-4 stubs to gated missing-stdu wrappers")
