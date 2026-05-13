/* Extra lifted functions — functions the lifter missed on the first pass
   because they're only reached via indirect CTR calls (C++ ctors/dtors). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#endif

#include "recompiled/ppu_recomp.h"

/* Runtime helpers (defined in runtime_glue.cpp) */
extern "C" uint8_t*  vm_base;
extern "C" uint8_t   vm_read8 (uint64_t addr);
extern "C" uint16_t  vm_read16(uint64_t addr);
extern "C" uint32_t  vm_read32(uint64_t addr);
extern "C" uint64_t  vm_read64(uint64_t addr);
extern "C" void      vm_write8 (uint64_t addr, uint8_t  val);
extern "C" void      vm_write16(uint64_t addr, uint16_t val);
extern "C" void      vm_write32(uint64_t addr, uint32_t val);
extern "C" void      vm_write64(uint64_t addr, uint64_t val);
extern "C" void      ps3_indirect_call(ppu_context* ctx);
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*);

#define DRAIN_TRAMPOLINE(ctx) do { \
    while (g_trampoline_fn) { \
        void(*_tf)(void*) = g_trampoline_fn; \
        g_trampoline_fn = 0; \
        _tf((void*)(ctx)); \
    } \
} while(0)

/* Rotate helpers (static inline — no conflict with ppu_recomp.cpp) */
static inline uint32_t ppc_rlwinm(uint32_t rs, int sh, int mb, int me) {
    uint32_t rotated = (rs << sh) | (rs >> (32 - sh));
    uint32_t mask;
    if (mb <= me) {
        mask = ((uint32_t)-1 >> mb) & ((uint32_t)-1 << (31 - me));
    } else {
        mask = ((uint32_t)-1 >> mb) | ((uint32_t)-1 << (31 - me));
    }
    return rotated & mask;
}
static inline uint64_t ppc_rotl64(uint64_t v, int n) {
    n &= 63;
    return (v << n) | (v >> (64 - n));
}
static inline uint64_t ppc_mask64(int mb, int me) {
    uint64_t mask;
    if (mb <= me) {
        mask = (mb == 0) ? ~0ULL : (~0ULL >> mb);
        mask &= (me == 63) ? ~0ULL : (~0ULL << (63 - me));
    } else {
        mask = (~0ULL >> mb) | (~0ULL << (63 - me));
    }
    return mask;
}
static inline uint64_t ppc_rldicl(uint64_t rs, int sh, int mb) {
    return ppc_rotl64(rs, sh) & ppc_mask64(mb, 63);
}
static inline uint64_t ppc_rldicr(uint64_t rs, int sh, int me) {
    return ppc_rotl64(rs, sh) & ppc_mask64(0, me);
}

/* ---- Lifted function bodies -------------------------------------------- */

void func_000E9218(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000E9144; return; }
}

void func_000CB5B0(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000CB564; return; }
}

void func_000C5C9C(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000C5C38; return; }
}

void func_000B73BC(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000B7398; return; }
}

void func_0003B208(ppu_context* ctx) {
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[3] + -20344);
        vm_write32(ctx->gpr[3] + -0x4F78, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x4, ctx->gpr[4]);
        return;
}

