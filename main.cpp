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

    /* Game context stub at 0x700000 (returned by func_0003AAC4):
       - bits 0-1 of struct+0 must be non-zero: func_000CE5C4 gate check reads
         them via ppc_rlwinm(struct+0, 0, 30, 31); if zero it returns -1 and
         the game never initializes.
       - bit 7 (0x80) MUST be set: func_000D0664 (called before CE5C4) routes
         through func_000D0694 which inspects bit 7.
           bit7=0 → D0694 calls func_000D0700, which writes r0 (=0, the bit-7
                    mask) back into struct+0, ZEROING bits 0-1 before CE5C4
                    runs — gate always fails.
           bit7=1 → D0694 takes the slab-free path (func_000D90FC→D9108),
                    which our slab guard in D9108 silently skips when the slab
                    pool is uninitialized (slab+0x10==0). struct+0 is never
                    touched, bits 0-1 survive, and CE5C4 passes.
       - struct+0x44 must point to a zero-initialized scratch block (0x700100):
         func_000CE628 reads it as a linked-list head and passes it to D58C4. */
    vm_write16(0x700000u, 0x0083u);  /* bits 0-1 set (gate) | bit 7 set (slab-free path) */
    /* struct+0x44 is a pointer to a ref-counted resource object.
       Point it to a clean zero-initialized scratch block at 0x700100.
       func_000D58C4/D5880/D596C treat [ptr+0] as refcount, [ptr+0x10] as
       mutex data — all stubs, so zeros in that area are safe. */
    vm_write32(0x700044u, 0x00700100u);

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
