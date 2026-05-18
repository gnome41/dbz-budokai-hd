/*
 * SPURS kernel thread — runs the embedded SPURS kernel SPU program.
 *
 * The SPURS kernel ELF is embedded in the PS3 game binary and mapped to
 * guest address 0x10BD00.  We load it into an spu_ctx_t and run it in a
 * dedicated Windows thread.
 *
 * The kernel reads its management area (at guest 0x700000) via DMA, then
 * dispatches SPU workloads.  We trace its first DMA operations to learn the
 * SPURS data format.
 */
#include "spu_interp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ---- Globals set by main.cpp before spurs_start() ----------------------- */
extern "C" uint8_t *vm_base;
extern "C" volatile bool g_threads_should_exit;

/* Guest addresses of the embedded SPU ELFs (from PT_LOAD segment analysis) */
#define SPURS_KERNEL_A_GADDR  0x10BD00u   /* ~130KB, entry 0xD0   */
#define GAME_WORKLOAD_1_GADDR 0x142900u   /* ~33KB code, entry 0x3050 */
#define GAME_WORKLOAD_2_GADDR 0x14AE80u   /* ~38KB code, entry 0x3050 */

/* Maximum ELF size for the SPURS kernel.
 * The kernel branches to LS[0x298E0] for its mailbox-wait loop; we need the
 * full binary.  Gap to next ELF (0x142900 - 0x10BD00) = 0x36C00; use 0x36800
 * to stay within bounds.  spu_load_elf honours the actual ELF segment sizes,
 * so loading extra is safe — BSS regions remain zero. */
#define SPURS_KERNEL_ELF_SIZE  0x36800u

/* Game workload ELF sizes: gap between consecutive ELF start addresses */
#define GAME_WORKLOAD_1_SIZE  (GAME_WORKLOAD_2_GADDR - GAME_WORKLOAD_1_GADDR)  /* ~0x8580 */
#define GAME_WORKLOAD_2_SIZE  0x10000u  /* safe upper bound for workload 2 */

/* SPURS management area EA (the synthetic context we set up in main.cpp).
 * Must satisfy: byte[2]&0xF0==0 AND byte[3]&0x0F==0 so that the SPURS kernel's
 * ceqi check at LS[0x17C] passes (the rothi+rotqbybi chain extracts EA bits
 * and must give r4.u32[0]==8).  0x70A000 meets both conditions:
 *   byte[2]=0x0A (bits[23:20]=0 ✓), byte[3]=0x70 (bits[15:12]=0 ✓). */
#define SPURS_CTX_EA   0x70A000u

static spu_ctx_t g_spurs_ctx;
static int       g_spurs_started = 0;

/* ---- Game workload runner ------------------------------------------------ */
/* Loaded lazily on first dispatch.  One shared context reused per slot since
 * we run workloads synchronously during the diagnostic burst. */
static spu_ctx_t  g_wl_ctx;          /* reused for each workload dispatch */
static int        g_wl_ctx_valid = 0;/* 1 after successful spu_load_elf */