void func_000393D4(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]); ctx->gpr[1] += -0x90;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0xA0, ctx->gpr[0]);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        vm_write64(ctx->gpr[1] + 0x88, ctx->gpr[31]);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[3] + -27248);
        ctx->gpr[5] = (int64_t)(int32_t)(0x5B0);
        vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[30]);
        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[29]);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[31] + 0x18);
        func_000D5654(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[30] = (int64_t)(int32_t)(0);
        ctx->gpr[29] = (int64_t)(int32_t)(1);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        vm_write32(ctx->gpr[31] + 0x5C8, ctx->gpr[30]);
        vm_write32(ctx->gpr[31] + 0x5CC, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D0, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D1, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D6, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D2, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D3, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D4, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D5, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D7, ctx->gpr[29]);
        vm_write8(ctx->gpr[31] + 0x5D8, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5F8, ctx->gpr[30]);
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        func_000F109C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[3] + -25712);
        ctx->gpr[5] = (int64_t)(int32_t)(0x5B0);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[31] + 0x18);
        func_000D5654(ctx); DRAIN_TRAMPOLINE(ctx);
        vm_write32(ctx->gpr[31] + 0x5C8, ctx->gpr[30]);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        vm_write32(ctx->gpr[31] + 0x5CC, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D0, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D1, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D6, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D2, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D3, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D4, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D5, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D7, ctx->gpr[29]);
        vm_write8(ctx->gpr[31] + 0x5D8, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5F8, ctx->gpr[30]);
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        func_000F109C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[3] + -24176);
        ctx->gpr[5] = (int64_t)(int32_t)(0x5B0);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[31] + 0x18);
        func_000D5654(ctx); DRAIN_TRAMPOLINE(ctx);
        vm_write32(ctx->gpr[31] + 0x5C8, ctx->gpr[30]);
        ctx->gpr[3] = (int64_t)(int32_t)(1);
        vm_write32(ctx->gpr[31] + 0x5CC, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D0, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D1, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D6, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D2, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D3, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D4, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D5, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5D7, ctx->gpr[29]);
        vm_write8(ctx->gpr[31] + 0x5D8, ctx->gpr[30]);
        vm_write8(ctx->gpr[31] + 0x5F8, ctx->gpr[30]);
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        func_000F109C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[31] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + -22464);
loc_00039504:
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[31], 0, 32);
        func_00039540(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[30] = (int64_t)(int32_t)(ctx->gpr[30] + 1);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + 0x90);
        { int64_t a = (int32_t)ctx->gpr[30]; int64_t b = (int64_t)0xE; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 8)) goto loc_00039504;
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[29] = vm_read64(ctx->gpr[1] + 0x78);
        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x80);
        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x88);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x90);
        return;
}

void func_00033A54(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x70, ctx->gpr[1]); ctx->gpr[1] += -0x70;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[0]);
        ctx->gpr[4] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[5] = (int64_t)(int32_t)(0);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[4] + -1872);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[3] + -1864);
        vm_write8(ctx->gpr[4] + -0x750, ctx->gpr[5]);
        vm_write32(ctx->gpr[6] + 0x4, ctx->gpr[5]);
        func_00032C08(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[3] + 0x396C);
        func_00032C08(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x80);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x70);
        return;
}

void func_0002761C(ppu_context* ctx) {
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[3] + -2060);
        vm_write32(ctx->gpr[3] + -0x80C, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x4, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x8, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0xC, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x10, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x14, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x18, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x1C, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x20, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x24, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x28, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x2C, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x30, ctx->gpr[4]);
        vm_write32(ctx->gpr[5] + 0x34, ctx->gpr[4]);
        return;
}

void func_00019E8C(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[5] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x16 << 16);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[5] + 0x3EF0);
        ctx->gpr[7] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[8] = (int64_t)(int32_t)(ctx->gpr[4] + -2112);
        ctx->gpr[6] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[3] + 0xA70);
        ctx->gpr[9] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        vm_write32(ctx->gpr[4] + -0x840, ctx->gpr[5]);
        ctx->gpr[7] = (int64_t)(int32_t)(ctx->gpr[7] + 0x3FD0);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[6] + 0x3FD0);
        ctx->gpr[10] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        vm_write32(ctx->gpr[8] + 0x8, ctx->gpr[3]);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[9] + -2100);
        vm_write32(ctx->gpr[8] + 0x4, ctx->gpr[7]);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[10] + 0x4060);
        vm_write32(ctx->gpr[9] + -0x834, ctx->gpr[6]);
        ctx->gpr[7] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[6] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[7] = (int64_t)(int32_t)(ctx->gpr[7] + 0x4060);
        vm_write32(ctx->gpr[4] + 0x8, ctx->gpr[3]);
        ctx->gpr[9] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        vm_write32(ctx->gpr[4] + 0x4, ctx->gpr[5]);
        ctx->gpr[8] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[9] + 0x4140);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[6] + -2088);
        vm_write32(ctx->gpr[6] + -0x828, ctx->gpr[7]);
        ctx->gpr[9] = (int64_t)(int32_t)((uint32_t)0x24 << 16);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[8] + 0x4140);
        ctx->gpr[7] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[8] = (int64_t)(int32_t)(ctx->gpr[9] + 0x4220);
        ctx->gpr[9] = (int64_t)(int32_t)(ctx->gpr[7] + -2076);
        vm_write32(ctx->gpr[4] + 0x8, ctx->gpr[3]);
        vm_write32(ctx->gpr[4] + 0x4, ctx->gpr[5]);
        vm_write32(ctx->gpr[7] + -0x81C, ctx->gpr[6]);
        vm_write32(ctx->gpr[9] + 0x4, ctx->gpr[8]);
        vm_write32(ctx->gpr[9] + 0x8, ctx->gpr[3]);
        return;
}

