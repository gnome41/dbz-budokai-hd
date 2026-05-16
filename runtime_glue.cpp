#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
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

/* Per-PPU-thread synthetic ID (set at thread-proc entry, 0 = main thread) */
extern "C" __declspec(thread) uint32_t g_ppu_current_thread_id = 0;

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

    g_ppu_current_thread_id = tid;
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

/* ==========================================================================
 * LV2 kernel-object pool — semaphores, mutexes, condvars, event queues/ports
 * ==========================================================================
 *
 * Every LV2 object is identified by a uint32_t ID (1..MAX_LV2_OBJ-1).
 * ID 0 is invalid.  The pool is a fixed array; IDs are re-used after free.
 *
 * lwmutex and lwcond operate on guest-memory structs (not IDs), so they use
 * separate maps keyed by guest address.
 */

#define MAX_LV2_OBJ  512
#define EVQ_CAP      128

enum Lv2Kind { LV2_FREE=0, LV2_SEM, LV2_MUTEX, LV2_COND, LV2_EVQ, LV2_EVPORT };

struct Lv2Event { uint64_t source, data1, data2, data3; };

struct Lv2Evq {
    CRITICAL_SECTION cs;
    HANDLE           avail;            /* counting semaphore: count = queued entries */
    Lv2Event         buf[EVQ_CAP];
    int              head, tail, cnt;
};

struct Lv2Obj {
    Lv2Kind kind;
    union {
        HANDLE sem;                    /* LV2_SEM  */
        struct {                       /* LV2_MUTEX */
            CRITICAL_SECTION cs;
        } mutex;
        struct {                       /* LV2_COND */
            CONDITION_VARIABLE cv;
            uint32_t           mutex_id;
        } cond;
        Lv2Evq* evq;                   /* LV2_EVQ   */
        struct {                       /* LV2_EVPORT */
            uint32_t queue_id;
            uint64_t name;
        } port;
    };
};

static CRITICAL_SECTION g_pool_cs;
static Lv2Obj           g_pool[MAX_LV2_OBJ];
static uint32_t         g_pool_cursor = 1;

/* lwmutex / lwcond maps: guest struct address → Windows primitive */
static std::unordered_map<uint32_t, CRITICAL_SECTION*>*   g_lwmutex_map  = nullptr;
static std::unordered_map<uint32_t, CONDITION_VARIABLE*>* g_lwcond_map   = nullptr;
static CRITICAL_SECTION                                    g_lw_map_cs;

static void lv2_pool_init() {
    InitializeCriticalSection(&g_pool_cs);
    InitializeCriticalSection(&g_lw_map_cs);
    memset(g_pool, 0, sizeof(g_pool));
    g_lwmutex_map = new std::unordered_map<uint32_t, CRITICAL_SECTION*>();
    g_lwcond_map  = new std::unordered_map<uint32_t, CONDITION_VARIABLE*>();
}

static uint32_t pool_alloc(Lv2Kind kind) {
    EnterCriticalSection(&g_pool_cs);
    for (uint32_t i = 0; i < MAX_LV2_OBJ - 1; i++) {
        uint32_t id = (g_pool_cursor - 1 + i) % (MAX_LV2_OBJ - 1) + 1;
        if (g_pool[id].kind == LV2_FREE) {
            memset(&g_pool[id], 0, sizeof(Lv2Obj));
            g_pool[id].kind = kind;
            g_pool_cursor = id % (MAX_LV2_OBJ - 1) + 1;
            LeaveCriticalSection(&g_pool_cs);
            return id;
        }
    }
    LeaveCriticalSection(&g_pool_cs);
    fprintf(stderr, "[LV2] POOL FULL kind=%d\n", (int)kind);
    return 0;
}

static Lv2Obj* pool_get(uint32_t id, Lv2Kind kind) {
    if (id == 0 || id >= MAX_LV2_OBJ) return nullptr;
    Lv2Obj* o = &g_pool[id];
    return (o->kind == kind) ? o : nullptr;
}

static void pool_free(uint32_t id) {
    if (id == 0 || id >= MAX_LV2_OBJ) return;
    EnterCriticalSection(&g_pool_cs);
    g_pool[id].kind = LV2_FREE;
    LeaveCriticalSection(&g_pool_cs);
}