static void spurs_run_workload(int slot_idx) {
    /* Only load the ELF on the first dispatch (slot 0).
     * The game has two workload ELFs; start with workload 1 for all slots. */
    if (!g_wl_ctx_valid) {
        uint32_t gaddr = GAME_WORKLOAD_1_GADDR;
        uint32_t gsz   = GAME_WORKLOAD_1_SIZE;
        const uint8_t* elf_ptr = vm_base + gaddr;

        if (elf_ptr[0] != 0x7F || elf_ptr[1] != 'E' ||
            elf_ptr[2] != 'L'  || elf_ptr[3] != 'F') {
            fprintf(stderr, "[WL] no ELF magic at guest 0x%X — skipping\n", gaddr);
            fflush(stderr);
            return;
        }

        spu_ctx_init(&g_wl_ctx, 2, vm_base);
        g_wl_ctx.verbose = 1;      /* log DMA + channel ops */
        g_wl_ctx.trace_limit = 50; /* trace first 50 instructions */
        g_wl_ctx.trace_count = 0;

        if (spu_load_elf(&g_wl_ctx, elf_ptr, gsz) != 0) {
            fprintf(stderr, "[WL] spu_load_elf failed for workload at 0x%X\n", gaddr);
            fflush(stderr);
            return;
        }
        g_wl_ctx_valid = 1;

        fprintf(stderr, "[WL] ELF loaded from guest 0x%X, entry=0x%X, size=0x%X\n",
                gaddr, g_wl_ctx.pc, gsz);
        fflush(stderr);
    } else {
        /* Subsequent dispatches: re-run from entry, keeping the loaded LS.
         * Reset PC to entry (stored in pc at load time — we'd need to save it). */
        g_wl_ctx.trace_limit = 0;  /* no per-insn trace after first run */
        /* pc will be set from the ELF entry on each reload; since we reuse
         * the ctx after the first load, we reset pc to the ELF entry.
         * For now: just skip subsequent dispatches (focus on slot 0). */
        fprintf(stderr, "[WL] slot %d: reusing ctx (skipping reload)\n", slot_idx);
        fflush(stderr);
        return;
    }

    /* Set up register state for workload 1 (EDGE SPU library).
     *
     * The workload entry encodes the return address via three rotmai chains
     * that all write to r0 — whichever runs last wins.  Decoded from the
     * disassembly (wl1.elf LS[0x30B0..0x30C0]):
     *   rotmai r0, r88,  511 → r0 = r88 >> 1    (shift=1)
     *   rotmai r0, r19,  0   → r0 = r19 >> 0    (shift=0, direct copy)
     *   rotmai r0, r114, 56  → r0 = r114 >> 8   (shift=8)
     * For all three to give r0=0x40 (our stop-0x100 sentinel): */
    g_wl_ctx.gpr[3].u32[0]   = SPURS_CTX_EA;  /* SPURS management area EA */
    g_wl_ctx.gpr[0].u32[0]   = 0x40;           /* initial r0 (overwritten by rotmai) */
    g_wl_ctx.gpr[88].u32[0]  = 0x80;   /* r88>>1  = 0x40 */
    g_wl_ctx.gpr[19].u32[0]  = 0x40;   /* r19>>0  = 0x40 */
    g_wl_ctx.gpr[114].u32[0] = 0x4000; /* r114>>8 = 0x40 */
    /* Also copy the kernel's return-addr pattern for safety */
    g_wl_ctx.gpr[86].u32[0] = 0x80;
    g_wl_ctx.gpr[89].u32[0] = 0x80;

    /* Mechanism: first pass stores r0=0x40 to stack (stqd r0, 0x10(r1)),
     * then allocates frame (ai r1, r1, -32), issues stop-0x3FFF.
     * After stop-0x3FFF restart: lqd r0, 0x30(r1_new) = lqd r0, 0x30(r1_old-32)
     * = reads from r1_old+0x10 = the same location we saved to → r0=0x40.
     * bi r0 → LS[0x40] = stop-0x100 sentinel → workload returns. */

    /* Place stop 0x100 at LS[0x40] as the "workload done" sentinel */
    uint8_t stop100[4] = {0x00, 0x00, 0x01, 0x00};
    memcpy(g_wl_ctx.ls + 0x40, stop100, 4);

    /* Run workload with restart handling (same lifecycle as the SPURS kernel):
     *   stop 0   = yield/wait: restart from current PC
     *   stop 0x100 = workload "returned": done
     *   other    = real halt
     * Run up to 20 restart iterations, up to 50K insns each.            */
    fprintf(stderr, "[WL] slot %d: running from entry 0x%X\n", slot_idx, g_wl_ctx.pc);
    fflush(stderr);

    uint32_t ran = 0;
    for (int wl_restart = 0; wl_restart < 20; wl_restart++) {
        ran += spu_run(&g_wl_ctx, 50000);
        if (!g_wl_ctx.running) {
            uint32_t wcode = g_wl_ctx.stop_code;
            if (wcode == 0) {
                /* Yield — restart from current PC */
                fprintf(stderr, "[WL] slot %d restart %d at PC=0x%X r3=0x%X r4=0x%X\n",
                        slot_idx, wl_restart+1, g_wl_ctx.pc,
                        g_wl_ctx.gpr[3].u32[0], g_wl_ctx.gpr[4].u32[0]);
                fflush(stderr);
                if (g_wl_ctx.pc >= 0x20000u) {
                    fprintf(stderr, "[WL] slot %d: BSS idle at 0x%X — done\n",
                            slot_idx, g_wl_ctx.pc);
                    break;
                }
                g_wl_ctx.running = 1;
                /* Trace next 30 instructions of first restart to understand the code */
                if (wl_restart == 0) {
                    g_wl_ctx.trace_limit = 30;
                    g_wl_ctx.trace_count = 0;
                } else {
                    g_wl_ctx.trace_limit = 0;
                }
            } else if (wcode == 0x100) {
                fprintf(stderr, "[WL] slot %d: workload returned (stop 0x100)\n", slot_idx);
                break;
            } else if (wcode == 0x3FFF) {
                /* SPURS LV2 service trap — restart from next PC to explore further */
                fprintf(stderr, "[WL] slot %d: SPURS svc trap 0x3FFF at PC=0x%X r3=0x%X r4=0x%X (continuing)\n",
                        slot_idx, g_wl_ctx.pc,
                        g_wl_ctx.gpr[3].u32[0], g_wl_ctx.gpr[4].u32[0]);
                fflush(stderr);
                g_wl_ctx.running = 1;
                g_wl_ctx.trace_limit = 30;
                g_wl_ctx.trace_count = 0;
            } else {
                fprintf(stderr, "[WL] slot %d: stop=0x%X at PC=0x%X\n", slot_idx, wcode, g_wl_ctx.pc);
                break;
            }
        }
    }

    fprintf(stderr, "[WL] slot %d: done ran=%u insns stop=0x%X pc=0x%X\n",
            slot_idx, ran, g_wl_ctx.stop_code, g_wl_ctx.pc);
    fflush(stderr);
}