void func_00014660(ppu_context* ctx) {
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xF << 16);
        ctx->gpr[5] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[4] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[8] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[5] + -16912);
        ctx->gpr[7] = vm_read64(ctx->gpr[3] + 0x28F0);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[3] + 0x28F0);
        vm_write64(ctx->gpr[5] + -0x4210, ctx->gpr[7]);
        ctx->gpr[5] = (int64_t)(int32_t)(ctx->gpr[4] + -16896);
        vm_write64(ctx->gpr[4] + -0x4200, ctx->gpr[7]);
        ctx->gpr[4] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[10] = (int64_t)(int32_t)(ctx->gpr[8] + -16880);
        ctx->gpr[3] = vm_read64(ctx->gpr[3] + 0x8);
        ctx->gpr[9] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        vm_write64(ctx->gpr[8] + -0x41F0, ctx->gpr[7]);
        ctx->gpr[8] = (int64_t)(int32_t)(ctx->gpr[4] + -16864);
        vm_write64(ctx->gpr[6] + 0x8, ctx->gpr[3]);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[9] + -16832);
        vm_write64(ctx->gpr[5] + 0x8, ctx->gpr[3]);
        vm_write64(ctx->gpr[4] + -0x41E0, ctx->gpr[7]);
        vm_write64(ctx->gpr[10] + 0x8, ctx->gpr[3]);
        vm_write64(ctx->gpr[8] + 0x8, ctx->gpr[3]);
        vm_write64(ctx->gpr[9] + -0x41C0, ctx->gpr[7]);
        vm_write64(ctx->gpr[6] + 0x8, ctx->gpr[3]);
        return;
}

void func_00014538(ppu_context* ctx) {
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        ctx->gpr[6] = (int64_t)(int32_t)((uint32_t)0xF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[3] + -17008);
        ctx->gpr[5] = (int64_t)(int32_t)(0);
        ctx->gpr[6] = (int64_t)(int32_t)(ctx->gpr[6] + 0x2890);
        vm_write8(ctx->gpr[3] + -0x4270, ctx->gpr[5]);
        vm_write32(ctx->gpr[4] + 0x4, ctx->gpr[6]);
        return;
}

void func_000E770C(ppu_context* ctx) {
        ctx->gpr[3] = vm_read32(ctx->gpr[2] + -0x5B50);
        ctx->gpr[0] = ctx->lr;
        ctx->gpr[4] = vm_read32(ctx->gpr[2] + -0x5B54);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[3] + 0x18);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[4] + 0x10);
        vm_write64(ctx->gpr[1] + -0x70, ctx->gpr[1]); ctx->gpr[1] += -0x70;
        vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[0]);
        func_000F1EFC(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if ((!((ctx->cr >> 0) & 2))) { g_trampoline_fn = (void(*)(void*))func_000E7754; return; }
        ctx->gpr[3] = vm_read32(ctx->gpr[2] + -0x5B48);
        func_000CE56C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x80);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x70);
        ctx->lr = ctx->gpr[0];
        return;
}

void func_000E775C(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]); ctx->gpr[1] += -0x90;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[29]);
        ctx->gpr[29] = vm_read32(ctx->gpr[2] + -0x5B50);
        ctx->gpr[4] = (int64_t)(int32_t)(0);
        vm_write64(ctx->gpr[1] + 0x70, ctx->gpr[28]);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 0x18);
        vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[30]);
        vm_write64(ctx->gpr[1] + 0x88, ctx->gpr[31]);
        vm_write64(ctx->gpr[1] + 0xA0, ctx->gpr[0]);
        func_000F1E7C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if ((!((ctx->cr >> 0) & 2))) goto loc_000E7840;
        ctx->gpr[28] = vm_read32(ctx->gpr[29] + 0x78);
        { int64_t a = (int32_t)ctx->gpr[28]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if (((ctx->cr >> 0) & 2)) goto loc_000E782C;
loc_000E77A0:
        ctx->gpr[30] = (int64_t)(int32_t)(ctx->gpr[28] + 0x10);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[29] + 0x30);
