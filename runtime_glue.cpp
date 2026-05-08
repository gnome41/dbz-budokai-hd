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

/* --------------------------------------------------------------------------
 * Thread runtime
 * --------------------------------------------------------------------------
 * Guest stacks for additional threads are allocated below the main-thread
 * stack region (0xD0000000).  Each thread gets THREAD_STACK_SIZE bytes,
 * committed on demand from the 4 GB reserved region.
 *
 *   thread 1: top = 0xCFFF0000  (stack 0xCFF00000–0xCFFF0000)
 *   thread 2: top = 0xCEFF0000  (stack 0xCEF00000–0xCEFF0000)
 *   …
 */
#define THREAD_STACK_SIZE  0x00100000u   /* 1 MB per thread */
#define THREAD_STACK_BIAS  0x00010000u   /* guard/align offset from top */

static CRITICAL_SECTION g_thread_cs;
static bool             g_thread_cs_init = false;
static uint32_t         g_thread_stack_cursor = 0xCFFF0000u; /* next top */
static volatile uint32_t g_thread_id_next = 1;

/* Table of live thread handles so the main thread can join them */
#define MAX_THREADS 64
static HANDLE   g_thread_handles[MAX_THREADS];
static uint32_t g_thread_ids   [MAX_THREADS];
static int      g_thread_count = 0;

/* Per-thread creation parameters (heap-allocated, freed by the thread) */
struct PpuThreadParam {
    uint32_t entry_addr;   /* resolved guest function address (post-OPD) */
    uint32_t toc;          /* TOC value for this thread */
    uint64_t arg;          /* r3 passed to the thread entry */
    uint32_t stack_top;    /* guest address of the initial sp */
    uint32_t thread_id;    /* our synthetic thread ID */
};

/* Forward declarations for address resolvers defined in the generated code */
extern "C" void (*ppu_resolve_addr (uint64_t addr))(ppu_context*);
extern "C" void (*ppu_resolve_extra(uint64_t addr))(ppu_context*);

static DWORD WINAPI ppu_thread_proc(LPVOID param) {
    PpuThreadParam* tp = (PpuThreadParam*)param;

    ppu_context ctx = {};
    ctx.gpr[1] = tp->stack_top;
    ctx.gpr[2] = tp->toc;
    ctx.gpr[3] = tp->arg;

    uint32_t tid   = tp->thread_id;
    uint32_t entry = tp->entry_addr;
    free(tp);

    fprintf(stderr, "[THREAD %u] starting at 0x%08X sp=0x%08X\n", tid, entry, (uint32_t)ctx.gpr[1]);

    auto fn = ppu_resolve_addr(entry);
    if (!fn) fn = ppu_resolve_extra(entry);
    if (fn) {
        fn(&ctx);
        /* Drain any pending trampoline left by the last call */
        while (g_trampoline_fn) {
            void(*tf)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            tf((void*)&ctx);
        }
    } else {
        fprintf(stderr, "[THREAD %u] UNRESOLVED entry 0x%08X — thread stubbed\n", tid, entry);
    }

    fprintf(stderr, "[THREAD %u] exited (r3=0x%llX)\n", tid, (unsigned long long)ctx.gpr[3]);
    return 0;
}

static uint32_t alloc_thread_stack() {
    EnterCriticalSection(&g_thread_cs);
    uint32_t top = g_thread_stack_cursor;
    g_thread_stack_cursor -= THREAD_STACK_SIZE;
    LeaveCriticalSection(&g_thread_cs);

    /* Commit the pages for this stack */
    uint32_t base = top - THREAD_STACK_SIZE;
    if (!VirtualAlloc(vm_base + base, THREAD_STACK_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "[THREAD] ERROR: failed to commit stack at guest 0x%08X\n", base);
        return 0;
    }
    /* Return sp pointing just below the very top (standard ABI back-chain slot) */
    return top - THREAD_STACK_BIAS;
}

