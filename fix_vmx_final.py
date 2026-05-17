#!/usr/bin/env python3
"""
Implement the 5 remaining identified VMX instruction stubs:
  vmx_x262 = vspltb   (replicate byte element uimm from VB into all bytes of VD)
  vmx_x294 = vsplth   (replicate halfword element uimm from VB into all halfwords of VD)
  vmx_x611 = vcmpeqfp.(compare equal float, set CR6)
  vmx_x739 = vcmpgefp.(compare >= float, set CR6)
  vmx_x867 = vcmpgtfp.(compare > float, set CR6)

XO identification:
  vmx_x262: xo_old=262 => XO_11=524 = vspltb
  vmx_x294: xo_old=294 => XO_11=588 = vsplth
  vmx_x611: xo_old=611 => XO_11=1222 = 1024+198 = vcmpeqfp (Rc=1)
  vmx_x739: xo_old=739 => XO_11=1478 = 1024+454 = vcmpgefp (Rc=1)
  vmx_x867: xo_old=867 => XO_11=1734 = 1024+710 = vcmpgtfp (Rc=1)

CR6 update for Rc=1 variants:
  CR6 occupies x86 bits 7-4 of ctx->cr (IBM bits 24-27)
  CR6[0] = bit 7 (0x80): set if ALL 4 elements are 0xFFFFFFFF (all TRUE)
  CR6[2] = bit 5 (0x20): set if NO elements are 0xFFFFFFFF (none TRUE)
"""
import re
import sys

src = "recompiled/ppu_recomp.cpp"

with open(src, "r", encoding="utf-8") as f:
    content = f.read()

counts = {}

# --- vspltb (vmx_x262) ---
# vspltb VD, uimm, VB: replicate byte uimm of VB into all 16 bytes of VD
# VA field in instruction encoding holds the uimm (4-bit element selector, 0-15)
def repl_vspltb(m):
    VD, VA, VB = m.group(1), m.group(2), m.group(3)
    return (f'{{ uint8_t _b=((uint8_t*)&ctx->vr[{VB}])[{VA}&0xF]; '
            f'uint8_t* _d=(uint8_t*)&ctx->vr[{VD}]; for(int _i=0;_i<16;_i++) _d[_i]=_b; }}')

new_content, n = re.subn(
    r'/\* TODO: vmx_x262 v(\d+), v(\d+), v(\d+) \*/;',
    repl_vspltb, content)
counts['vspltb'] = n
content = new_content

# --- vsplth (vmx_x294) ---
# vsplth VD, uimm, VB: replicate halfword uimm of VB into all 8 halfwords of VD
# VA field holds the uimm (3-bit element selector, 0-7)
def repl_vsplth(m):
    VD, VA, VB = m.group(1), m.group(2), m.group(3)
    return (f'{{ uint16_t _h=((uint16_t*)&ctx->vr[{VB}])[{VA}&0x7]; '
            f'uint16_t* _d=(uint16_t*)&ctx->vr[{VD}]; for(int _i=0;_i<8;_i++) _d[_i]=_h; }}')

new_content, n = re.subn(
    r'/\* TODO: vmx_x294 v(\d+), v(\d+), v(\d+) \*/;',
    repl_vsplth, content)
counts['vsplth'] = n
content = new_content

# --- vcmpeqfp. (vmx_x611) --- compare equal float, Rc=1 sets CR6
def repl_vcmpeqfp_rc(m):
    VD, VA, VB = m.group(1), m.group(2), m.group(3)
    return (f'{{ float* _a=(float*)&ctx->vr[{VA}]; float* _b=(float*)&ctx->vr[{VB}]; '
            f'uint32_t* _d=(uint32_t*)&ctx->vr[{VD}]; int _at=1,_nt=1; '
            f'for(int _i=0;_i<4;_i++){{_d[_i]=(_a[_i]==_b[_i])?~0u:0; if(!_d[_i])_at=0; else _nt=0;}} '
            f'ctx->cr=(ctx->cr&~0xF0u)|(_at?0x80u:0u)|(_nt?0x20u:0u); }}')

new_content, n = re.subn(
    r'/\* TODO: vmx_x611 v(\d+), v(\d+), v(\d+) \*/;',
    repl_vcmpeqfp_rc, content)
counts['vcmpeqfp.'] = n
content = new_content

# --- vcmpgefp. (vmx_x739) --- compare >= float, Rc=1 sets CR6
def repl_vcmpgefp_rc(m):
    VD, VA, VB = m.group(1), m.group(2), m.group(3)
    return (f'{{ float* _a=(float*)&ctx->vr[{VA}]; float* _b=(float*)&ctx->vr[{VB}]; '
            f'uint32_t* _d=(uint32_t*)&ctx->vr[{VD}]; int _at=1,_nt=1; '
            f'for(int _i=0;_i<4;_i++){{_d[_i]=(_a[_i]>=_b[_i])?~0u:0; if(!_d[_i])_at=0; else _nt=0;}} '
            f'ctx->cr=(ctx->cr&~0xF0u)|(_at?0x80u:0u)|(_nt?0x20u:0u); }}')

new_content, n = re.subn(
    r'/\* TODO: vmx_x739 v(\d+), v(\d+), v(\d+) \*/;',
    repl_vcmpgefp_rc, content)
counts['vcmpgefp.'] = n
content = new_content

# --- vcmpgtfp. (vmx_x867) --- compare > float, Rc=1 sets CR6
def repl_vcmpgtfp_rc(m):
    VD, VA, VB = m.group(1), m.group(2), m.group(3)
    return (f'{{ float* _a=(float*)&ctx->vr[{VA}]; float* _b=(float*)&ctx->vr[{VB}]; '
            f'uint32_t* _d=(uint32_t*)&ctx->vr[{VD}]; int _at=1,_nt=1; '
            f'for(int _i=0;_i<4;_i++){{_d[_i]=(_a[_i]>_b[_i])?~0u:0; if(!_d[_i])_at=0; else _nt=0;}} '
            f'ctx->cr=(ctx->cr&~0xF0u)|(_at?0x80u:0u)|(_nt?0x20u:0u); }}')

new_content, n = re.subn(
    r'/\* TODO: vmx_x867 v(\d+), v(\d+), v(\d+) \*/;',
    repl_vcmpgtfp_rc, content)
counts['vcmpgtfp.'] = n
content = new_content

with open(src, "w", encoding="utf-8") as f:
    f.write(content)

total = sum(counts.values())
print(f"Replaced {total} VMX stubs:")
for k, v in counts.items():
    print(f"  {k}: {v}")
