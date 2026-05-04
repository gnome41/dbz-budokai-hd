#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "recompiled/ppu_recomp.h"

extern jmp_buf g_process_exit_jmpbuf;
extern int     g_process_exit_code;

/* Guest address space base pointer */
extern "C" uint8_t* vm_base = nullptr;

/* Set to true by sys_988(r3=4) from the abort handler; checked by func_000F217C */
extern "C" bool g_abort_called = false;

/* Trampoline TLS variable for cross-fragment branches */
extern "C" __declspec(thread) void (*g_trampoline_fn)(void*) = nullptr;

/* Memory access — big-endian byte-swap, addr truncated to 32-bit */
extern "C" uint8_t vm_read8(uint64_t addr) {
    return vm_base[(uint32_t)addr];
}
extern "C" uint16_t vm_read16(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    return (uint16_t)(((uint16_t)vm_base[a] << 8) | vm_base[a + 1]);
}
extern "C" uint32_t vm_read32(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    return ((uint32_t)vm_base[a]     << 24) | ((uint32_t)vm_base[a + 1] << 16) |
           ((uint32_t)vm_base[a + 2] <<  8) |  (uint32_t)vm_base[a + 3];
}
extern "C" uint64_t vm_read64(uint64_t addr) {
    return ((uint64_t)vm_read32(addr) << 32) | vm_read32(addr + 4);
}
extern "C" void vm_write8(uint64_t addr, uint8_t val) {
    vm_base[(uint32_t)addr] = val;
}
extern "C" void vm_write16(uint64_t addr, uint16_t val) {
    uint32_t a = (uint32_t)addr;
    vm_base[a]     = (uint8_t)(val >> 8);
    vm_base[a + 1] = (uint8_t)(val);
}
extern "C" void vm_write32(uint64_t addr, uint32_t val) {
    uint32_t a = (uint32_t)addr;
    vm_base[a]     = (uint8_t)(val >> 24);
    vm_base[a + 1] = (uint8_t)(val >> 16);
    vm_base[a + 2] = (uint8_t)(val >>  8);
    vm_base[a + 3] = (uint8_t)(val);
}
extern "C" void vm_write64(uint64_t addr, uint64_t val) {
    vm_write32(addr,     (uint32_t)(val >> 32));
    vm_write32(addr + 4, (uint32_t)(val));
}

/* LV2 syscall dispatcher stub */
extern "C" void lv2_syscall(ppu_context* ctx) {
    uint32_t sysnum = (uint32_t)ctx->gpr[11];

    switch (sysnum) {

    /* sys_tty_write — print the game's debug/stdout output */
    case 403: {
        uint32_t fd  = (uint32_t)ctx->gpr[3];   /* 0=stderr 1=stdout */
        uint32_t buf = (uint32_t)ctx->gpr[4];
        uint32_t len = (uint32_t)ctx->gpr[5];
        if (len > 0 && len < 0x10000 && buf >= 0x10000) {
            const char* s = (const char*)(vm_base + buf);
            fprintf(fd == 1 ? stdout : stderr, "[TTY%u] %.*s", fd, (int)len, s);
        }
        /* out param: bytes written — write len into gpr[4] slot if r6 is a ptr */
        if (ctx->gpr[6]) vm_write32(ctx->gpr[6], len);
        ctx->gpr[3] = 0;
        break;
    }

    /* sys_988: watchdog/trace family (sys_game_watchdog_* or similar).
       r3=4 is specifically emitted by the abort() handler (func_000D91E4)
       to flag that the process is aborting — set g_abort_called so that
       the next sys_process_exit (func_000F217C) triggers a clean longjmp. */
    case 988:
        fprintf(stderr, "[LV2] sys_988 (r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
        if (ctx->gpr[3] == 4) {
            g_abort_called = true;
            /* func_000D91E4 saves original LR at sp+0xD0 before calling sys_988.
               sp has been decremented by 0xC0 by func_000D91E4's prologue,
               so the back-chain (original sp) is at sp+0, and the saved LR is at sp+0xD0.
               Walk the back-chain to dump the call stack. */
            uint32_t sp = (uint32_t)ctx->gpr[1];
            uint32_t saved_lr = vm_read32(sp + 0xD0);
            fprintf(stderr, "[ABORT] func_000D91E4 caller LR=0x%08X\n", saved_lr);
            /* Walk 4 more frames for context */
            uint32_t frame = vm_read32(sp);  /* back-chain to caller frame */
            for (int i = 0; i < 4 && frame > 0x10000 && frame < 0x10000000; i++) {
                uint32_t frame_lr = vm_read32(frame + 0x10); /* LR saved at frame+0x10 in PS3 ABI */
                fprintf(stderr, "[ABORT] frame[%d] sp=0x%08X lr@sp+0x10=0x%08X\n", i, frame, frame_lr);
                uint32_t next = vm_read32(frame);
                if (next == frame || next == 0) break;
                frame = next;
            }
        }
        ctx->gpr[3] = 0;
        break;

    default:
        fprintf(stderr, "[LV2] syscall %u (r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                sysnum,
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
        ctx->gpr[3] = 0;
        break;
    }
}

/* Indirect call dispatcher — resolves guest address in CTR to recompiled host fn */
extern "C" void (*ppu_resolve_addr(uint64_t addr))(ppu_context*);
extern "C" void (*ppu_resolve_extra(uint64_t addr))(ppu_context*);

/* Bitmask of guest addresses already printed (simple bloom via modulo) */
static bool g_icall_trace_enabled = true;

extern "C" void ps3_indirect_call(ppu_context* ctx) {
    if (g_icall_trace_enabled) {
        fprintf(stderr, "[ICALL] CTR=0x%08llX r3=0x%08llX r4=0x%08llX\n",
                (unsigned long long)ctx->ctr,
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4]);
    }
    auto fn = ppu_resolve_addr(ctx->ctr);
    if (!fn) fn = ppu_resolve_extra(ctx->ctr);
    if (fn) {
        fn(ctx);
        return;
    }
    /* Unknown address: log first occurrence per address, then stub it out.
       Set gpr[3]=0 so any CELL_OK checks downstream pass. */
    static uint64_t logged[256];
    static int nlogged = 0;
    bool seen = false;
    for (int i = 0; i < nlogged; i++) {
        if (logged[i] == ctx->ctr) { seen = true; break; }
    }
    if (!seen) {
        fprintf(stderr, "[RECOMP] unresolved indirect call: CTR=0x%llX\n",
                (unsigned long long)ctx->ctr);
        if (nlogged < 256) logged[nlogged++] = ctx->ctr;
    }
    ctx->gpr[3] = 0;
}