static void thread_table_add(uint32_t tid, HANDLE h) {
    EnterCriticalSection(&g_thread_cs);
    if (g_thread_count < MAX_THREADS) {
        g_thread_handles[g_thread_count] = h;
        g_thread_ids   [g_thread_count] = tid;
        ++g_thread_count;
    }
    LeaveCriticalSection(&g_thread_cs);
}

static HANDLE thread_table_find(uint32_t tid) {
    EnterCriticalSection(&g_thread_cs);
    HANDLE h = NULL;
    for (int i = 0; i < g_thread_count; i++) {
        if (g_thread_ids[i] == tid) { h = g_thread_handles[i]; break; }
    }
    LeaveCriticalSection(&g_thread_cs);
    return h;
}

/* --------------------------------------------------------------------------
 * SDK thread trampoline
 * --------------------------------------------------------------------------
 * The ps3recomp SDK's sys_ppu_thread_create (in runtime/syscalls/sys_ppu_thread.c)
 * calls g_ppu_thread_entry_trampoline(ctx) to dispatch into recompiled code.
 * ctx->cia holds the guest entry address as passed by the game.
 *
 * On PS3, a function pointer is an OPD address: [code_ptr u32, toc_ptr u32].
 * The game passes the OPD address as r4 to sys_ppu_thread_create; the SDK
 * stores it verbatim in ctx->cia.  We try resolving it as a direct code
 * address first (catches any callers that already pass code), then fall back
 * to dereferencing as an OPD.
 * -------------------------------------------------------------------------- */

/* Forward declared by the SDK header — defined in sys_ppu_thread.c */
extern "C" void (*g_ppu_thread_entry_trampoline)(ppu_context*);

static void ppu_sdk_thread_trampoline(ppu_context* ctx) {
    uint32_t candidate = (uint32_t)ctx->cia;

    /* Try as direct code address first */
    auto fn = ppu_resolve_addr(candidate);
    if (!fn) fn = ppu_resolve_extra(candidate);

    if (!fn) {
        /* Dereference as OPD: [code u32 @ candidate, toc u32 @ candidate+4] */
        uint32_t code_addr = vm_read32(candidate);
        uint32_t toc       = vm_read32(candidate + 4);
        fprintf(stderr, "[THREAD %llu] OPD@0x%08X → code=0x%08X toc=0x%08X\n",
                (unsigned long long)ctx->thread_id, candidate, code_addr, toc);
        if (toc) ctx->gpr[2] = toc;
        fn = ppu_resolve_addr(code_addr);
        if (!fn) fn = ppu_resolve_extra(code_addr);
        if (fn) candidate = code_addr;
    }

    if (fn) {
        fprintf(stderr, "[THREAD %llu] dispatching to 0x%08X\n",
                (unsigned long long)ctx->thread_id, candidate);
        fn(ctx);
        /* Drain any pending trampoline left by the last call */
        while (g_trampoline_fn) {
            void(*tf)(void*) = g_trampoline_fn;
            g_trampoline_fn  = nullptr;
            tf((void*)ctx);
        }
    } else {
        fprintf(stderr, "[THREAD %llu] UNRESOLVED entry 0x%08X — thread stubbed\n",
                (unsigned long long)ctx->thread_id, (uint32_t)ctx->cia);
    }

    fprintf(stderr, "[THREAD %llu] exited (r3=0x%llX)\n",
            (unsigned long long)ctx->thread_id, (unsigned long long)ctx->gpr[3]);
}

/* Called from main() before vm_init so the critical section is ready */
extern "C" void thread_runtime_init() {
    InitializeCriticalSection(&g_thread_cs);
    g_thread_cs_init = true;

    /* Wire the SDK thread trampoline so SDK-created threads dispatch into
       recompiled PPU code instead of silently no-op'ing */
    g_ppu_thread_entry_trampoline = ppu_sdk_thread_trampoline;
}

