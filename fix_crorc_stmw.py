#!/usr/bin/env python3
"""
Implement crorc (CR OR with Complement) and stmw (Store Multiple Words).

crorc BT, BA, BB: CR[BT] = CR[BA] | ~CR[BB]
  IBM bit N in ctx->cr = x86 bit (31-N)

stmw rS, d(rA): store gpr[rS]..gpr[31] to memory at EA, EA+4, ...
  rA=0 means EA = d (not ctx->gpr[0])
"""
import re

src = "recompiled/ppu_recomp.cpp"
with open(src, "r", encoding="utf-8") as f:
    content = f.read()

counts = {}

# --- crorc BT, BA, BB ---
# CR[BT] = CR[BA] | ~CR[BB]
# IBM bit N = x86 bit (31-N)
def repl_crorc(m):
    BT, BA, BB = int(m.group(1)), int(m.group(2)), int(m.group(3))
    bt_x = 31 - BT
    ba_x = 31 - BA
    bb_x = 31 - BB
    return (f'{{ uint32_t _ba=((ctx->cr>>{ba_x})&1u),_bb=((ctx->cr>>{bb_x})&1u),_r=_ba|(~_bb&1u); '
            f'ctx->cr=(ctx->cr&~(1u<<{bt_x}))|(_r<<{bt_x}); }}')

new_content, n = re.subn(
    r'/\* TODO: crorc (\d+), (\d+), (\d+) \*/;',
    repl_crorc, content)
counts['crorc'] = n
content = new_content

# --- stmw rS, d(rA) ---
# Store gpr[rS]..gpr[31] to EA, EA+4, ... where EA = (rA==0 ? d : gpr[rA]+d)
def repl_stmw(m):
    rS = int(m.group(1))
    d_str = m.group(2)   # displacement, may be hex like 0x0 or decimal
    rA = int(m.group(3))
    d = int(d_str, 16) if d_str.startswith('0x') or d_str.startswith('-0x') else int(d_str)
    # Sign-extend 16-bit displacement
    if d > 32767: d -= 65536

    if rA == 0:
        ea_expr = f"(uint32_t)({d})"
    else:
        ea_expr = f"(uint32_t)(ctx->gpr[{rA}] + ({d}))"

    stores = ""
    for r in range(rS, 32):
        offset = (r - rS) * 4
        stores += f"vm_write32({ea_expr}+{offset}u,(uint32_t)ctx->gpr[{r}]); "

    return f'{{ {stores}}}'

new_content, n = re.subn(
    r'/\* TODO: stmw r(\d+), ((?:0x)?-?[0-9a-fA-F]+)\(r(\d+)\) \*/;',
    repl_stmw, content)
counts['stmw'] = n
content = new_content

with open(src, "w", encoding="utf-8") as f:
    f.write(content)

total = sum(counts.values())
print(f"Replaced {total} stubs:")
for k, v in counts.items():
    print(f"  {k}: {v}")