/* Called from main() before vm_init so the critical section is ready */
extern "C" void thread_runtime_init() {
    InitializeCriticalSection(&g_thread_cs);
    g_thread_cs_init = true;
    lv2_pool_init();

    /* Main thread gets a well-known pseudo-ID */
    g_ppu_current_thread_id = 0xFFFF0000u;

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
    uint8_t v = vm_base[a];
    if (a < 0x1000) { fprintf(stderr, "[LOW-READ8]  guest addr=0x%08X val=0x%02X\n", a, v); fflush(stderr); }
    return v;
}
extern "C" uint16_t vm_read16(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    uint16_t v = (uint16_t)(((uint16_t)vm_base[a] << 8) | vm_base[a + 1]);
    if (a < 0x1000) { fprintf(stderr, "[LOW-READ16] guest addr=0x%08X val=0x%04X\n", a, v); fflush(stderr); }
    return v;
}
extern "C" uint32_t vm_read32(uint64_t addr) {
    uint32_t a = (uint32_t)addr;
    if (a < 0x1000) {
        static volatile int s_read4_done = 0;
        if (a == 4 && !s_read4_done) {
            s_read4_done = 1;
            /* Use DbgHelp to resolve host function names from the PDB */
            HANDLE hProc = GetCurrentProcess();
            SymInitialize(hProc, nullptr, TRUE);
            void* stack[32] = {};
            USHORT frames = CaptureStackBackTrace(0, 32, stack, nullptr);
            char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            fprintf(stderr, "[LOW-READ32-TRACE] first read of addr=0x4 (%u frames):\n", frames);
            for (USHORT i = 0; i < frames; i++) {
                DWORD64 disp = 0;
                if (SymFromAddr(hProc, (DWORD64)stack[i], &disp, sym))
                    fprintf(stderr, "  [%u] %s+0x%llX\n", i, sym->Name, (unsigned long long)disp);
                else
                    fprintf(stderr, "  [%u] %p (no symbol)\n", i, stack[i]);
            }
            fflush(stderr);
        }
        static uint32_t s_low32_count[0x1000] = {};
        uint32_t v32 = ((uint32_t)vm_base[a] << 24) | ((uint32_t)vm_base[a+1] << 16) |
                       ((uint32_t)vm_base[a+2] <<  8) |  (uint32_t)vm_base[a+3];
        if (++s_low32_count[a] <= 5)
            fprintf(stderr, "[LOW-READ32] guest addr=0x%08X val=0x%08X (count=%u)\n", a, v32, s_low32_count[a]);
        else if (s_low32_count[a] == 6)
            fprintf(stderr, "[LOW-READ32] addr=0x%08X further repeats suppressed\n", a);
        fflush(stderr);
        return v32;
    }
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
    if (a == 0x27F814u) {
        fprintf(stderr, "[MONITOR] vm_write32(0x27F814) = 0x%08X%s\n",
                val, (val == 0) ? " → redirected to 0x70B000" : ""); fflush(stderr);
        if (val == 0) val = 0x0070B000u;  /* preserve states 11/13/14 struct chain */
    }
    /* Null RSX backend: when PPU writes the RSX PUT register (0x10), update
     * the RSX GET register (0x00) to the same value so the game never stalls
     * waiting for RSX to "process" commands it submitted.  This makes every
     * RSX command-buffer flush appear to complete instantly. */
    if (a == 0x10u) {
        vm_base[0x00] = (uint8_t)(val >> 24);
        vm_base[0x01] = (uint8_t)(val >> 16);
        vm_base[0x02] = (uint8_t)(val >>  8);
        vm_base[0x03] = (uint8_t)(val);
    }
    /* Null RSX backend: RSX REF register (0x04) is polled by game code that
     * writes 0xFFFFFFFF as a "pending" sentinel and spins until RSX clears it.
     * Discard all writes so it stays at 0 — RSX "instantly acknowledges". */
    if (a == 0x4u) {
        fprintf(stderr, "[RSX-REF] write 0x%08X discarded (keeping REF=0)\n", val); fflush(stderr);
        return;
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

    /* sys_ppu_thread_get_id (48) */
    case 48:
        ctx->gpr[3] = g_ppu_current_thread_id ? g_ppu_current_thread_id : 0xFFFF0000u;
        break;

    /* ---- Semaphores -------------------------------------------------------- */

    /* sys_semaphore_create (84): r3=out*, r4=attr*, r5=initial, r6=max */
    case 84: {
        uint32_t out = (uint32_t)ctx->gpr[3];
        int32_t  ini = (int32_t)ctx->gpr[5];
        int32_t  mx  = (int32_t)ctx->gpr[6];
        uint32_t id  = pool_alloc(LV2_SEM);
        if (!id) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        g_pool[id].sem = CreateSemaphoreA(NULL, ini, mx > 0 ? mx : 0x7FFFFFFF, NULL);
        if (out) vm_write32(out, id);
        fprintf(stderr, "[LV2] sys_semaphore_create id=%u initial=%d max=%d\n", id, ini, mx);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_semaphore_destroy (85): r3=id */
    case 85: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_SEM);
        if (o) { CloseHandle(o->sem); pool_free(id); }
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_semaphore_wait (86): r3=id, r4=timeout_us (0=infinite) */
    case 86: {
        uint32_t id  = (uint32_t)ctx->gpr[3];
        uint64_t tus = ctx->gpr[4];
        Lv2Obj* o = pool_get(id, LV2_SEM);
        if (!o) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        DWORD wms = tus ? (DWORD)((tus + 999) / 1000) : INFINITE;
        fprintf(stderr, "[LV2] sys_semaphore_wait id=%u timeout=%llu us\n",
                id, (unsigned long long)tus);
        DWORD r = WaitForSingleObject(o->sem, wms);
        ctx->gpr[3] = (r == WAIT_TIMEOUT) ? 0x80410034ull : 0;
        break;
    }
    /* sys_semaphore_trywait (87): r3=id */
    case 87: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_SEM);
        if (!o) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        ctx->gpr[3] = (WaitForSingleObject(o->sem, 0) == WAIT_OBJECT_0) ? 0 : 0x80410034ull;
        break;
    }
    /* sys_semaphore_post (88): r3=id, r4=count */
    case 88: {
        uint32_t id  = (uint32_t)ctx->gpr[3];
        uint32_t cnt = (uint32_t)ctx->gpr[4];
        Lv2Obj* o = pool_get(id, LV2_SEM);
        if (o) ReleaseSemaphore(o->sem, cnt ? (LONG)cnt : 1, NULL);
        ctx->gpr[3] = 0;
        break;
    }

    /* ---- Mutexes ---------------------------------------------------------- */

    /* sys_mutex_create (90): r3=out*, r4=attr* */
    case 90: {
        uint32_t out = (uint32_t)ctx->gpr[3];
        uint32_t id  = pool_alloc(LV2_MUTEX);
        if (!id) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        InitializeCriticalSection(&g_pool[id].mutex.cs);
        if (out) vm_write32(out, id);
        fprintf(stderr, "[LV2] sys_mutex_create id=%u\n", id);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_mutex_destroy (91): r3=id */
    case 91: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_MUTEX);
        if (o) { DeleteCriticalSection(&o->mutex.cs); pool_free(id); }
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_mutex_lock (92): r3=id, r4=timeout_us */
    case 92: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_MUTEX);
        if (!o) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        EnterCriticalSection(&o->mutex.cs);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_mutex_trylock (93): r3=id */
    case 93: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_MUTEX);
        if (!o) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        ctx->gpr[3] = TryEnterCriticalSection(&o->mutex.cs) ? 0 : 0x8001000Bull;
        break;
    }
    /* sys_mutex_unlock (94): r3=id */
    case 94: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_MUTEX);
        if (o) LeaveCriticalSection(&o->mutex.cs);
        ctx->gpr[3] = 0;
        break;
    }

    /* ---- Condition variables ---------------------------------------------- */

    /* sys_cond_create (95): r3=out*, r4=mutex_id, r5=attr* */
    case 95: {
        uint32_t out  = (uint32_t)ctx->gpr[3];
        uint32_t mid  = (uint32_t)ctx->gpr[4];
        uint32_t id   = pool_alloc(LV2_COND);
        if (!id) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        InitializeConditionVariable(&g_pool[id].cond.cv);
        g_pool[id].cond.mutex_id = mid;
        if (out) vm_write32(out, id);
        fprintf(stderr, "[LV2] sys_cond_create id=%u mutex=%u\n", id, mid);
        ctx->gpr[3] = 0;
        break;
    }
    case 96: /* sys_cond_destroy */
        pool_free((uint32_t)ctx->gpr[3]);
        ctx->gpr[3] = 0; break;
    /* sys_cond_wait (97): r3=cond_id, r4=timeout_us */
    case 97: {
        uint32_t cid = (uint32_t)ctx->gpr[3];
        uint64_t tus = ctx->gpr[4];
        Lv2Obj* co = pool_get(cid, LV2_COND);
        if (!co) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        Lv2Obj* mo = pool_get(co->cond.mutex_id, LV2_MUTEX);
        if (!mo) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        DWORD wms = tus ? (DWORD)((tus + 999) / 1000) : INFINITE;
        fprintf(stderr, "[LV2] sys_cond_wait cond=%u mutex=%u\n", cid, co->cond.mutex_id);
        BOOL ok = SleepConditionVariableCS(&co->cond.cv, &mo->mutex.cs, wms);
        ctx->gpr[3] = ok ? 0 : 0x80410034ull;
        break;
    }
    case 98: { /* sys_cond_signal */
        Lv2Obj* o = pool_get((uint32_t)ctx->gpr[3], LV2_COND);
        if (o) WakeConditionVariable(&o->cond.cv);
        ctx->gpr[3] = 0; break;
    }
    case 99: { /* sys_cond_signal_all */
        Lv2Obj* o = pool_get((uint32_t)ctx->gpr[3], LV2_COND);
        if (o) WakeAllConditionVariable(&o->cond.cv);
        ctx->gpr[3] = 0; break;
    }

    /* ---- Lightweight mutexes (lwmutex) ------------------------------------ */

    /* sys_lwmutex_create (612): r3=guest lwmutex struct ptr, r4=attr* */
    case 612: {
        uint32_t lw = (uint32_t)ctx->gpr[3];
        EnterCriticalSection(&g_lw_map_cs);
        if (!g_lwmutex_map->count(lw)) {
            auto* cs = (CRITICAL_SECTION*)malloc(sizeof(CRITICAL_SECTION));
            InitializeCriticalSection(cs);
            (*g_lwmutex_map)[lw] = cs;
        }
        LeaveCriticalSection(&g_lw_map_cs);
        fprintf(stderr, "[LV2] sys_lwmutex_create lw=0x%08X\n", lw);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_lwmutex_destroy (613): r3=guest lwmutex struct ptr */
    case 613: {
        uint32_t lw = (uint32_t)ctx->gpr[3];
        EnterCriticalSection(&g_lw_map_cs);
        auto it = g_lwmutex_map->find(lw);
        if (it != g_lwmutex_map->end()) {
            DeleteCriticalSection(it->second);
            free(it->second);
            g_lwmutex_map->erase(it);
        }
        LeaveCriticalSection(&g_lw_map_cs);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_lwmutex_lock (614): r3=guest ptr, r4=timeout_us */
    case 614: {
        uint32_t lw = (uint32_t)ctx->gpr[3];
        CRITICAL_SECTION* cs = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwmutex_map->find(lw); if (it != g_lwmutex_map->end()) cs = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cs) { fprintf(stderr, "[LV2] sys_lwmutex_lock lw=0x%08X\n", lw); EnterCriticalSection(cs); }
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_lwmutex_trylock (615): r3=guest ptr */
    case 615: {
        uint32_t lw = (uint32_t)ctx->gpr[3];
        CRITICAL_SECTION* cs = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwmutex_map->find(lw); if (it != g_lwmutex_map->end()) cs = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        ctx->gpr[3] = (cs && TryEnterCriticalSection(cs)) ? 0 : 0x8001000Bull;
        break;
    }
    /* sys_lwmutex_unlock (616): r3=guest ptr */
    case 616: {
        uint32_t lw = (uint32_t)ctx->gpr[3];
        CRITICAL_SECTION* cs = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwmutex_map->find(lw); if (it != g_lwmutex_map->end()) cs = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cs) LeaveCriticalSection(cs);
        ctx->gpr[3] = 0;
        break;
    }

    /* ---- Lightweight condition variables (lwcond) ------------------------- */

    /* sys_lwcond_create (620): r3=guest lwcond ptr, r4=lwmutex ptr, r5=attr* */
    case 620: {
        uint32_t lc = (uint32_t)ctx->gpr[3];
        EnterCriticalSection(&g_lw_map_cs);
        if (!g_lwcond_map->count(lc)) {
            auto* cv = (CONDITION_VARIABLE*)malloc(sizeof(CONDITION_VARIABLE));
            InitializeConditionVariable(cv);
            (*g_lwcond_map)[lc] = cv;
        }
        LeaveCriticalSection(&g_lw_map_cs);
        fprintf(stderr, "[LV2] sys_lwcond_create lc=0x%08X\n", lc);
        ctx->gpr[3] = 0;
        break;
    }
    case 621: { /* sys_lwcond_destroy */
        uint32_t lc = (uint32_t)ctx->gpr[3];
        EnterCriticalSection(&g_lw_map_cs);
        auto it = g_lwcond_map->find(lc);
        if (it != g_lwcond_map->end()) { free(it->second); g_lwcond_map->erase(it); }
        LeaveCriticalSection(&g_lw_map_cs);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_lwcond_wait (622): r3=lwcond ptr, r4=lwmutex ptr, r5=timeout_us */
    case 622: {
        uint32_t lc = (uint32_t)ctx->gpr[3], lm = (uint32_t)ctx->gpr[4];
        uint64_t tus = ctx->gpr[5];
        CONDITION_VARIABLE* cv = nullptr; CRITICAL_SECTION* cs = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwcond_map->find(lc);  if (it != g_lwcond_map->end())  cv = it->second; }
        { auto it = g_lwmutex_map->find(lm); if (it != g_lwmutex_map->end()) cs = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cv && cs) {
            DWORD wms = tus ? (DWORD)((tus + 999) / 1000) : INFINITE;
            fprintf(stderr, "[LV2] sys_lwcond_wait lc=0x%08X lm=0x%08X\n", lc, lm);
            BOOL ok = SleepConditionVariableCS(cv, cs, wms);
            ctx->gpr[3] = ok ? 0 : 0x80410034ull;
        } else {
            ctx->gpr[3] = 0;
        }
        break;
    }
    case 623: { /* sys_lwcond_signal */
        uint32_t lc = (uint32_t)ctx->gpr[3];
        CONDITION_VARIABLE* cv = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwcond_map->find(lc); if (it != g_lwcond_map->end()) cv = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cv) WakeConditionVariable(cv);
        ctx->gpr[3] = 0; break;
    }
    case 624: { /* sys_lwcond_signal_all */
        uint32_t lc = (uint32_t)ctx->gpr[3];
        CONDITION_VARIABLE* cv = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwcond_map->find(lc); if (it != g_lwcond_map->end()) cv = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cv) WakeAllConditionVariable(cv);
        ctx->gpr[3] = 0; break;
    }
    case 625: { /* sys_lwcond_signal_to: wake all (Windows can't target specific waiter) */
        uint32_t lc = (uint32_t)ctx->gpr[3];
        CONDITION_VARIABLE* cv = nullptr;
        EnterCriticalSection(&g_lw_map_cs);
        { auto it = g_lwcond_map->find(lc); if (it != g_lwcond_map->end()) cv = it->second; }
        LeaveCriticalSection(&g_lw_map_cs);
        if (cv) WakeAllConditionVariable(cv);
        ctx->gpr[3] = 0; break;
    }

    /* ---- Event queues ----------------------------------------------------- */

    /* sys_event_queue_create (125): r3=out*, r4=attr*, r5=key, r6=size */
    case 125: {
        uint32_t out = (uint32_t)ctx->gpr[3];
        uint32_t sz  = (uint32_t)ctx->gpr[6];
        uint32_t id  = pool_alloc(LV2_EVQ);
        if (!id) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        Lv2Evq* q = (Lv2Evq*)calloc(1, sizeof(Lv2Evq));
        InitializeCriticalSection(&q->cs);
        q->avail = CreateSemaphoreA(NULL, 0, EVQ_CAP * 4, NULL);
        g_pool[id].evq = q;
        if (out) vm_write32(out, id);
        fprintf(stderr, "[LV2] sys_event_queue_create id=%u size=%u\n", id, sz);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_event_queue_destroy (126): r3=id, r4=mode */
    case 126: {
        uint32_t id = (uint32_t)ctx->gpr[3];
        Lv2Obj* o = pool_get(id, LV2_EVQ);
        if (o && o->evq) {
            CloseHandle(o->evq->avail);
            DeleteCriticalSection(&o->evq->cs);
            free(o->evq); o->evq = nullptr;
        }
        pool_free(id);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_event_queue_receive (127): r3=id, r4=out_event*(32B), r5=timeout_us */
    case 127: {
        uint32_t id  = (uint32_t)ctx->gpr[3];
        uint32_t evp = (uint32_t)ctx->gpr[4];
        uint64_t tus = ctx->gpr[5];
        Lv2Obj* o = pool_get(id, LV2_EVQ);
        if (!o || !o->evq) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        DWORD wms = tus ? (DWORD)((tus + 999) / 1000) : INFINITE;
        fprintf(stderr, "[LV2] sys_event_queue_receive id=%u timeout=%llu us\n",
                id, (unsigned long long)tus);
        DWORD r = WaitForSingleObject(o->evq->avail, wms);
        if (r == WAIT_TIMEOUT) { ctx->gpr[3] = 0x80410034ull; break; }
        Lv2Evq* q = o->evq;
        EnterCriticalSection(&q->cs);
        Lv2Event ev = q->buf[q->head];
        q->head = (q->head + 1) % EVQ_CAP;
        q->cnt--;
        LeaveCriticalSection(&q->cs);
        if (evp) {
            vm_write64(evp,      ev.source);
            vm_write64(evp +  8, ev.data1);
            vm_write64(evp + 16, ev.data2);
            vm_write64(evp + 24, ev.data3);
        }
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_event_queue_tryreceive (128): r3=id, r4=event_list*, r5=sz, r6=out_count* */
    case 128: {
        uint32_t id  = (uint32_t)ctx->gpr[3];
        uint32_t evp = (uint32_t)ctx->gpr[4];
        uint32_t cp  = (uint32_t)ctx->gpr[6];
        Lv2Obj* o = pool_get(id, LV2_EVQ);
        if (!o || !o->evq) { if (cp) vm_write32(cp, 0); ctx->gpr[3] = 0; break; }
        if (WaitForSingleObject(o->evq->avail, 0) != WAIT_OBJECT_0) {
            if (cp) vm_write32(cp, 0); ctx->gpr[3] = 0; break;
        }
        Lv2Evq* q = o->evq;
        EnterCriticalSection(&q->cs);
        Lv2Event ev = q->buf[q->head];
        q->head = (q->head + 1) % EVQ_CAP;
        q->cnt--;
        LeaveCriticalSection(&q->cs);
        if (evp) { vm_write64(evp, ev.source); vm_write64(evp+8, ev.data1); vm_write64(evp+16, ev.data2); vm_write64(evp+24, ev.data3); }
        if (cp) vm_write32(cp, 1);
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_event_port_create (130): r3=out*, r4=type, r5=name */
    case 130: {
        uint32_t out = (uint32_t)ctx->gpr[3];
        uint64_t nm  = ctx->gpr[5];
        uint32_t id  = pool_alloc(LV2_EVPORT);
        if (!id) { ctx->gpr[3] = (uint64_t)-1LL; break; }
        g_pool[id].port.queue_id = 0;
        g_pool[id].port.name     = nm;
        if (out) vm_write32(out, id);
        fprintf(stderr, "[LV2] sys_event_port_create id=%u name=0x%llX\n",
                id, (unsigned long long)nm);
        ctx->gpr[3] = 0;
        break;
    }
    case 131: /* sys_event_port_destroy */
        pool_free((uint32_t)ctx->gpr[3]);
        ctx->gpr[3] = 0; break;
    /* sys_event_port_connect_local (132): r3=port_id, r4=queue_id */
    case 132: {
        uint32_t pid = (uint32_t)ctx->gpr[3], qid = (uint32_t)ctx->gpr[4];
        Lv2Obj* o = pool_get(pid, LV2_EVPORT);
        if (o) { o->port.queue_id = qid; fprintf(stderr, "[LV2] port %u → queue %u\n", pid, qid); }
        ctx->gpr[3] = 0;
        break;
    }
    /* sys_event_port_send (134): r3=port_id, r4=data1, r5=data2, r6=data3 */
    case 134: {
        uint32_t pid = (uint32_t)ctx->gpr[3];
        Lv2Obj* po = pool_get(pid, LV2_EVPORT);
        if (!po) { ctx->gpr[3] = 0; break; }
        Lv2Obj* qo = pool_get(po->port.queue_id, LV2_EVQ);
        if (!qo || !qo->evq) { ctx->gpr[3] = 0; break; }
        Lv2Evq* q = qo->evq;
        Lv2Event ev = { po->port.name, ctx->gpr[4], ctx->gpr[5], ctx->gpr[6] };
        EnterCriticalSection(&q->cs);
        if (q->cnt < EVQ_CAP) {
            q->buf[q->tail] = ev;
            q->tail = (q->tail + 1) % EVQ_CAP;
            q->cnt++;
            ReleaseSemaphore(q->avail, 1, NULL);
        } else {
            fprintf(stderr, "[LV2] sys_event_port_send: queue %u full\n", po->port.queue_id);
        }
        LeaveCriticalSection(&q->cs);
        ctx->gpr[3] = 0;
        break;
    }

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