loc_000E77A8:
        ctx->gpr[9] = ppc_rldicl(ctx->gpr[31], 0, 32);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + 8);
        ctx->gpr[0] = vm_read8(ctx->gpr[9] + 0x0);
        { int64_t a = (int32_t)ctx->gpr[0]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if (((ctx->cr >> 0) & 2)) goto loc_000E77F8;
        ctx->gpr[0] = vm_read32(ctx->gpr[9] + 0x4);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[30], 0, 32);
        { int64_t a = (int32_t)ctx->gpr[0]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if (((ctx->cr >> 0) & 2)) goto loc_000E77F8;
        ctx->gpr[9] = ppc_rldicl(ctx->gpr[0], 0, 32);
        ctx->gpr[0] = vm_read32(ctx->gpr[3] + 0x0);
        { int64_t a = (int32_t)ctx->gpr[0]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[0], 0, 32);
        if (((ctx->cr >> 0) & 2)) goto loc_000E77F8;
        ctx->gpr[0] = vm_read32(ctx->gpr[9] + 0x0);
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->ctr = (uint32_t)ctx->gpr[0];
        ctx->gpr[2] = vm_read32(ctx->gpr[9] + 0x4);
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_000E77F8:
        ctx->gpr[0] = (int64_t)(int32_t)(ctx->gpr[29] + 0x50);
        ctx->gpr[30] = (int64_t)(int32_t)(ctx->gpr[30] + 4);
        { int64_t a = (int32_t)ctx->gpr[31]; int64_t b = (int32_t)ctx->gpr[0]; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if ((!((ctx->cr >> 0) & 2))) goto loc_000E77A8;
        ctx->gpr[9] = ppc_rldicl(ctx->gpr[28], 0, 32);
        ctx->gpr[3] = ctx->gpr[9] | ctx->gpr[9];
        ctx->gpr[31] = vm_read32(ctx->gpr[9] + 0x0);
        func_000D90FC(ctx); DRAIN_TRAMPOLINE(ctx);
        { int64_t a = (int32_t)ctx->gpr[31]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 0)) | (cr_val << 0); }
        if (((ctx->cr >> 0) & 2)) goto loc_000E782C;
        ctx->gpr[28] = ctx->gpr[31] | ctx->gpr[31];
        goto loc_000E77A0;
loc_000E782C:
        ctx->gpr[0] = (int64_t)(int32_t)(0);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 0x18);
        vm_write32(ctx->gpr[29] + 0x78, ctx->gpr[0]);
        func_000F1E9C(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_000E7840:
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);
        ctx->gpr[28] = vm_read64(ctx->gpr[1] + 0x70);
        ctx->gpr[29] = vm_read64(ctx->gpr[1] + 0x78);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x80);
        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x88);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x90);
        return;
}

void func_00033A50(ppu_context* ctx) {
        return;
}

void func_000C5C8C(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(0);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000C5C38; return; }
}

void func_000CB5C0(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(0);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000CB564; return; }
}

void func_000E9208(ppu_context* ctx) {
        ctx->gpr[4] = (int64_t)(int32_t)(-1);
        ctx->gpr[3] = (int64_t)(int32_t)(0);
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 48);
        { g_trampoline_fn = (void(*)(void*))func_000E9144; return; }
}

void func_0003937C(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0x90, ctx->gpr[0]);
        vm_write64(ctx->gpr[1] + 0x70, ctx->gpr[30]);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);
        ctx->gpr[30] = (int64_t)(int32_t)(0xD);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[3] + -22464);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + 0x750);
loc_0003937C_loop:
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[31], 0, 32);
        func_0003959C(ctx); DRAIN_TRAMPOLINE(ctx);
        /* nop */;
        ctx->gpr[30] = (int64_t)(int32_t)(ctx->gpr[30] + -1);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + -144);
        { int64_t a = (int32_t)ctx->gpr[30]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 8))) goto loc_0003937C_loop;
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x90);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x70);
        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x80);
        return;
}

void func_0003B124(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0x90, ctx->gpr[0]);
        vm_write64(ctx->gpr[1] + 0x70, ctx->gpr[30]);
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0x29 << 16);
        ctx->gpr[30] = (int64_t)(int32_t)(ctx->gpr[3] + -20344);
        ctx->gpr[30] = vm_read32(ctx->gpr[30] + 0x4);
        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);
        { int64_t a = (int32_t)ctx->gpr[30]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_0003B124_done;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[30] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[31] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_0003B124_lwarx1:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[31] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_0003B124_lwarx1;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_0003B124_done;
        ctx->gpr[4] = vm_read32(ctx->gpr[30] + 0x0);
        ctx->gpr[3] = ctx->gpr[30] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[30] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_0003B124_lwarx2:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[31] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_0003B124_lwarx2;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_0003B124_done;
        ctx->gpr[4] = vm_read32(ctx->gpr[30] + 0x0);
        ctx->gpr[3] = ctx->gpr[30] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_0003B124_done:
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x90);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x70);
        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x80);
        return;
}

