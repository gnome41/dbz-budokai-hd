#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "recompiled/ppu_recomp.h"

extern "C" uint8_t* vm_base;

jmp_buf g_process_exit_jmpbuf;
int     g_process_exit_code = -1;

extern "C" void thread_runtime_init();
extern "C" void thread_runtime_join_all();

static bool vm_init() {
#ifdef _WIN32
    vm_base = (uint8_t*)VirtualAlloc(NULL, 0x100000000ULL, MEM_RESERVE, PAGE_NOACCESS);
    if (!vm_base) { fprintf(stderr, "ERROR: VirtualAlloc reserve failed\n"); return false; }

    /* Low PS3 memory: 0x00000000-0x0000FFFF (PPU local storage area).
       On real PS3 this region is valid; many games write global structs here.
       func_000CE77C initializes a game-context struct at guest address 0.
       Without this commit the runtime null-guard would silently drop those
       writes and the initialization chain would see stale zeros. */
    if (!VirtualAlloc(vm_base + 0x00000000u, 0x10000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ERROR: Failed to commit low memory region\n");
        VirtualFree(vm_base, 0, MEM_RELEASE); return false;
    }
    /* Main RAM: 0x10000 .. 0x10010000 (256 MB, covers code + data + BSS) */
    if (!VirtualAlloc(vm_base + 0x00010000u, 0x10000000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ERROR: Failed to commit main RAM\n");
        VirtualFree(vm_base, 0, MEM_RELEASE); return false;
    }
    /* Stack: 0xD0000000 .. 0xE0000000 */
    if (!VirtualAlloc(vm_base + 0xD0000000u, 0x10000000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ERROR: Failed to commit stack region\n");
        VirtualFree(vm_base, 0, MEM_RELEASE); return false;
    }
    /* Caller-frame headroom above initial stack pointer.
       func_000379BC (SPURS state machine) saves LR at gpr[1]+0xF0 BEFORE
       decrementing gpr[1].  With initial SP=0xDFFFFE00, the first call to
       func_000379BC from loc_0003AE74 (gpr[1]=0xDFFFFF10 after nested frames)
       writes to 0xDFFFFF10+0xF0=0xE0000000 — one byte past the committed
       stack region — causing an ACCESS VIOLATION.
       One 4 KB page is enough now that the func_000379BC SP-leak bug
       is patched; the headroom only needs to cover the PPC caller-frame
       linkage area written above SP before the prologue decrements it. */
    if (!VirtualAlloc(vm_base + 0xE0000000u, 0x1000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ERROR: Failed to commit stack headroom\n");
        VirtualFree(vm_base, 0, MEM_RELEASE); return false;
    }
    /* PS3 LV2/TLS high area: 0xFFFF0000 .. 0xFFFFFFFF (64 KB).
       LV2 maps thread-local storage and kernel-shared structs here.
       func_0003AAC8 cleanup path writes through a pointer that resolves
       to guest 0xFFFF9004 — without this commit we get an AV. */
    if (!VirtualAlloc(vm_base + 0xFFFF0000u, 0x10000u, MEM_COMMIT, PAGE_READWRITE)) {
        fprintf(stderr, "ERROR: Failed to commit LV2/TLS high region\n");
        VirtualFree(vm_base, 0, MEM_RELEASE); return false;
    }
#else
    #error "Non-Windows not yet supported"
#endif
    return true;
}

static void vm_shutdown() {
#ifdef _WIN32
    if (vm_base) { VirtualFree(vm_base, 0, MEM_RELEASE); vm_base = nullptr; }
#endif
}

static uint16_t elf_u16(const uint8_t* p, size_t o) {
    return (uint16_t)(((uint16_t)p[o] << 8) | p[o+1]);
}
static uint32_t elf_u32(const uint8_t* p, size_t o) {
    return ((uint32_t)p[o]<<24)|((uint32_t)p[o+1]<<16)|((uint32_t)p[o+2]<<8)|(uint32_t)p[o+3];
}
static uint64_t elf_u64(const uint8_t* p, size_t o) {
    return ((uint64_t)elf_u32(p,o) << 32) | elf_u32(p, o+4);
}

/* Load PT_LOAD segments from a big-endian ELF64 into guest memory. */
static bool elf_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ERROR: Cannot open ELF: %s\n", path); return false; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    uint8_t* elf = (uint8_t*)malloc((size_t)fsize);
    if (!elf || (long)fread(elf, 1, (size_t)fsize, f) != fsize) {
        fprintf(stderr, "ERROR: Failed to read ELF\n");
        fclose(f); free(elf); return false;
    }
    fclose(f);

    if (memcmp(elf, "\x7f" "ELF", 4) != 0) {
        fprintf(stderr, "ERROR: Bad ELF magic\n");
        free(elf); return false;
    }

    uint64_t e_phoff     = elf_u64(elf, 0x20);
    uint16_t e_phentsize = elf_u16(elf, 0x36);
    uint16_t e_phnum     = elf_u16(elf, 0x38);

    int loaded = 0;
    for (int i = 0; i < e_phnum; i++) {
        size_t   ph      = (size_t)(e_phoff + (uint64_t)i * e_phentsize);
        uint32_t p_type  = elf_u32(elf, ph);
        uint64_t p_off   = elf_u64(elf, ph +  8);
        uint64_t p_vaddr = elf_u64(elf, ph + 16);
        uint64_t p_filesz= elf_u64(elf, ph + 32);
        uint64_t p_memsz = elf_u64(elf, ph + 40);

        if (p_type != 1 /* PT_LOAD */ || p_vaddr == 0 || p_memsz == 0)
            continue;

        printf("  Seg %d: vaddr=0x%08llX filesz=0x%llX memsz=0x%llX\n",
               i, (unsigned long long)p_vaddr,
               (unsigned long long)p_filesz, (unsigned long long)p_memsz);

        uint8_t* dest = vm_base + (uint32_t)p_vaddr;
        if (p_filesz > 0)
            memcpy(dest, elf + (size_t)p_off, (size_t)p_filesz);
        if (p_memsz > p_filesz)
            memset(dest + (size_t)p_filesz, 0, (size_t)(p_memsz - p_filesz));
        ++loaded;
    }

    free(elf);
    printf("Loaded %d ELF segment(s)\n", loaded);
    return loaded > 0;
}

int main(int argc, char* argv[]) {
    const char* elf_path = (argc > 1) ? argv[1] : "EBOOT.ELF";
    printf("=== ps3recomp: dbz-budokai-hd ===\n");

    thread_runtime_init();
    if (!vm_init()) return 1;
    printf("Guest memory initialized\n");

    printf("Loading ELF: %s\n", elf_path);
    if (!elf_load(elf_path)) return 1;

    /* Set up a minimal argv/envp area in committed guest memory.
       _start (func_0003B328) runs do-while copy loops over argv (gpr[4]) and
       envp (gpr[5]), each of which reads base+4 at least once even when count=0.
       Point them at safe scratch pages so those reads don't fault. */
    const uint32_t SCRATCH = 0x00500000u;  /* well past ELF BSS end (0x32B410) */
    memset(vm_base + SCRATCH, 0, 0x1000);  /* zero 4 KB scratch area */

    /* Pool manager linked-list sentinel nodes: the +0x44 chain field in each
       struct must be self-referential (chain = &chain) for an empty list.
       The ELF stores 0 and the C++ constructor table is in BSS (empty), so
       no ctors run to initialize these. Fix them before entering the game. */
    vm_write32(0x27BBD4u, 0x0027BBD4u);  /* struct @ 0x27BB90, chain @ +0x44 */
    vm_write32(0x27BC3Cu, 0x0027BC3Cu);  /* struct @ 0x27BBF8, chain @ +0x44 */
    vm_write32(0x27BCA4u, 0x0027BCA4u);  /* struct @ 0x27BC60, chain @ +0x44 */

    /* Fake SPURS manager context at 0x700000.
       func_0003AAC4 returns this address (overriding func_0003AAC8's 0).

       ctx+0x0 field layout (16-bit halfword):
         bits 0-1  (gate)        must be non-zero → func_000CE5C4 passes the gate
         bit  7    (slab path)   0x80 → slab-free path in CE77C
         bit  13   (real-init)   0x2000 → CE77C takes the real SPURS init path
                                 instead of the CE8B8 "no work" no-op path

       func_000D5450 (D544C): reads ctx+0x4 halfword; must be >= 2 to trigger
       the syscall 0x324 init path instead of immediate-return-0.

       func_000CE77C (bit13=1 path): checks "ctx+8 < ctx+0x10"; ctx+8 = 0 (BSS),
       so ctx+0x10 must be non-zero for the D54B0 syscall-0x323 loop to fire. */
    vm_write16(0x700000u, 0x2083u);   /* bits 0-1 (gate) | bit 7 (slab) | bit 13 (real-init) */
    vm_write16(0x700004u, 2u);        /* D544C: struct+4 >= 2 → syscall 0x324 path */
    vm_write32(0x700010u, 0x700043u); /* CE77C: struct+0x10 sentinel > struct+8(0) */

    /* func_000CE628 reads [ctx+0x44] → passes to func_000D1E64 as a sub-object
       pointer.  func_000D1E64 then calls func_000D58C4 with it.  Point it at a
       zeroed page area: func_000D58C4([ptr+0x0]=0) immediately takes the safe
       loc_000D5914 early-return path. */
    vm_write32(0x700044u, 0x700100u);

    /* func_000379BC is the SPURS workload state machine (driven by [0x28B050]).
       State 0 and state 1 (func_00037534) both spin on guest address 0x0
       waiting for a SPURS SPU structure that never arrives.  Start at state 2
       so the dispatcher jumps straight to loc_00037A84 → func_00035198 →
       state 3.  Each subsequent SPURS loop pass advances the state machine;
       when it reaches state 0xF (15) the patched loc_00038194 code writes
       state=21 and [0x27F830]=1 to exit the dispatch loop cleanly. */
    vm_write32(0x28B050u, 2u);   /* start SPURS state machine at state 2 (skips SPU-wait states 0-1) */

    /* SPURS workload dispatch chain.
       loc_0003AE74 in func_0003AAC8 reads [gpr[30]+0x28] = [0x27F81C] as a
       SPURS workload struct pointer, then dereferences a vtable chain to call
       the workload function (func_000379BC).  With [0x27F81C]=0 all reads fall
       to guest addresses 0x0/0x8 → infinite LOW-READ32 spin.

       NOTE: the bump-slab fallback allocator (func at ~line 523438 in ppu_recomp.cpp)
       starts its pool at 0x701000.  Any synthetic data placed there will be
       overwritten the first time an uninitialised slab fires, corrupting the
       chain.  Use 0x70A000 instead (well above any observed bump allocation).

       Build a minimal synthetic dispatch chain at 0x70A000:
         A = [0x27F81C]      → 0x70A000  (workload struct)
         B = [A + 0x00]      → 0x70A010  (vtable-like ptr)
         C = [B + 0x08]      → 0x70A020  (OPD entry)
         code = [C + 0x00]   → 0x000379BC
         TOC  = [C + 0x04]   → 0x0016A0F8  */
    vm_write32(0x27F81Cu, 0x70A000u);   /* A  = workload struct ptr */
    vm_write32(0x70A000u, 0x70A010u);   /* B  = [A+0x00] */
    vm_write32(0x70A018u, 0x70A020u);   /* C  = [B+0x08] (OPD slot) */
    vm_write32(0x70A020u, 0x000379BCu); /* code pointer */
    vm_write32(0x70A024u, 0x0016A0F8u); /* TOC */

    /* func_000355D4 / func_000355FC gate (states 13 and 14).
       These functions read [gpr[30]+0x20]=[0x27F814] → ptr P,
       then [P+0x148] → ptr Q, then [Q+0xC].  func_000355D4 returns 1 if
       [Q+0xC]==1, func_000355FC returns 1 if [Q+0xC]>=2.  With BSS=0 both
       return 0 → states 13 and 14 stall forever.
       Build a synthetic struct chain at 0x70B000 with [Q+0xC]=2. */
    vm_write32(0x27F814u, 0x70B000u);   /* P  = [0x27F814] */
    vm_write32(0x70B148u, 0x70B200u);   /* Q  = [P+0x148] */
    vm_write32(0x70B20Cu, 1u);          /* [Q+0xC] = 1 → func_000355D4 returns 1 (checks ==1) */

    /* State 7 (loc_00037D3C): reads [0x289B90+0x5D0]=[0x28A160].
       Code: "if ([0x28A160] != 0) return" — so 0 means PROCEED, 1 means SPIN.
       BSS default is 0, which is correct.  The earlier pre-patch of 1 was wrong
       (caused state 7 to spin forever).  Do NOT set [0x28A160] here; state 7
       will write it to 1 itself after creating Thread 2. */

    /* State 12 (loc_00037F38): calls func_00027090 which reads [0x27F831].
       If non-zero, advances to state 13.  On a real PS3 an SPU task sets this.
       Pre-set to 1 so state 12 advances on its first pass. */
    vm_write8(0x27F831u, 1u);

    /* loc_0003B088 in func_0003AAC8 spins on vm_read8(0x290000 - 0x4F70) =
       vm_read8(0x28B090) waiting for SPURS shutdown confirmation (normally set
       by SPU tasks, which we don't run).  Pre-set to 1 so the spin exits on
       the first check. */
    vm_write8(0x28B090u, 1u);

    /* func_0003AAC4 is now a real trampoline that calls func_0003AAC8 (the actual
       game initializer / thread launcher).  The synthetic game-context stub at
       0x700000 is no longer needed as func_0003AAC4's return value.  We keep
       the scratch block at 0x700100 zeroed in case any surviving code path
       still reads through it, but the main initialization is now real. */

    ppu_context ctx = {};
    ctx.gpr[1]  = 0xD0000000u + 0x10000000u - 0x200u;  /* stack top */
    ctx.gpr[2]  = 0x0016A0F8u;   /* TOC base (from OPD at 0x161400+4) */
    ctx.gpr[3]  = 0;             /* argc = 0 */
    ctx.gpr[4]  = SCRATCH;       /* argv – do-while reads [argv+4] once, harmless */
    ctx.gpr[5]  = SCRATCH + 0x100; /* envp – same pattern */
    ctx.gpr[6]  = 0;             /* envp count = 0 */

    printf("Entering recompiled entry point...\n");
    /* OPD at e_entry 0x161400: code=0x3B220, 8-instr prolog loads TOC then bl 0x3B328.
       TOC pre-loaded above; call the lifted body directly. */
#ifdef _WIN32
    /* setjmp target for sys_process_exit — longjmp(g_process_exit_jmpbuf,1) unwinds here. */
    if (setjmp(g_process_exit_jmpbuf) != 0) {
        printf("Process exited normally (code=%d). Initialization complete.\n",
               g_process_exit_code);
        vm_shutdown();
        return 0;
    }

    EXCEPTION_POINTERS* g_ep = nullptr;
    __try {
        func_0003B328(&ctx);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? (g_ep = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER)
                    : EXCEPTION_CONTINUE_SEARCH) {
        ULONG_PTR fault_host  = g_ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t  fault_guest = (uint32_t)(fault_host - (ULONG_PTR)vm_base);
        int       is_write    = (int)g_ep->ExceptionRecord->ExceptionInformation[0];
        fprintf(stderr, "\nACCESS VIOLATION (%s) at host 0x%p -> guest 0x%08X\n",
                is_write ? "write" : "read", (void*)fault_host, fault_guest);
        fprintf(stderr, "gpr[1]=0x%08llX gpr[2]=0x%08llX\n",
                (unsigned long long)ctx.gpr[1], (unsigned long long)ctx.gpr[2]);
        return 1;
    }
#else
    func_0003B328(&ctx);
#endif

    printf("Entry point returned. Waiting for game threads...\n");
    thread_runtime_join_all();
    vm_shutdown();
    return 0;
}
