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

/* Maximum ELF size for the SPURS kernel (entry 0xD0, code up to ~0x20560) */
#define SPURS_KERNEL_ELF_SIZE  0x21000u

/* SPURS management area EA (the synthetic context we set up in main.cpp) */
#define SPURS_CTX_EA   0x700000u

static spu_ctx_t g_spurs_ctx;
static int       g_spurs_started = 0;

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

    /* Verbose: log DMA ops + channel reads. Trace: log first 80 insns of restart-2. */
    g_spurs_ctx.verbose = 1;
    g_spurs_ctx.trace_limit = 0;  /* will be set for restart-2 */

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
    for (int restart = 0; restart < 20; restart++) {
        if (!g_spurs_ctx.running) {
            uint32_t code = g_spurs_ctx.stop_code;
            if (code == 0) {
                /* SPURS idle — continue from current PC */
                fprintf(stderr, "[SPURS] restart %d (idle) at PC=0x%X insns=%u r0=0x%X\n",
                        restart+1, g_spurs_ctx.pc, total, g_spurs_ctx.gpr[0].u32[0]);
                fflush(stderr);
                /* Enable per-instruction trace for restart 2 (the critical 40-insn batch) */
                if (restart == 1) {
                    g_spurs_ctx.trace_limit = 80;
                    g_spurs_ctx.trace_count = 0;
                    fprintf(stderr, "[SPURS] TRACE ENABLED for restart 2 (80 insns)\n");
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