void func_00027110(ppu_context* ctx) {
        vm_write64(ctx->gpr[1] + -0x90, ctx->gpr[1]); ctx->gpr[1] += -0x90;
        ctx->gpr[0] = ctx->lr;
        vm_write64(ctx->gpr[1] + 0xA0, ctx->gpr[0]);
        vm_write64(ctx->gpr[1] + 0x88, ctx->gpr[31]);
        ctx->gpr[31] = (int64_t)(int32_t)((uint32_t)0x28 << 16);
        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[29]);
        ctx->gpr[31] = (int64_t)(int32_t)(ctx->gpr[31] + -2060);
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0x34);
        vm_write64(ctx->gpr[1] + 0x80, ctx->gpr[30]);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e0;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx1:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx1;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx2:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx2;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e0:
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0x2C);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e90;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx3:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx3;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e90;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx4:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx4;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e90;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e90:
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0x24);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e140;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx5:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx5;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e140;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx6:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx6;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e140;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e140:
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0x1C);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e1f0;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx7:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx7;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e1f0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx8:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx8;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e1f0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e1f0:
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0x14);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e2a0;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx9:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx9;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e2a0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx10:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx10;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e2a0;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e2a0:
        ctx->gpr[29] = vm_read32(ctx->gpr[31] + 0xC);
        { int64_t a = (int32_t)ctx->gpr[29]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_e350;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[29] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx11:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx11;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e350;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[29] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx12:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx12;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_e350;
        ctx->gpr[4] = vm_read32(ctx->gpr[29] + 0x0);
        ctx->gpr[3] = ctx->gpr[29] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_e350:
        ctx->gpr[31] = vm_read32(ctx->gpr[31] + 0x4);
        { int64_t a = (int32_t)ctx->gpr[31]; int64_t b = (int64_t)0; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if (((ctx->cr >> 28) & 2)) goto loc_00027110_done;
        ctx->gpr[3] = (int64_t)(int32_t)((uint32_t)0xFFFF << 16);
        ctx->gpr[4] = (int64_t)(int32_t)(ctx->gpr[31] + 4);
        ctx->gpr[3] = ctx->gpr[3] | 0xFFFF;
        ctx->gpr[4] = ppc_rldicl(ctx->gpr[4], 0, 32);
        ctx->gpr[30] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx13:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; ctx->gpr[3] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[3]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[3]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[4]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx13;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[3]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_done;
        ctx->gpr[4] = vm_read32(ctx->gpr[31] + 0x0);
        ctx->gpr[3] = ctx->gpr[31] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0x8);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[3] = (int64_t)(int32_t)(ctx->gpr[31] + 8);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
        ctx->gpr[3] = ppc_rldicl(ctx->gpr[3], 0, 32);
        /* sync */;
loc_00027110_lwarx14:
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; ctx->gpr[4] = vm_read32(ea); ctx->reserve_addr = (uint32_t)ea; ctx->reserve_value = ctx->gpr[4]; }
        ctx->gpr[0] = (int64_t)(int32_t)((uint32_t)ctx->gpr[30] + (uint32_t)ctx->gpr[4]);
        { uint64_t ea = ctx->gpr[0] + ctx->gpr[3]; if (ctx->reserve_addr == (uint32_t)ea) { vm_write32(ea, (uint32_t)ctx->gpr[0]); ctx->cr = (ctx->cr & ~(0xFu << 28)) | (2u << 28); } else { ctx->cr = (ctx->cr & ~(0xFu << 28)); } ctx->reserve_addr = 0; }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_lwarx14;
        /* isync */;
        { int64_t a = (int32_t)ctx->gpr[4]; int64_t b = (int64_t)1; uint32_t cr_val = (a < b) ? 8 : (a > b) ? 4 : 2; ctx->cr = (ctx->cr & ~(0xFu << 28)) | (cr_val << 28); }
        if ((!((ctx->cr >> 28) & 2))) goto loc_00027110_done;
        ctx->gpr[4] = vm_read32(ctx->gpr[31] + 0x0);
        ctx->gpr[3] = ctx->gpr[31] | 0x0;
        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);
        ctx->gpr[4] = vm_read32(ctx->gpr[4] + 0xC);
        ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);
        ctx->gpr[2] = vm_read32(ctx->gpr[4] + 0x4);
        ctx->ctr = (uint32_t)ctx->gpr[5];
        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);
        ctx->gpr[2] = vm_read64(ctx->gpr[1] + 0x28);