/* ---- SPURS kernel thread ------------------------------------------------ */
static DWORD WINAPI spurs_kernel_thread(LPVOID) {
    fprintf(stderr, "[SPURS] kernel thread starting (insn limit=unlimited)\n");
    fflush(stderr);

    /* Batch: run up to 1M instructions at a time, checking exit flag */
    while (!g_threads_should_exit) {
        if (!g_spurs_ctx.running) {
            uint32_t code = g_spurs_ctx.stop_code;
            fprintf(stderr, "[SPURS] kernel stopped (stop_code=0x%X insns=%llu)\n",
                    code, (unsigned long long)g_spurs_ctx.insn_count);
            fflush(stderr);
            /* Stopped waiting for mailbox — yield and retry */
            if (code == 0) {
                Sleep(1);
                /* Restart from current PC (rdch blocking case) */
                g_spurs_ctx.running = 1;
            } else {
                break;  /* real stop instruction */
            }
        }
        spu_run(&g_spurs_ctx, 100000);
    }

    fprintf(stderr, "[SPURS] kernel thread exiting (total insns=%llu)\n",
            (unsigned long long)g_spurs_ctx.insn_count);
    fflush(stderr);
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

extern "C" void spurs_start(void) {
    if (g_spurs_started) return;
    g_spurs_started = 1;

    spu_ctx_init(&g_spurs_ctx, 0, vm_base);

    /* Verbose: log DMA + channel ops.  Trace: first 50 insns only (entry path). */
    g_spurs_ctx.verbose = 1;
    g_spurs_ctx.trace_limit = 50;
    g_spurs_ctx.trace_count = 0;

    /* Load the SPURS kernel ELF from guest memory */
    const uint8_t *elf_ptr = vm_base + SPURS_KERNEL_A_GADDR;
    uint32_t elf_size = SPURS_KERNEL_ELF_SIZE;

    /* Sanity check: verify ELF magic at the expected guest address */
    if (elf_ptr[0] != 0x7F || elf_ptr[1] != 'E' || elf_ptr[2] != 'L' || elf_ptr[3] != 'F') {
        fprintf(stderr, "[SPURS] ERROR: no ELF magic at guest 0x%X (got %02X%02X%02X%02X)\n",
                SPURS_KERNEL_A_GADDR,
                elf_ptr[0], elf_ptr[1], elf_ptr[2], elf_ptr[3]);
        fflush(stderr);
        return;
    }

    int ret = spu_load_elf(&g_spurs_ctx, elf_ptr, elf_size);
    if (ret != 0) {
        fprintf(stderr, "[SPURS] ERROR: spu_load_elf failed (%d)\n", ret);
        fflush(stderr);
        return;
    }

    uint32_t entry_pc = g_spurs_ctx.pc;
    fprintf(stderr, "[SPURS] kernel ELF loaded from guest 0x%X, entry PC=0x%X\n",
            SPURS_KERNEL_A_GADDR, entry_pc);
    fflush(stderr);

    /* Provide a return address for the SPURS kernel.  On a real PS3, LV2 is
     * the caller and supplies r0.  In our emulation r0 defaults to 0 which
     * causes bi $0 to fall into LS[0x00] (all zeros = stop 0 loop).
     *
     * The kernel also writes 16 bytes of zeros to LS[0x00] via a stqx
     * instruction before executing bi $0, so any sentinel at LS[0x00] is
     * overwritten.  Use LS[0x40] instead (below the code at LS[0x80], and
     * never touched by any kernel store instruction).
     *
     * Place stop 0x100 at LS[0x40] and set r0 = 0x40 so that "bi $0"
     * (= bi r0) lands on our recognisable sentinel. */
    uint8_t stop100[4] = {0x00, 0x00, 0x01, 0x00};  /* stop 0x100 */
    memcpy(g_spurs_ctx.ls + 0x40, stop100, 4);
    g_spurs_ctx.gpr[0].u32[0] = 0x40;  /* r0 = return address = LS[0x40] */

    /* Patch LS[0x17C]: replace ceqi r2, r4, 8 with ilh r2, 1 to force the
     * dispatch branch at LS[0x184] (brnz r2, dispatch).
     *
     * The ceqi check extracts a type field from the SPURS EA via rothi+rotqbybi.
     * For it to pass naturally, SPURS_CTX_EA bits[23:20] must be 0 (need
     * r11.u8[2]=0 after rothi), but 0x70A000 has bits[23:20]=7 — same as
     * 0x700000.  Option B: ilh r2, 1 (= 0x40000082, op7=0x20 I16=1 RT=2)
     * sets r2.u32[0]=0x00010001 ≠ 0 → brnz always fires. */
    {
        uint32_t ilh_r2_1 = 0x40000082u;
        g_spurs_ctx.ls[0x17C] = (uint8_t)(ilh_r2_1 >> 24);
        g_spurs_ctx.ls[0x17D] = (uint8_t)(ilh_r2_1 >> 16);
        g_spurs_ctx.ls[0x17E] = (uint8_t)(ilh_r2_1 >>  8);
        g_spurs_ctx.ls[0x17F] = (uint8_t)(ilh_r2_1);
        fprintf(stderr, "[SPURS] patched LS[0x17C] with ilh r2,1 (0x%08X)\n", ilh_r2_1);
    }

    /* Patch LS[0x03C0]: replace brhnz r13, 0x298E0 → lnop to force dispatch.
     *
     * The `brhnz r13` at 0x03C0 checks if r13.high≠0 → idles.  r13 is
     * always 0x1F5F0 (from `ila r13, 0x1F5F0` at 0x0308) — upper halfword=1.
     * The check is a "no workload found" sentinel.  Patching to lnop forces
     * fall-through so the dispatch code at 0x03C4+ runs and we can trace
     * what the kernel does when it tries to dispatch a workload. */
    {
        uint32_t lnop_insn = 0x00200000u;   /* lnop = no-operation (odd pipe) */
        g_spurs_ctx.ls[0x3C0] = (uint8_t)(lnop_insn >> 24);
        g_spurs_ctx.ls[0x3C1] = (uint8_t)(lnop_insn >> 16);
        g_spurs_ctx.ls[0x3C2] = (uint8_t)(lnop_insn >>  8);
        g_spurs_ctx.ls[0x3C3] = (uint8_t)(lnop_insn);
        fprintf(stderr, "[SPURS] patched LS[0x03C0] brhnz-r13 → lnop (force dispatch)\n");
    }

    /* Set up initial arguments:
     *   GPR[3] = EA of SPURS management area (low 32 bits)
     *   GPR[4] = EA high 32 bits (always 0 on PS3)
     * The SPURS kernel DMA's the management area from this EA into its LS. */
    g_spurs_ctx.gpr[3].u32[0] = SPURS_CTX_EA;
    g_spurs_ctx.gpr[3].u32[1] = 0;
    g_spurs_ctx.gpr[3].u32[2] = 0;
    g_spurs_ctx.gpr[3].u32[3] = 0;
    g_spurs_ctx.gpr[4].u32[0] = 0;  /* EA high = 0 */

    /* The kernel encodes the return address across three rotmai instructions:
     *   LS[0x138]: rotmai r0, r86, 511  → r0 = r86 >> 1   (sh=1)
     *   LS[0x14C]: rotmai r0, r55, 205  → r0 = r55 >> 51  (sh=51, overwrites)
     *   LS[0x150]: rotmai r0, r70, 245  → r0 = r70 >> 2   (sh=2, final value)
     * Then "bi r0" branches to the return address (LS[0x40]).
     * On a real PS3, LV2 sets r86,r55,r70 to encode return_addr in each step.
     * Our sentinel is LS[0x40], so the last rotmai needs r70 = 0x40 << 2 = 0x100. */
    /* Each "bi r0" (function return) uses r0 = return_addr = LS[0x40].
     * The kernel computes r0 via multiple rotmai/rotmi instructions from different
     * source registers.  On a real PS3, LV2 sets these registers to encode
     * return_addr * 2^sh.  Our sentinel is LS[0x40], so for each instruction
     * with shift sh: src_reg = 0x40 << sh.
     *
     * Path 1 (LS[0x138..0x154], first pass through entry):
     *   LS[0x138]: rotmai r0, r86, I7=127 → sh=1   → r86=0x40<<1=0x80
     *   LS[0x14C]: rotmai r0, r55, I7=77  → sh=51  (overwritten by next)
     *   LS[0x150]: rotmai r0, r70, I7=117 → sh=11  → r70=0x40<<11=0x20000
     *
     * Path 2 (LS[0x022C..0x027C], second code region):
     *   LS[0x0254]: rotmi r0, r89, I7=127 → sh=1   → r89=0x40<<1=0x80 */
    g_spurs_ctx.gpr[86].u32[0] = 0x80;     /* r86 >> 1  = 0x40 (LS[0x138]) */
    g_spurs_ctx.gpr[70].u32[0] = 0x20000;  /* r70 >> 11 = 0x40 (LS[0x150]) */
    g_spurs_ctx.gpr[89].u32[0] = 0x80;     /* r89 >> 1  = 0x40 (LS[0x254]) */
    g_spurs_ctx.gpr[86].u32[1] = 0;
    g_spurs_ctx.gpr[86].u32[2] = 0;
    g_spurs_ctx.gpr[86].u32[3] = 0;

    /* SPURS type field encoded in r3 upper word.
     * The kernel does: r11=rothi(r3,12); r4=rotqbybi(r11,sh=15); ceqi r2,r4,8.
     * For ceqi to pass, r4.u32[0] must equal 8.
     * Working backwards: r4.u32[0]=8 requires r11.byte[15]=0x08, which requires
     * r3.u16[7]=0x8000 (rotate(0x8000,12)→0x0800, high byte=0x08).
     * r3.u16[7]=0x8000 means r3.u32[3]=0x80000000 in LE host storage. */
    g_spurs_ctx.gpr[3].u32[3] = 0x80000000u;  /* SPURS type=8 in high word of r3 */

    /* Run synchronously for a diagnostic burst so we can see the first DMA
       operations regardless of thread scheduling latency.  After this burst
       the kernel will likely be blocked on rdch (waiting for PPU mailbox).
       NOTE: The first DMA is at LS[0x138D4], ~20K instructions from entry;
       we need enough iterations to get there. */
    fprintf(stderr, "[SPURS] running diagnostic burst (2M insns, verbose=1)...\n");
    fflush(stderr);

    /* Run with restarts to get past init stops and the function-return jump.
     * stop 0    = SPURS idle/yield: restart from current PC
     * stop 0x100 = kernel "returned" (bi $0 → LS[0x00]): restart from entry
     * Other stop = real halt, exit loop */
    uint32_t total = 0;
    for (int restart = 0; restart < 200; restart++) {
        if (!g_spurs_ctx.running) {
            uint32_t code = g_spurs_ctx.stop_code;
            if (code == 0) {
                /* SPURS idle — continue from current PC */
                fprintf(stderr, "[SPURS] restart %d (idle) at PC=0x%X insns=%u r0=0x%X r4=0x%X\n",
                        restart+1, g_spurs_ctx.pc, total, g_spurs_ctx.gpr[0].u32[0],
                        g_spurs_ctx.gpr[4].u32[0]);
                fflush(stderr);
                /* Dispatch signal: kernel stopped at LS[0x04..0x3C] = "run workload".
                 * These 15 stops are the kernel signalling LV2 to start workload threads. */
                if (g_spurs_ctx.pc >= 0x04u && g_spurs_ctx.pc <= 0x3Cu) {
                    int slot = (int)((g_spurs_ctx.pc - 0x04u) / 4u);
                    fprintf(stderr, "[SPURS] dispatch slot %d at PC=0x%X\n",
                            slot, g_spurs_ctx.pc);
                    fflush(stderr);
                    spurs_run_workload(slot);
                    g_spurs_ctx.running = 1;  /* kernel continues from next instruction */
                    total += spu_run(&g_spurs_ctx, 200000);
                    continue;
                }

                /* End burst when kernel idles in BSS/data regions past the code section.
                 * 0x298E0: old "no workload" idle; 0x20600+: new "wait for completion" idle.
                 * Stop the burst at either, so we don't burn restarts on sequential stops. */
                if (g_spurs_ctx.pc >= 0x29000u || g_spurs_ctx.pc >= 0x20600u) {
                    fprintf(stderr, "[SPURS] kernel post-dispatch idle at PC=0x%X — ending burst\n",
                            g_spurs_ctx.pc);
                    fflush(stderr);
                    g_spurs_ctx.running = 1;
                    break;
                }
                /* Enable per-instruction trace for the dispatch run (restart=1 in loop).
                 * Note: restart=0 is the initial spu_run; the if-block first fires
                 * at restart=1 (after the 34-insn entry pass hits its first stop 0). */
                if (restart == 1) {
                    g_spurs_ctx.trace_limit = 200;
                    g_spurs_ctx.trace_count = 0;
                    fprintf(stderr, "[SPURS] TRACE dispatch run (200 insns from brhnz)\n");
                    fflush(stderr);
                } else {
                    g_spurs_ctx.trace_limit = 0;
                }
                g_spurs_ctx.running = 1;
            } else if (code == 0x100) {
                /* Kernel returned (bi $0 → stop 0x100 sentinel) — restart from entry */
                fprintf(stderr, "[SPURS] restart %d (returned) → entry 0x%X insns=%u\n",
                        restart+1, entry_pc, total);
                fflush(stderr);
                g_spurs_ctx.pc = entry_pc;
                g_spurs_ctx.running = 1;
                /* Reset initial arguments for the new dispatch cycle */
                g_spurs_ctx.gpr[3].u32[0] = SPURS_CTX_EA;
                g_spurs_ctx.gpr[3].u32[3] = 0x80000000u;  /* preserve type field */
                g_spurs_ctx.gpr[4].u32[0] = 0;
                g_spurs_ctx.gpr[0].u32[0] = 0x40;  /* restore return addr */
                /* Re-place sentinel (kernel may have overwritten LS[0x40]) */
                uint8_t s[4] = {0x00, 0x00, 0x01, 0x00};
                memcpy(g_spurs_ctx.ls + 0x40, s, 4);
            } else {
                break;  /* real halt */
            }
        }
        total += spu_run(&g_spurs_ctx, 200000);
    }
    uint32_t ran = total;
    fprintf(stderr, "[SPURS] burst done: ran=%u insns=%llu pc=0x%X running=%d stop=0x%X\n",
            ran, (unsigned long long)g_spurs_ctx.insn_count,
            g_spurs_ctx.pc, g_spurs_ctx.running, g_spurs_ctx.stop_code);
    fflush(stderr);

    /* Disable verbose to avoid log flood during the game's main execution */
    g_spurs_ctx.verbose = 0;

    /* If the kernel stopped (blocking rdch — waiting for PPU mailbox), continue
       running it in a background thread so it processes events as they arrive. */
    HANDLE h = CreateThread(nullptr, 256*1024, spurs_kernel_thread, nullptr, 0, nullptr);
    if (!h) {
        fprintf(stderr, "[SPURS] WARNING: CreateThread failed (%lu), kernel remains stopped\n",
                GetLastError());
        fflush(stderr);
        return;
    }
    CloseHandle(h);
    fprintf(stderr, "[SPURS] kernel thread launched (background)\n");
    fflush(stderr);
}

/* Disable verbose logging after enough trace data has been collected */
extern "C" void spurs_verbose_off(void) {
    g_spurs_ctx.verbose = 0;
}