/* Wait for all created game threads to finish (called from main after _start returns) */
extern "C" void thread_runtime_join_all() {
    if (!g_thread_cs_init) return;
    EnterCriticalSection(&g_thread_cs);
    int n = g_thread_count;
    HANDLE handles[MAX_THREADS];
    for (int i = 0; i < n; i++) handles[i] = g_thread_handles[i];
    LeaveCriticalSection(&g_thread_cs);

    if (n > 0) {
        fprintf(stderr, "[RUNTIME] waiting for %d game thread(s)...\n", n);
        WaitForMultipleObjects(n, handles, TRUE, INFINITE);
        fprintf(stderr, "[RUNTIME] all game threads finished\n");
    }
}

/* Memory access — big-endian byte-swap, addr truncated to 32-bit.
 *
 * Guest addresses 0x0000-0xFFFF are valid on real PS3 (PPU local storage).
 * We commit that range in vm_init() (main.cpp).  Log accesses below 0x1000
 * for visibility but always perform the actual read/write — do NOT skip. */
extern "C" uint8_t vm_read8(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-READ8]  guest addr=0x%08X\n", a); fflush(stderr); }
    return vm_base[a];
}
extern "C" uint16_t vm_read16(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-READ16] guest addr=0x%08X\n", a); fflush(stderr); }
    return (uint16_t)(((uint16_t)vm_base[a] << 8) | vm_base[a + 1]);
}
extern "C" uint32_t vm_read32(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-READ32] guest addr=0x%08X\n", a); fflush(stderr); }
    return ((uint32_t)vm_base[a]     << 24) | ((uint32_t)vm_base[a + 1] << 16) |
           ((uint32_t)vm_base[a + 2] <<  8) |  (uint32_t)vm_base[a + 3];
}
extern "C" uint64_t vm_read64(uint64_t addr) {
    return ((uint64_t)vm_read32(addr) << 32) | vm_read32(addr + 4);
}
extern "C" void vm_write8(uint64_t addr, uint8_t val) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-WRITE8]  guest addr=0x%08X val=0x%02X\n", a, val); fflush(stderr); }
    vm_base[a] = val;
}
extern "C" void vm_write16(uint64_t addr, uint16_t val) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-WRITE16] guest addr=0x%08X val=0x%04X\n", a, (unsigned)val); fflush(stderr); }
    vm_base[a]     = (uint8_t)(val >> 8);
    vm_base[a + 1] = (uint8_t)(val);
}
extern "C" void vm_write32(uint64_t addr, uint32_t val) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) { fprintf(stderr, "[LOW-WRITE32] guest addr=0x%08X val=0x%08X\n", a, val); fflush(stderr); }
    /* The SPURS init code (func_000CE77C) zeros [0x27F81C] (workload list head)
       as part of its own initialization.  Since we have no real SPU runtime,
       we intercept that zero-write and keep our synthetic dispatch chain in place.
       Any non-zero write is allowed through so the real chain can be updated if
       the game ever enqueues a real workload (unlikely without SPU emulation). */
    if (a == 0x27F81Cu) {
        fprintf(stderr, "[MONITOR] vm_write32(0x27F81C) = 0x%08X%s\n",
                val, (val == 0) ? " → redirected to 0x70A000" : ""); fflush(stderr);
        if (val == 0) val = 0x0070A000u;  /* preserve synthetic workload chain */
    }
    vm_base[a]     = (uint8_t)(val >> 24);
    vm_base[a + 1] = (uint8_t)(val >> 16);
    vm_base[a + 2] = (uint8_t)(val >>  8);
    vm_base[a + 3] = (uint8_t)(val);
}
extern "C" void vm_write64(uint64_t addr, uint64_t val) {
    vm_write32(addr,     (uint32_t)(val >> 32));
    vm_write32(addr + 4, (uint32_t)(val));
}