loc_00027110_done:
        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0xA0);
        ctx->lr = ctx->gpr[0];
        ctx->gpr[29] = vm_read64(ctx->gpr[1] + 0x78);
        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x80);
        ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x88);
        ctx->gpr[1] = (int64_t)(int32_t)(ctx->gpr[1] + 0x90);
        return;
}

/* ---- Wrapper for thread entry func_000EFD18 ----------------------------
 * The OPD entry for "sdu_yah_size_check" points to 0x000EFD18, but the
 * lifter only emitted func_000EFD1C (4 bytes later) and dropped the
 * `stwu r1, -0xB0(r1)` prologue at 0x000EFD18 — same lifter bug as
 * func_000379BC.  This wrapper performs the missing stwu and then calls
 * the body.  func_000EFD1C's epilogue does `gpr[1] += 0xB0`, restoring SP. */
void func_000EFD1C(ppu_context* ctx);
void func_000EFD18(ppu_context* ctx) {
    vm_write64(ctx->gpr[1] - 0xB0, ctx->gpr[1]);  /* back-chain */
    ctx->gpr[1] -= 0xB0;
    func_000EFD1C(ctx);
}

/* ---- Wrapper for thread entry func_000EFACC ----------------------------
 * OPD for "sdu_yah_all_list_delete" has code=0x000EFACC, but the lifter
 * emitted func_000EFAD0 (4 bytes later) and dropped the stdu r1,-0xC0(r1)
 * prologue at 0xEFACC.  func_000EFAD0's epilogue does gpr[1] += 0xC0. */
void func_000EFAD0(ppu_context* ctx);
void func_000EFACC(ppu_context* ctx) {
    vm_write64(ctx->gpr[1] - 0xC0, ctx->gpr[1]);  /* back-chain */
    ctx->gpr[1] -= 0xC0;
    func_000EFAD0(ctx);
}

/* ---- HLE: sysPrxForUser NID 0xA2C7BA64 (called by func_0003B244, r3=0) ----
 * Called from the C runtime startup wrapper.  In a real PS3 build this would
 * be a libc helper (TLS/atexit/argv setup).  Stub: return 0.  The natural
 * call flow through func_000CE57C still runs the .init_array / .fini_array
 * walkers (func_000CE1E8) and the destructor list (func_000F0A78 →
 * func_0003B4FC → func_0003B500), so we do not need to drive those here. */
void func_000F205C(ppu_context* ctx) {
    ctx->gpr[3] = 0;
}

/* ---- Secondary function table ------------------------------------------ */

typedef struct { uint64_t addr; void (*func)(ppu_context*); } extra_entry;
static const extra_entry extra_table[] = {
    { 0x000E9218ULL, func_000E9218 },
    { 0x000CB5B0ULL, func_000CB5B0 },
    { 0x000C5C9CULL, func_000C5C9C },
    { 0x000B73BCULL, func_000B73BC },
    { 0x0003B208ULL, func_0003B208 },
    { 0x000393D4ULL, func_000393D4 },
    { 0x00033A54ULL, func_00033A54 },
    { 0x0002761CULL, func_0002761C },
    { 0x00019E8CULL, func_00019E8C },
    { 0x00014660ULL, func_00014660 },
    { 0x00014538ULL, func_00014538 },
    { 0x000E770CULL, func_000E770C },
    { 0x000E775CULL, func_000E775C },
    { 0x00027110ULL, func_00027110 },   /* full prologue+body with stdu -0x90 */
    { 0x00033A50ULL, func_00033A54 },   /* skip unknown bl; enter at stdu -0x70 */
    { 0x0003937CULL, func_0003937C },   /* full prologue+body with stdu -0x80 */
    { 0x0003B124ULL, func_0003B124 },
    { 0x000C5C8CULL, func_000C5C8C },
    { 0x000CB5C0ULL, func_000CB5C0 },
    { 0x000E9208ULL, func_000E9208 },
    { 0x000EFD18ULL, func_000EFD18 },   /* sdu_yah_size_check thread entry: missing stwu wrapper */
    { 0x000EFACCULL, func_000EFACC },   /* sdu_yah_all_list_delete thread entry: missing stdu wrapper */
    { 0, nullptr },
};

extern "C" void (*ppu_resolve_extra(uint64_t addr))(ppu_context*) {
    for (int i = 0; extra_table[i].func != nullptr; i++) {
        if (extra_table[i].addr == addr)
            return extra_table[i].func;
    }
    return nullptr;
}