/* LV2 syscall dispatcher */
extern "C" void lv2_syscall(ppu_context* ctx) {
    uint32_t sysnum = (uint32_t)ctx->gpr[11];

    switch (sysnum) {

    /* ---- Threading -------------------------------------------------------- */

    /* sys_ppu_thread_create (44)
       r3: out sys_ppu_thread_t*   r4: OPD ptr (code, toc)
       r5: arg (→ r3 in new thread) r6: priority  r7: stack_size
       r8: flags (1=joinable)       r9: name ptr */
    case 44: {
        uint32_t id_out   = (uint32_t)ctx->gpr[3];
        uint32_t opd_ptr  = (uint32_t)ctx->gpr[4];
        uint64_t arg      = ctx->gpr[5];
        uint32_t prio     = (uint32_t)ctx->gpr[6];
        uint32_t stk_sz   = (uint32_t)ctx->gpr[7];
        uint32_t flags    = (uint32_t)ctx->gpr[8];
        uint32_t name_ptr = (uint32_t)ctx->gpr[9];

        /* Decode OPD: [code_ptr u32, toc_ptr u32] */
        uint32_t code_addr = vm_read32(opd_ptr);
        uint32_t toc       = vm_read32(opd_ptr + 4);

        char name_buf[64] = "<unnamed>";
        if (name_ptr > 0x10000u && name_ptr < 0x10010000u)
            strncpy(name_buf, (const char*)(vm_base + name_ptr), 63);

        fprintf(stderr,
            "[LV2] sys_ppu_thread_create name=\"%s\" opd=0x%08X code=0x%08X toc=0x%08X"
            " arg=0x%llX stk=0x%X prio=%d flags=0x%X\n",
            name_buf, opd_ptr, code_addr, toc,
            (unsigned long long)arg, stk_sz, (int)prio, flags);

        /* Allocate a guest stack for this thread */
        uint32_t stack_top = alloc_thread_stack();
        if (!stack_top) {
            fprintf(stderr, "[LV2] sys_ppu_thread_create: stack alloc failed\n");
            ctx->gpr[3] = (uint64_t)(int64_t)-1LL; /* CELL_ENOMEM */
            break;
        }

        /* Build thread params */
        PpuThreadParam* tp = (PpuThreadParam*)malloc(sizeof(PpuThreadParam));
        tp->entry_addr = code_addr;
        tp->toc        = toc;
        tp->arg        = arg;
        tp->stack_top  = stack_top;
        tp->thread_id  = InterlockedIncrement((volatile LONG*)&g_thread_id_next) - 1;

        /* Write synthetic thread ID back to guest */
        if (id_out) vm_write32(id_out, tp->thread_id);

        HANDLE h = CreateThread(NULL, 0, ppu_thread_proc, tp, 0, NULL);
        if (!h) {
            fprintf(stderr, "[LV2] sys_ppu_thread_create: CreateThread failed (%lu)\n",
                    GetLastError());
            free(tp);
            ctx->gpr[3] = (uint64_t)(int64_t)-1LL;
            break;
        }
        thread_table_add(tp->thread_id, h);

        ctx->gpr[3] = 0; /* CELL_OK */
        break;
    }

    /* sys_ppu_thread_exit (41): r3 = exit value */
    case 41:
        fprintf(stderr, "[LV2] sys_ppu_thread_exit (code=0x%llX)\n",
                (unsigned long long)ctx->gpr[3]);
        ExitThread((DWORD)ctx->gpr[3]);
        break; /* unreachable */

    /* sys_ppu_thread_yield (45) */
    case 45:
        SwitchToThread();
        ctx->gpr[3] = 0;
        break;

    /* sys_ppu_thread_join (43): r3 = sys_ppu_thread_t, r4 = out exit_status* */
    case 43: {
        uint32_t tid    = (uint32_t)ctx->gpr[3];
        uint32_t out_st = (uint32_t)ctx->gpr[4];
        HANDLE h = thread_table_find(tid);
        if (h) {
            WaitForSingleObject(h, INFINITE);
            DWORD exit_code = 0;
            GetExitCodeThread(h, &exit_code);
            if (out_st) vm_write32(out_st, exit_code);
        } else {
            fprintf(stderr, "[LV2] sys_ppu_thread_join: unknown tid %u\n", tid);
        }
        ctx->gpr[3] = 0;
        break;
    }

    /* sys_ppu_thread_get_id (48): returns current thread's synthetic ID.
       We don't track per-thread IDs yet — return a placeholder. */
    case 48:
        ctx->gpr[3] = 0xDEAD0001ull;
        break;

    /* ---- Mutexes ---------------------------------------------------------- */

    /* sys_mutex_create (90): r3=out mutex_id*, r4=attr_ptr → stub, return 0 */
    case 90: {
        static volatile uint32_t g_mutex_id = 0x1000;
        uint32_t id_out = (uint32_t)ctx->gpr[3];
        uint32_t mid = InterlockedIncrement((volatile LONG*)&g_mutex_id);
        if (id_out) vm_write32(id_out, mid);
        fprintf(stderr, "[LV2] sys_mutex_create → id=0x%X\n", mid);
        ctx->gpr[3] = 0;
        break;
    }
    case 91:  /* sys_mutex_destroy */
        ctx->gpr[3] = 0; break;
    case 92:  /* sys_mutex_lock   */
        ctx->gpr[3] = 0; break;
    case 93:  /* sys_mutex_trylock */
        ctx->gpr[3] = 0; break;
    case 94:  /* sys_mutex_unlock */
        ctx->gpr[3] = 0; break;

    /* ---- Condition variables ---------------------------------------------- */
    case 95:  /* sys_cond_create  */
        ctx->gpr[3] = 0; break;
    case 96:  /* sys_cond_destroy */
        ctx->gpr[3] = 0; break;
    case 97:  /* sys_cond_wait    */
        SwitchToThread();
        ctx->gpr[3] = 0; break;
    case 98:  /* sys_cond_signal  */
        ctx->gpr[3] = 0; break;
    case 99:  /* sys_cond_signal_all */
        ctx->gpr[3] = 0; break;

    /* ---- Lightweight mutexes (lwmutex) ------------------------------------ */
    case 612: /* sys_lwmutex_create  */
        ctx->gpr[3] = 0; break;
    case 613: /* sys_lwmutex_destroy */
        ctx->gpr[3] = 0; break;
    case 614: /* sys_lwmutex_lock    */
        ctx->gpr[3] = 0; break;
    case 615: /* sys_lwmutex_trylock */
        ctx->gpr[3] = 0; break;
    case 616: /* sys_lwmutex_unlock  */
        ctx->gpr[3] = 0; break;

    /* ---- Semaphores -------------------------------------------------------- */
    case 84:  /* sys_semaphore_create  */
        ctx->gpr[3] = 0; break;
    case 85:  /* sys_semaphore_destroy */
        ctx->gpr[3] = 0; break;
    case 86:  /* sys_semaphore_wait    */
        SwitchToThread();
        ctx->gpr[3] = 0; break;
    case 87:  /* sys_semaphore_trywait */
        ctx->gpr[3] = 0; break;
    case 88:  /* sys_semaphore_post    */
        ctx->gpr[3] = 0; break;

    /* ---- Event queues ----------------------------------------------------- */
    case 125: /* sys_event_queue_create  */
        ctx->gpr[3] = 0; break;
    case 126: /* sys_event_queue_destroy */
        ctx->gpr[3] = 0; break;
    case 127: /* sys_event_queue_receive */
        SwitchToThread();
        ctx->gpr[3] = 0; break;
    case 128: /* sys_event_queue_tryreceive */
        ctx->gpr[3] = 0; break;
    case 130: /* sys_event_port_create   */
        ctx->gpr[3] = 0; break;
    case 131: /* sys_event_port_destroy  */
        ctx->gpr[3] = 0; break;
    case 132: /* sys_event_port_connect_local */
        ctx->gpr[3] = 0; break;
    case 134: /* sys_event_port_send     */
        ctx->gpr[3] = 0; break;

    /* ---- Time -------------------------------------------------------------- */
    case 141: /* sys_time_get_system_time: returns µs since boot */
        ctx->gpr[3] = (uint64_t)GetTickCount64() * 1000ull;
        break;
    case 145: /* sys_time_get_timebase_frequency: PS3 timebase = 79.8 MHz */
        ctx->gpr[3] = 79800000ull;
        break;

    /* ---- Memory ----------------------------------------------------------- */
    case 348: /* sys_memory_allocate: r3=size r4=align r5=out_ptr* */
    case 349: /* sys_memory_allocate_from_container */
        fprintf(stderr, "[LV2] sys_memory_allocate sz=0x%llX align=0x%llX\n",
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4]);
        ctx->gpr[3] = (uint64_t)(int64_t)-1LL; /* CELL_ENOMEM — caller must handle */
        break;
    case 350: /* sys_memory_free */
        ctx->gpr[3] = 0; break;

    /* ---- TTY output ------------------------------------------------------- */
    case 403: {
        uint32_t fd  = (uint32_t)ctx->gpr[3];
        uint32_t buf = (uint32_t)ctx->gpr[4];
        uint32_t len = (uint32_t)ctx->gpr[5];
        if (len > 0 && len < 0x10000u && buf >= 0x10000u) {
            const char* s = (const char*)(vm_base + buf);
            fprintf(fd == 1 ? stdout : stderr, "[TTY%u] %.*s", fd, (int)len, s);
        }
        if (ctx->gpr[6]) vm_write32(ctx->gpr[6], len);
        ctx->gpr[3] = 0;
        break;
    }

    /* ---- Watchdog / abort ------------------------------------------------- */
    case 988:
        fprintf(stderr, "[LV2] sys_988 (r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
        if (ctx->gpr[3] == 4) {
            g_abort_called = true;
            uint32_t sp = (uint32_t)ctx->gpr[1];
            uint32_t saved_lr = vm_read32(sp + 0xD0);
            fprintf(stderr, "[ABORT] func_000D91E4 caller LR=0x%08X\n", saved_lr);
            uint32_t frame = vm_read32(sp);
            for (int i = 0; i < 4 && frame > 0x10000u && frame < 0x10000000u; i++) {
                uint32_t frame_lr = vm_read32(frame + 0x10);
                fprintf(stderr, "[ABORT] frame[%d] sp=0x%08X lr=0x%08X\n", i, frame, frame_lr);
                uint32_t next = vm_read32(frame);
                if (next == frame || next == 0) break;
                frame = next;
            }
        }
        ctx->gpr[3] = 0;
        break;

    /* ---- Unknown SPU/SPURS/event syscalls triggered by init chain ---------- */

    /* sys_0x323 (803): triggered by func_000CE77C (bit13=1 path → D54B0) when
       struct+8 < struct+0x10; purpose unknown, likely SPURS/event-port setup.
       Return CELL_OK so callers continue. */
    case 0x323:
        fprintf(stderr, "[LV2] sys_0x323 (r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
        ctx->gpr[3] = 0;
        break;

    /* sys_0x324 (804): triggered by func_000D5450 (D544C) when struct+4 >= 2;
       purpose unknown.  Return CELL_OK so callers continue. */
    case 0x324:
        fprintf(stderr, "[LV2] sys_0x324 (r3=0x%llX r4=0x%llX r5=0x%llX)\n",
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5]);
        ctx->gpr[3] = 0;
        break;

    default:
        fprintf(stderr, "[LV2] syscall %u (r3=0x%llX r4=0x%llX r5=0x%llX r6=0x%llX)\n",
                sysnum,
                (unsigned long long)ctx->gpr[3],
                (unsigned long long)ctx->gpr[4],
                (unsigned long long)ctx->gpr[5],
                (unsigned long long)ctx->gpr[6]);
        ctx->gpr[3] = 0;
        break;
    }
}

/* Indirect call dispatcher — resolves guest address in CTR to recompiled host fn */

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
