# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

A static recompilation of *Dragon Ball Z: Budokai HD* (`EBOOT.elf`) to a native Windows x64 executable, built on top of the **ps3recomp SDK** located two levels up at `../../` (i.e., `E:\Games\RecompLauncher\ps3recomp\`). See that directory's CLAUDE.md for SDK-wide architecture.

The PS3 PPU instructions were lifted offline by `ppu_lifter.py` into `recompiled/ppu_recomp.cpp`; this project wires up the lifted code with a hand-written runtime, stubs LV2 syscalls, and patches game-specific data structures.

## Build & Run

```bat
build_run.bat      # configure + build via Ninja (echoes BUILD_EXIT=)
force_rebuild.bat  # faster: force-recompile changed .cpp files then link (skips cmake configure)
run.bat            # build\dbz-budokai-hd.exe ..\game\EBOOT.elf
```

The ELF must be a decrypted PS3 ELF64 at `E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf` (outside this repo). No automated tests — verify by reading stderr diagnostics.

### Build incantation that actually works (PowerShell)

`build_run.bat` configures cmake without `-DPS3RECOMP_DIR=...`, which defaults to `../../` from the project dir → `E:\Games\RecompLauncher` (wrong; the SDK is at `E:\Games\RecompLauncher\ps3recomp`). When that's wrong cmake's `add_subdirectory` falls through and configure fails. The reliable invocation is:

```powershell
cd E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd
# Initial configure (only needed once or after deleting build\):
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -B build -G Ninja -S . -DPS3RECOMP_DIR=E:/Games/RecompLauncher/ps3recomp'
# Subsequent rebuilds (auto-tracks dependencies):
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build'
```

vcvars64.bat MUST be sourced inside the same `cmd` session as cmake — env vars set in PowerShell with `&` don't propagate to the cmake child. Don't try to use Git Bash to invoke MSVC: it mangles `/flag` args as paths.

If the cmake configure cache is broken (missing `build.ninja`), delete `build\CMakeCache.txt` and `build\CMakeFiles\` and reconfigure. `force_rebuild.bat` exists as a fallback that direct-compiles changed files but its final `cmake --build` link step still requires a valid build.ninja.

## Source Files

| File | Purpose |
|---|---|
| `main.cpp` | Reserves 4 GB guest VA, loads ELF segments, patches pool-manager sentinels + game-context stub, calls `func_0003B328` (`_start`) then `func_0003B244` (two-step entry); defines `DRAIN_TRAMPOLINE` macro |
| `runtime_glue.cpp` | `vm_base` + `vm_read*/vm_write*` (big-endian, 32-bit truncated addresses); `lv2_syscall` dispatcher; `ps3_indirect_call` for CTR indirect branches; RSX FIFO parser (`rsx_process_fifo`) |
| `extra_funcs.cpp` | Hand-lifted PPU functions missed by the lifter (ctors/dtors reached only via indirect calls); same `void func_XXXXXXXX(ppu_context*)` signature |
| `stubs.cpp` | Reference template for NID overrides/module hooks — not compiled; kept as documentation |
| `spu_interp.cpp` | SPU instruction interpreter — all opcodes for SPURS kernel + EDGE geometry processor, including lqx (0x1FD), orx (0x3F8), shlqbi (0x1DD), shlqbybi (0x1DF) |
| `spu_spurs.cpp` | SPURS/SPU orchestration: loads kernel ELF, patches LS, runs SPU burst loop, dispatches EDGE workloads, `spurs_test_edge_geometry()` diagnostic |
| `recompiled/ppu_recomp.cpp` | **Auto-generated base** — but does receive targeted manual patches (e.g. `bcctrl` trampoline fixups, guard insertions). Document any manual edits clearly with comments. |
| `recompiled/ppu_recomp.h` | **Auto-generated** — declares `ppu_context`, memory helpers, all `func_XXXXXXXX` prototypes |
| `config.toml` | Lifter config: input ELF path, output dir, HLE/LLE module choices, memory/threading/graphics/audio settings |

## Key Runtime Details

### Guest Memory Layout
- `vm_base`: 4 GB `MEM_RESERVE` via `VirtualAlloc`. Guest 32-bit addresses index as `vm_base[uint32_t(addr)]`.
- Committed regions (see `vm_init` in `main.cpp`):
  - `0x00000000–0x0000FFFF` — low PS3 memory (PPU-local storage / game global structs)
  - `0x00010000–0x10010000` — 256 MB main RAM (code + data + BSS)
  - `0xD0000000–0xE0000000` — 256 MB stack
  - `0xE0000000–0xE0000FFF` — **caller-frame headroom** (1 page above SP top). PPC ABI saves LR into the *caller's* frame at `SP+0xF0` *before* the prologue decrements SP; with initial `SP=0xDFFFFE00` the very first save lands at `0xE0000000` which would AV without this commit.
  - `0xFFFF0000–0xFFFFFFFF` — PS3 LV2/TLS high area (thread-local storage, kernel-shared structs)
- Always use `vm_read*/vm_write*` helpers — never raw pointer casts (big-endian byte-swap required).
- `vm_write32` intercepts zero-writes to `0x27F81C` (SPURS workload list head → `0x70A000`) and `0x27F814` (states-13/14 struct head → `0x70B000`) to preserve synthetic dispatch chains — see below.

### LV2 Syscalls (`runtime_glue.cpp: lv2_syscall`)
Dispatches on `ctx->gpr[11]`. Currently handled:

| Syscall | Name | Notes |
|---|---|---|
| 41 | `sys_ppu_thread_exit` | calls `ExitThread` |
| 43 | `sys_ppu_thread_join` | `WaitForSingleObject` on the Windows thread handle |
| 44 | `sys_ppu_thread_create` | allocates guest stack, creates a real Windows thread running `ppu_thread_proc` |
| 45 | `sys_ppu_thread_yield` | `SwitchToThread()` |
| 48 | `sys_ppu_thread_get_id` | returns placeholder |
| 84–88 | semaphores | `CreateSemaphoreA`/`WaitForSingleObject`/`ReleaseSemaphore`; IDs in `g_pool[]` |
| 90–94 | mutexes | `InitializeCriticalSection`/`EnterCriticalSection`/`LeaveCriticalSection`; IDs in `g_pool[]` |
| 95–99 | condition variables | `InitializeConditionVariable`/`SleepConditionVariableCS`/`WakeConditionVariable` |
| 125–134 | event queues/ports | circular buffer + counting semaphore + CRITICAL_SECTION; IDs in `g_pool[]` |
| 141 | `sys_time_get_system_time` | `GetTickCount64() * 1000` |
| 145 | `sys_time_get_timebase_frequency` | returns 79 800 000 |
| 403 | `sys_tty_write` | prints game debug output |
| 612–616 | lwmutex | `CRITICAL_SECTION` per guest ptr in `g_lwmutex_map` |
| 620–625 | lwcond | `CONDITION_VARIABLE` per guest ptr in `g_lwcond_map` |
| 988 | watchdog | `r3==1` is a heartbeat (logged, ignored); `r3==4` sets `g_abort_called`, dumps call stack |

All others log and return 0. Add new cases directly to `lv2_syscall`.

### Thread Runtime (`runtime_glue.cpp`)
`thread_runtime_init()` initialises the critical section — call before `vm_init()` in `main.cpp`.  
`thread_runtime_join_all()` waits for all game threads — call after both entry calls (`func_0003B328` + `func_0003B244`) return.

Each game thread gets a 1 MB guest stack committed on demand (bump-allocated downward from `0xCFFF0000`). The thread entry OPD is decoded (code ptr + TOC at `opd_ptr` / `opd_ptr+4`) and the resolved host function is called with a fresh `ppu_context`. `g_trampoline_fn` is `__declspec(thread)` so each Windows thread has its own trampoline slot.

### Indirect Calls
`ps3_indirect_call` resolves `ctx->ctr` via `ppu_resolve_addr` (generated table) then `ppu_resolve_extra` (hand-lifted). Unknown addresses log once and stub with `gpr[3] = 0`.

### Process Exit
`func_000F217C` calls `longjmp(g_process_exit_jmpbuf, 1)`; the `setjmp` in `main.cpp` catches it and exits cleanly.

### Trampoline Pattern
Cross-fragment tail calls set `g_trampoline_fn` (TLS). Always drain with `DRAIN_TRAMPOLINE(ctx)` after any call in `extra_funcs.cpp`.

## Current Status (last verified working state)

**The program runs cleanly through its full startup → shutdown lifecycle with no AV, no abort, no spin.** Verified end-state (8 threads run, including the UpdateThread audio stub):

1. SPURS init runs (`func_0003B328`/`func_0003AAC4`/`func_0003AAC8`).
2. SPURS workload state machine advances `2 → 3 → 4 → 6 → 7 → 8 → 9 → 12 → 13 → 14 → 15 → 21`.
3. Four original SPURS-spawned threads start; each spawns a sub-worker via `func_000F10FC`/`func_000F16DC`:
   - Thread 1: `sdu_yah_size_check` @ `0x000EFD18` (arg `0x289590`) → spawns worker @ `0x000EFE30` → joins it → exits
   - Thread 2: `sdu_yah_size_check` @ `0x000EFD18` (arg `0x289B90`) → spawns worker @ `0x000EFE30` → joins it → exits
   - Thread 3: `sdu_yah_all_list_delete` @ `0x000EFACC` → spawns worker @ `0x000EFBE8` → exits
   - Thread 4: `Terminate Thread` @ `0x00039E20` → exits immediately (no pending requests)
4. Three sub-worker threads run and return (stubs).
5. C runtime wrapper (`func_0003B244` → `func_000F205C`) calls game main `func_00012420`.
   - `func_000510E4` (now implemented) creates Thread 8: UpdateThread (bnusCore audio stub at 0xD3020).
6. Game main passes all GCM init checks (force-succeeded), runs through RSX sync (RSX REF null backend), and executes its full initialization sequence.
7. C++ destructor walker fires; `[RUNTIME] all game threads finished` — program exits cleanly.

**Game main `func_00012420` architecture:**
- Called from `func_0003B2BC → func_0003B244 → func_000F205C` (within func_0003B328's lifecycle)
- Loads sysmodules (func_000F0E9C — NID stubs returning 0)
- Calls `cellGcmInit` (`func_0004370C`, force-succeeded), `func_00040C0C`, `func_00040BD4` (all force-succeeded with null RSX context)
- Allocates display buffers (slab bump guard + `func_000D908C`)
- Calls `func_0004BE54` (display buffer setup) and `func_00012258` (init helper, zero-stub)
- Returns after completing init — there is NO game loop in `func_00012420`

**`func_00012258` (0x12258, zero-stub):** reads/initializes game state at 0x243AF0-0x27BDB4, ends with `memset(0x243C18, 0, 0x100)`. Pure initialization helper, not the game loop.

**Why the game doesn't actually run gameplay:** All 7 PS3 threads are SDU management threads. The `UpdateThread` is a bnusCore audio management thread (NOT the main game loop); the actual game loop is SPURS/SPU-driven and requires SPU emulation. `func_000510E4` (zero-stub at line 453, called from game main) is supposed to register a callback that eventually creates the UpdateThread via `func_000B8790`. Since it's a stub, the audio thread is never started.

**Win32 display window:** A `window_thread_proc` thread is launched from `main()` 200ms before the game entry point. It creates a 1280×720 DIB-backed window ("DBZ Budokai HD – PS3 Static Recompilation") with a dark-blue initial fill. `rsx_present_frame()` (exported from `main.cpp`) invalidates the window on each RSX frame. The framebuffer pointer (`g_framebuf`, BGRA) is accessible from `runtime_glue.cpp` for future RSX blit.

**UpdateThread (bnusCore audio) — now running:**
- Thread name: `"UpdateThread"` (string at 0xF5C38); entry code at 0xD3020 (`func_000D3020` stub in `extra_funcs.cpp`)
- `func_000510E4` (ppu_recomp.cpp line 453): implemented — pre-patches OPD at `[TOC[-0x6158]]=0x27BBF8` to (code=0xD3020, toc=0x16A0F8), then calls sys_ppu_thread_create (syscall 44) directly, bypassing the event port callback chain
- `func_000D3020` stub: 16 ms idle loop on `g_threads_should_exit` flag (in runtime_glue.cpp); main() sets flag before `thread_runtime_join_all()`
- The event port chain (func_000B8790 → func_000B8794 → syscall 130 → func_000B89BC → func_000D2F90) is not used; func_000D2F90's OPD pre-patch is also done inside func_000510E4 now

**LV2 syscall gate fix:** `func_00000030` now forwards to `lv2_syscall`. Address `0x30 → lv2_gate` in `extra_table`. 604 CTR-setting + `func_00000030` patterns mass-replaced with `ps3_indirect_call`.

**Game main `func_00012420` structure (line 7950):**
- Loads sysmodules via `func_000F0E9C` (IDs 0x17 and 0xE) — returns 0 (stub), continues
- Calls `func_000510E4` (line 7997) with r3=stack struct containing 0xF2890, r4=0xF2890
- Falls through to GCM init, 164K×64-byte slab allocs, display buffer setup, then returns

**EDGE output format (discovered this session):**
EDGE geometry processor outputs **raw float4 vertex data** (not NV4097 commands) to the output buffer EA. The MFC PUT writes 16KB (0x4000 bytes) of float4 vertices from LS[0xBC80] to the output EA. The PPU side would normally issue NV4097 draw calls referencing this vertex buffer. Our test triangle passes through EDGE unchanged:
- Vertex 0: (0,1,0,1) → screen (640, 0) = top center
- Vertex 1: (-1,-1,0,1) → screen (0, 720) = bottom left
- Vertex 2: (1,-1,0,1) → screen (1280, 720) = bottom right

**PUT EA formula (now fixed):**
- `lqx r43, r49, r124` at LS[0x35FC]: lsaddr = (r49+r124)&~15 = 0x3430 (from sentinel bytes 0x34)
- Original bytes[4..7] at lsaddr: 0x3FBF0C83 → PUT EA = 0x830CBF3F (garbage)
- Patched bytes[4..7] to {0x00,0x00,0x10,0xD0} → PUT EA = 0xD0100000 ✓
- Patch applied in `spu_step()` at `pc == 0x35FCu && ctx->id == 2`

**src_ea must be outside ELF BSS (0x10000–0x10010000):**
`src_ea = 0x70C000` (SPURS synthetic area). Previously 0xA07000 was corrupting ELF BSS data, causing SDU threads to exit immediately (regression introduced when workload dispatch first fired).

**Software rasterizer** in `rsx_on_edge_write()` (`runtime_glue.cpp`): reads float4 big-endian vertices from 0xD0100000, projects to screen, flat-shades with orange-gold diffuse lighting, writes BGRA8 to framebuffer. Only fires for LS source 0xBC80 (primary vertex output, tag=0). Win32 window holds 3 seconds after game main returns so the rendered image is visible.

**Current test geometry**: UV sphere (NLAT=14, NLON=14) generated in spu_spurs.cpp stop 0x3FFF handler — 1023 vertices (341 triangles) written to LS[0x134..0x5133] (16 KB). EDGE processes all 16 KB passthrough → PUT to 0xD0100000. Rasterizer draws 341 flat-shaded triangles. Visual: shaded sphere centred at screen mid-point (~640×360 area), warm gold-orange tones.

**Architecture findings (confirmed this session):**

**No PPU game loop**: `func_00012420` (full code read) is pure init. After cellGcmInit + display buffer allocation it returns — no game loop. The actual rendering cycle is SPURS-driven on the SPU side.

**SPURS mailbox mechanism**: The SPURS kernel idles at LS[0x298E0] (stop-0 in BSS) waiting for LV2 to restart it. On real PS3, LV2 detects the stop, checks the workload queue, updates kernel registers, and restarts from entry 0xD0. The kernel does NOT DMA the management area at 0x70A000 in our run — our lnop bypass at LS[0x03C0] forces dispatch without real workload data (r79=0 → sort loop sees no workloads → dispatch via stop-signal chain is hollow). To dispatch real game workloads: remove lnop patch, implement PPU→SPU mailbox signaling (populate `g_spurs_ctx.gpr` with workload data and restart from 0xD0 with r79≠0).

**AFS game data**: LAUNCH/data.afs has 16 entries: entry 0 = `nusc` scene config (camera angles, placement); entries 1-15 = `#A3T` textures. No polygon mesh data in the launcher pack. Game geometry is in DBZ1/data_en.afs (~350MB) and DBZ3/data_cmn.afs (~325MB), only accessible when the game loop loads assets via cellFs.

**NID stubs all return 0**: func_00012420's full call tree has no cellSpursAddWorkload or cellEdgeGeomAddJob calls — the game's SPURS workload registration only happens via the SPURS state machine (func_000379BC, states 2→21). Real game EDGE tasks are submitted from the game loop (not the init path we run).

**Outstanding questions for the next iteration:**
- **SPURS dispatch registers (corrected)** — CLAUDE.md previously said brhnz r12/r13 at LS[0x03BC]/[0x03C0]. From ELF decode and trace analysis, the actual registers are r36 (at 0x03BC) and r33 (at 0x03C0). Both are patched to lnop. With r79=0: r36.high=0 (falls through naturally), r33.high≠0 (needs lnop at 0x03C0). With r79=1: r36.high=0x00C0≠0 (needs lnop at 0x03BC too) — r79=1 breaks the single-lnop approach. Currently patching both 0x03BC and 0x03C0 to lnop, keeping r79=0.
- **SPURS mailbox signaling** — to dispatch real game workloads, implement `cellSpursAddWorkload` HLE that populates the SPU management area and signals the kernel via inbound mailbox. The kernel entry expects r79/r77 set by LV2 to indicate pending workloads. With both brhnz patched, r79≠0 also enables r42 (dispatch target) to be computed via selb from real workload data — but the selb output format is not yet understood.
- **SPURS management area format** — the kernel does NOT DMA the management area at 0x70A000 during our burst run. r79/r77 must be pre-populated via the kernel entry arguments, not DMA. The management area IS used to set up priority/sort tables (LS[0x1F5B0..0x1F62F]) but only during kernel ELF initialization.
- Implement a real bnusCore audio loop in `func_000D3020` (currently a 16 ms sleep stub)

### RSX / EDGE rendering infrastructure (`runtime_glue.cpp`)
- `rsx_process_fifo`: PPU RSX FIFO parser, fires on PUT register writes (guest addr 0x10). Handles NV4097_SET_COLOR_CLEAR_VALUE (0x1820), NV4097_CLEAR_SURFACE (0x1D94), NV4097_DRAW_ARRAYS (0x1808, logged only).
- `rsx_on_edge_write(put_end_ea)`: called by SPU interpreter when EDGE PUTs to 0xD0100000-0xD0200000. Invokes `edge_rasterize_triangles` to software-render the vertex batch.
- `edge_rasterize_triangles(vertex_ea, count)`: reads float4 BE vertices from guest memory, projects to 1280×720 screen, fills with barycentric test, writes BGRA8 to framebuffer.
- GCM context at 0x70E000 (TOC[-0x7FA0] = 0x162158). End pointer at +0x10 = 0xD01FFFFC. IO size at +0x18, flags at +0x1C.
- Identify the 26 sysPrxForUser NIDs imported by this ELF
- New syscall `sys_0x324` (0x324 = 804) seen during init — currently unimplemented

## SPU Interpreter (`spu_interp.cpp`) — Current State

The interpreter runs the embedded SPURS kernel ELF (at guest `0x10BD00`). SPU entry = `0xD0`. A diagnostic burst (verbose DMA logging + 50-insn trace) is run at startup in `spu_spurs.cpp`.

### Opcode fixes applied (verified against trace output)

| op11 | Correct mnemonic | Was | Fix |
|------|-----------------|-----|-----|
| `0x1DB` | `rotqbi` (rotate quad left by bit count from RB) | missing | added |
| `0x1DC` | `rotqbybi` (rotate quad left by byte count = RB>>3) | UNIMPL | added — key fix for ceqi path |
| `0x1D4` | `rotqbii` (rotate quad left by bit count immediate, RI7) | UNIMPL | added — required for workload init loop |
| `0x1FC` | `rotqby` (rotate quad by byte count from RB) | wrongly labelled rotqbybi | corrected |
| `0x07F` | `shlqbyi` (shift left quad by byte immediate) | no-op | implemented |
| `0x1F8`, `0x17F` | `.word` (not valid opcodes) | wrongly implemented | removed |
| `0x1DD` | `shlqbi` (shift left quad by RB[2:0] bits) | UNIMPL | added — SPURS dispatch code 0x0560 |
| `0x1DF` | `shlqbybi` (shift left quad by (RB>>3)&15 bytes) | UNIMPL | added — SPURS dispatch code 0x0574 |
| `0x1FD` | `lqx` variant (load quad indexed: LS[(RA+RB)&~15]→RT) | UNIMPL | added — EDGE geometry; key fix for DMA (without it MFC_EAL=0) |
| `0x3F8` | `orx` variant (OR across 4 words of RA into RT.u32[0]) | UNIMPL | added — EDGE geometry result accumulation |

Note: comment in code claimed `rotqbii = 0x1FB` — this is WRONG. Actual kernel uses `0x1D4`.

### ceqi fix: Option B — patch LS[0x17C] with `ilh r2, 1`

**Option A (change SPURS_CTX_EA to 0x70A000) does NOT work.** The analysis was wrong: both 0x700000 and 0x70A000 have bits[20:17]=0x7 (i.e., `r11.u8[2]=0x07` after rothi), so both give `r4.u32[0]=0x07_000008 ≠ 8`.

**Applied Option B:** In `spu_spurs.cpp`, after loading the ELF, patch LS[0x17C] with `ilh r2, 1` (= `0x40000082`, op7=0x20, I16=1, RT=2). This sets r2=0x00010001 ≠ 0 → `brnz r2, dispatch` at LS[0x184] fires.

### SPURS kernel execution flow — verified working

Entry path: **0xD0** → 34 instructions → `stop 0` at LS[0x154] (first idle) → restart from LS[0x158]:

```
0x168: rothi   r11, r3, 12      ; r11 = rotated halfwords of SPURS_CTX_EA
0x170: a       r3, r3, r12      ; r3 = EA + r12
0x178: rotqbybi r4, r11, r3     ; r4 = r11 rotated by (r3>>3)&15 bytes
0x17C: [patched to ilh r2, 1]   ; force r2=1 → dispatch branch fires
0x184: brnz    r2, 0x0280       ; jumps to dispatch path ← branch now taken
```

Dispatch path (LS[0x0280]–LS[0x02DC]):
- Initialization loop (16 iterations): initializes workload descriptors using `rotqbii r8, r4, 5` to compute slot offsets (r8 = counter × 32)
- Loads workload status flags from LS into registers
- `ila r13, 0x1F5F0` at LS[0x0308]: loads the workload availability address into r13

Key dispatch check at LS[0x03C0]:
- `brhnz r13, 0x298E0` — branches to "no work" idle if r13.u32[0] high halfword ≠ 0
- r13 = 0x1F5F0 (loaded by ila, high halfword=1) → always branches when no workloads registered
- **Kernel correctly identifies "no work available"** and branches to LS[0x298E0]

### "No work" idle state at LS[0x298E0]

LS[0x298E0] is in the kernel's BSS (intentional `stop 0`). On real PS3, LV2 would wake the kernel via mailbox when work arrives. In our emulation, the kernel hits `stop 0` here. The diagnostic burst detects PC≥0x29000 and ends early; the background thread sleeps+restarts in the idle loop (correct behavior).

**Total verified execution: 2837 instructions** from entry through dispatch check to idle stop.

### Scan loop algorithm (fully analyzed via disassembly + trace)

The dispatch path (LS[0x0280]–LS[0x03C0]) executes in three phases:

**Phase 1 (0x0280–0x0308): Setup**
- Saves registers, r81=SPURS_CTX_EA (0x70A000), r10=0 (counter)
- r14=0x1F5B0 (sort order table), r13=0x1F5F0 (priority table / "no workload" sentinel)
- `ila r13, 0x1F5F0` at 0x0308 — sets r13 as BOTH the priority table base AND the no-workload marker

**Phase 2 (0x030C–0x038C): 64-iteration workload priority sort loop**
- r10 = 0..63, terminates at `ceqi r9, r10, 64` / `brz r9, 0x310`
- Each iteration reads from sort table at LS[r10+r14=0x1F5B0] and priority table at LS[r10+r13=0x1F5F0]
- `stqx r20, r10, r11` builds a sorted dispatch table at LS[r10+r11] (LS[0x0C41C+])
- r20 = workload ready bitmask (derived from r79/r77 = DMA'd management area data = 0 currently)

**Phase 3 (0x0390–0x03C0): Dispatch check**
```
0x0394: rotmahi r12, r79, 239   ; r12 = r79>>17 per halfword (workload found indicator)
0x03B4: selb r42, r41, r14, r12 ; blend workload IDs using r12 mask
0x03B8: selb r42, r11, r43, r13 ; blend using r13 mask → r42 = dispatch target
0x03BC: brhnz r12, 0x298D0      ; if r12.high≠0 → idle (stop 0)
0x03C0: brhnz r13, 0x298E0      ; if r13.high≠0 → idle (stop 0)  ← ALWAYS taken
0x03C4+: dispatch workload r42  ; only reached when both r12.high=0 AND r13.high=0
```

**Why dispatch never happens (currently):**
- r79 = 0 (no DMA from management area — kernel never issues MFC DMA commands)
- r13 = 0x1F5F0 (set by `ila`, never updated to a workload ID because r79=0)
- r13.u32[0] >> 16 = 1 ≠ 0 → `brhnz r13, 0x298E0` always branches to idle

**Root cause:** The kernel waits for a PPU mailbox signal (`rdch CH_SPU_RdInMbox`). LV2 would:
1. Receive the stop-0 exception at LS[0x298E0]
2. Check PPU signal queue → find pending workload → restart kernel with r79/r77 populated
3. On restart, r79 ≠ 0 → sort loop finds workload → dispatch

**What's needed for SPU dispatch:**
1. `cellSpursAddWorkload` HLE populates CellSpurs struct at 0x70A000 with descriptors
2. PPU signals kernel: `ctx->inbound_mbox = info; ctx->inbound_mbox_count = 1;`
3. Kernel restarts with management area data → r79/r77 populated → workload dispatched

### LS data structures (from analysis)
| LS Address | Contents |
|---|---|
| `0x1F5B0..0x1F5EF` | Sort order permutation table (64 bytes, values 0x00–0x3F) |
| `0x1F5F0..0x1F62F` | Priority lookup table (values 0x08–0x53) |
| `0x1F630..0x1F64F` | Shift-mask table (0xFFFF, 0x7FFF, ..., 0x0001) |
| `0x0C41C..0x0C45B` | Sorted dispatch table built by sort loop (64 workload slots) |

### All UNIMPL instructions resolved (SPURS dispatch + EDGE geometry)
| op11 | Instruction | Notes |
|------|-------------|-------|
| `0x1D4` | `rotqbii` (RI7, immediate bit count) | SPURS workload init loop |
| `0x1DD` | `shlqbi` (RR, shift-left quad by RB[2:0] bits) | SPURS dispatch code 0x0560 |
| `0x1DF` | `shlqbybi` (RR, shift-left quad by (RB>>3)&15 bytes) | SPURS dispatch code 0x0574 |
| `0x1FD` | `lqx` variant (load quad indexed: `LS[(RA+RB)&~15] → RT`) | EDGE geometry processor — loads task descriptor; key fix for DMA |
| `0x3F8` | `orx` variant (OR across 4 words of RA into RT.u32[0]) | EDGE geometry processor result accumulation |
| `0x2AE` | `xswd` (extend sign word to doubleword) | Already implemented |
| `0x2B6` | `fscrrd` (returns 0 for RT) | Already implemented |

### SPURS kernel dispatch: current verified flow (2980 instructions)

1. Entry (0xD0, 34 insns) → stop-0 at LS[0x154] (first idle)
2. Dispatch path from LS[0x158]:
   - ceqi patch (ilh r2,1 at 0x17C) → brnz fires → dispatch branch at 0x280
   - 64-iteration sort loop (0x310-0x38C): builds priority table in LS
   - brhnz r13 patch (lnop at 0x03C0) → forces dispatch code at 0x03C4+
   - Dispatch code (0x03C4-0x06XX): processes workload descriptors from LS
3. Dispatch signals (15 × stop-0 at LS[0x04..0x3C]):
   - Each stop-0 at LS[0x04], [0x08], ..., [0x3C] = one SPURS workload dispatch signal
   - On real PS3: LV2 intercepts each and starts the SPU workload thread
   - In our emulation: burst loop restarts each time (workload thread not started)
4. Return via stop-0x100 at LS[0x40] → restart from entry 0xD0
5. 20 instructions from 0xD0 → stop-0 at LS[0x20614] (post-dispatch idle)
6. Burst ends: kernel correctly waiting for workloads to complete

**Workload execution (now running for slot 0):**
`spurs_run_workload()` loads workload 1 (guest 0x142900) on first dispatch signal.

Workload 1 ELF: entry=0x3050, code LS[0x3000-0xB3DF], BSS LS[0xB800-0x3D97F] (200KB+).

Workload lifecycle (63 total instructions):
1. Entry pass (30 insns, 0x3050-0x30C4): standard SPURS framework init → stop-0
2. Setup pass (13 insns, 0x30C8-0x30F8):
   - `ila r4, 0xADD0` (LS destination addr)
   - `ilh r3, 0x0100` then rotmai → r3=0x1000100 (SPURS service code)
   - `stop 0x3FFF` ← LV2 task context load request
3. Post-service (3 insns): branches to LS[0x0000] → executes LS[0x04..0x3C] stop signals
4. 15×stop-0 at LS[0x04..0x3C]: workload dispatches its OWN 15 sub-workloads
5. stop-0x100 at LS[0x40]: workload returns

**Multi-level SPURS hierarchy:**
```
SPURS kernel (0x10BD00)  → 15 dispatch signals (LS[0x04..0x3C])
  └── Workload 1 (0x142900)  → stop 0x3FFF (context load r3=0x1000100,r4=0xADD0)
        └── 15 more dispatch signals (LS[0x04..0x3C])
              └── Sub-workloads (actual game code, not yet identified)
```

**stop 0x3FFF = SPURS task context load.** Parameters:
- r3=0x1000100: task type/encoding (bits[24]=1, bits[8]=1, likely task ID or size)
- r4=0xADD0: LS address for LV2 to DMA the task's saved context

On real PS3: LV2 finds the task's code/state from the SPURS taskset and DMA's it to LS[0xADD0]. On first run, provides default initial state (the actual game code). Without implementing this, workload jumps to LS[0x0000] and runs through stop-signal sequence (correct "no task available" behavior).

**Identity of workload 1 (0x142900): Sony EDGE SPU library**
LS[0xADD0] contains: `"EDGE ASSERTION FAILURE: %s(%d)\n"` — confirms this is Sony SCEE's EDGE library, a graphics/geometry processing SPU framework used by many PS3 games.

**stop 0x3FFF = EDGE task scheduler callback.** When EDGE's SPU scheduler calls stop 0x3FFF:
- r3=0x01000100: EDGE function/task code (0x0100 = some EDGE task ID)
- r4=0xADD0: LS context address (points to the assertion string = an EDGE internal table entry)
On real PS3: EDGE's PPU-side task manager handles this, loading the next EDGE sub-task into LS.

**EDGE geometry processor at LS[0x3108]:**
```
ila  r85, 0xBC00          ; r85 = geometry table at LS[0xBC00]
ori  r87, r3, 0           ; r87 = task descriptor arg (from caller's r3)
ilh  r88, 0x1             ; r88 = 1 (some mode flag)
rdch r81, SPU_RdMachStat  ; read machine status
ila  r2, 0x3180; bi r2    ; jump to main processing loop at LS[0x3180]
```
This function reads r3 = EDGE task descriptor (geometry data EAs, output buffer, etc.)

**All 15 workload slots** now run 47 instructions each (EDGE "no task" path).
Total: SPURS kernel (2980) + 15 × EDGE (47 each = 705) = ~3685 SPU insns/cycle.

**EDGE geometry pipeline — fully wired end-to-end:**

Full dispatch: SPURS kernel → slot → EDGE scheduler → stop 0x3FFF → geometry proc → LS→LS GET → vertex processing → DMA PUT → RSX redirect.

`spurs_run_workload()` stop 0x3FFF handler:
- **Scheduler stop** (PC ~0x30FC): writes task descriptor at LS[r4=0xADD0], writes test triangle to LS[0x134], redirects to geometry processor LS[0x3108].
- **Batch complete stop** (PC ≥ 0x3100): breaks cleanly.

**Key EDGE geometry formula discoveries (from SPU register change detectors):**

Vertex source EA path (determines GET):
- `shlqbyi r61, r75, 4` at ~LS[0x3584]: r61.u32[0] = r75 bytes[4..7] = GET EA
- GET EA 0xFE000134 → LS-mapped: reads from LS[0x134] (our test triangle ✓)
- MFC GET from 0xFE000000-0xFEFFFFFF handled as LS→LS DMA (ea-0xFE000000 = LS offset)

Output EA path (determines PUT destination):
- `lqx r43, r49, r124` at LS[0x35FC] (op11=0x1FD): loads quad from LS[(r49+r124)&~15]
- `shlqbyi r31, r43, 4` at LS[0x3614] (op11=0x1FF): r31.u32[0] = r43 bytes[4..7] = PUT EA
- PUT EA = 0x830CBF3F (with sentinel stream data) = some PS3 memory region
- MFC PUT 0x80000000-0xA0000000 redirect → RSX diagnostic buffer at 0xD0101000+
- `rsx_on_edge_write()` fires, data written to RSX — but output is garbage vertices

Additional instruction discoveries at key PCs:
- LS[0x3188]: `ceqbi r21, r22, 0` (op8=0x7E) — byte comparison mask, NOT PUT EA source
- LS[0x3614]: `shlqbyi r31, r43, 4` (op11=0x1FF) — RA is r43 not r21 (earlier analysis error)
- LS[0x35FC]: `lqx r43, r49, r124` (op11=0x1FD) — the actual PUT EA loader

**For PUT EA = 0xD0100000 (RSX command buffer):**
Need r43.u8[4..7] = [0x00, 0x00, 0x10, 0xD0] (LE bytes of 0xD0100000) at time of shlqbyi.
The lqx at 0x35FC loads from LS[(r49+r124)&~15] — this address is computed from the stream
descriptor buffer content. Writing LE 0xD0100000 at bytes[4..7] of THAT specific LS quad
would make PUT EA = 0xD0100000 directly (bypassing the current redirect hack).

**Why output is still garbage:**
Geometry processor processes sentinel bytes (0x30..0x3F) from stream descriptor at 0xA07000.
These are not valid EDGE-format geometry. The output at LS[0xBC80] (written via PUT) is
garbage-transformed garbage. For valid RSX draw commands: needs real EDGE task descriptor
with proper vertex format info and real vertex data.

**EDGE return register setup (required for clean workload return):**
Three rotmai chains in EDGE's exit path all compute `r0 = Rx >> N`; all must yield r0=0x40:
- `gpr[88].u32[0] = 0x80` — `rotmai r0, r88, -1` → 0x40
- `gpr[19].u32[0] = 0x40` — `rotmai r0, r19, 0` → 0x40
- `gpr[114].u32[0] = 0x4000` — `rotmai r0, r114, -8` → 0x40
Without this: saved r0=0 → workload branches to LS[0x0000] instead of LS[0x40], replaying stop signals.

Workload 2 (0x14AE80, ~38KB, entry=0x3050) is also EDGE-based with same entry structure.

**New opcodes in SPURS dispatch code (0x0560 area):**
- `shlqbi` (0x1DD, RR): shift-left 128-bit quad by (RB&7) bits. Same pattern as rotqbi but without wrap.
- `shlqbybi` (0x1DF, RR): shift-left 128-bit quad by (RB>>3)&15 bytes. Same as shlqby but RB>>3.

**New opcodes in EDGE geometry processor (LS[0x3108]+):**
- `lqx` (0x1FD, RR): load quadword indexed — `LS[(RA+RB) & ~15] → RT`. EDGE loads task descriptor quads by index. Without it, MFC_EAL was 0 and no DMA fired.
- `orx` (0x3F8, RR): OR-reduce — `RT.u32[0] = RA.u32[0] | RA.u32[1] | RA.u32[2] | RA.u32[3]`. EDGE geometry result accumulation.

## Game-Specific Patches (in `main.cpp`)

All fixups are applied in `main()` before calling `func_0003B328` (`_start`).

### Pool-manager sentinel self-links
`vm_write32(0x27BBD4, 0x27BBD4)` / `0x27BC3C` / `0x27BCA4` — three linked-list sentinel nodes whose `chain` field (at struct+0x44) must point to themselves for an empty list. The ELF BSS zeros them and no constructor runs to fix them.

### SPURS context stub at `0x700000`
`func_0003AAC4` returns this address. Fields:

| Offset | Value | Reason |
|---|---|---|
| `+0x00` (u16) | `0x2083` | bits 0-1: CE5C4 gate; bit 7: slab-free path in CE77C; bit 13: CE77C real-init path |
| `+0x04` (u16) | `2` | `func_000D5450` checks `>= 2` to enter syscall `0x324` path |
| `+0x10` (u32) | `0x700043` | CE77C checks `struct+8 < struct+0x10`; struct+8 is 0 (BSS), so any non-zero sentinel here lets the loop fire |
| `+0x44` (u32) | `0x700100` | sub-object pointer for `func_000D58C4`; points at zeroed scratch so D58C4 early-returns |

**Why bit 7 must be set:** `func_000D0694` (called before the gate check) inspects bit 7. With bit 7 clear it writes 0 back into `struct+0`, destroying bits 0-1 before `CE5C4` runs → gate always fails. With bit 7 set it takes the slab-free path (`func_000D90FC → func_000D9108`); the slab guard skips the free safely.

**Why bit 13 must be set:** `func_000CE77C` checks bit 13. Clear → takes the `CE8B8` no-op path and the real SPURS init is skipped entirely.

### Slab-free guard in `func_000D9108` (`ppu_recomp.cpp`)
The slab allocator at `0x2E45F0` (via `TOC[-0x603C]`) lives in BSS; its constructor was missed by the lifter. Any free while `slab+0x10 == 0` (pool uninitialized) hits a block-header abort in `func_000D5FD0`. A guard at the top of `func_000D9108` detects `slab+0x10 == 0` and skips the free, logging `[SLAB-SKIP]` to stderr.

### SPURS workload dispatch chain at `0x70A000`
`loc_0003AE74` (inside `func_0003AAC8`) reads `[gpr[30]+0x28]` = `[0x27F81C]` as a SPURS workload pointer, then follows a vtable chain. Without this chain, `[0x27F81C] = 0` causes an infinite LOW-READ32 spin.

The SPURS init code (`func_000CE77C`) zeros `[0x27F81C]` as part of its own setup. `vm_write32` intercepts that zero-write and redirects it to `0x70A000`.

Synthetic chain layout:
```
[0x27F81C] = 0x70A000    ← workload struct ptr (A)
[0x70A000] = 0x70A010    ← vtable-like ptr (B) at A+0x00
[0x70A018] = 0x70A020    ← OPD entry (C) at B+0x08
[0x70A020] = 0x000379BC  ← code ptr (never actually dispatched — see below)
[0x70A024] = 0x0016A0F8  ← TOC
```

**Why `0x70A000` not `0x701000`:** the bump-slab allocator starts its pool at `0x701000`; data placed there gets overwritten on first use.

### `[0x27F814]` zero-write redirect
`func_000355D4` / `func_000355FC` (SPURS states 13/14) read `[0x27F814]` as the head of a struct chain. During SPURS shutdown the init code also zeros `[0x27F814]`. `vm_write32` intercepts that zero-write and redirects it to `0x70B000`, mirroring the `[0x27F81C] → 0x70A000` redirect. Keeps the states-13/14 struct chain alive across SPURS shutdown.

### SPURS state machine — now actually dispatched (`func_000379BC`)
`func_000379BC` is the SPURS workload state machine. The lifter originally compiled the `bctrl` in `loc_0003AE74` (inside `func_0003AAC8`) as a static call to `func_00000030` (the LV2 syscall stub at address `0x30`) — CTR was set to `0x379BC` but ignored. We patched that call site in `recompiled/ppu_recomp.cpp` (~line 62327) to call `func_000379BC(ctx); DRAIN_TRAMPOLINE(ctx);` directly so the state machine actually runs and advances `[0x28B050]` through states `2 → 3 → 4 → 6 → 7 → 8 → 9 → 12 → 13 → 14 → 15 → 21`.

### SPURS state machine pre-patches (`main.cpp`)
- `[0x28B050] = 2` — start the state machine at state 2 (NOT bypass to 21 — that was the old approach)
- `[0x27F831]` — intentionally left at BSS=0. State 12 calls `func_00027090` which reads this byte; pre-setting it to 1 causes states 10/12 to spin (the function expects 0 = "condition clear").
- `[0x70B20C] = 1` — `func_000355D4` (states 13+14) does an equality check `==1`, NOT `>=2`

### State 6 spin-wait skip (`recompiled/ppu_recomp.cpp` ~line 58874, `loc_00037BF4`)
On a real PS3, SPU tasks would write `state=7` externally to break state 6's spin. Without SPU emulation we force `gpr[4]=7` to advance directly.

### State 15 completion (`recompiled/ppu_recomp.cpp`, `loc_00038194`)
Manual writes:
```cpp
vm_write32(0x290000u - 0x4FB0u, 21u);  /* [0x28B050] = 21 = SPURS done */
vm_write8(0x27F830u, 1u);              /* signal func_00027084 to exit loop */
```

### States 13 / 14 struct chain at `0x70B000`
`func_000355D4` and `func_000355FC` (used at SPURS states 13 and 14) read `[0x27F814] → P`, `[P+0x148] → Q`, `[Q+0xC]`. Both return 1 when `[Q+0xC] == 1`. Pre-set: `[0x27F814]=0x70B000`, `[0x70B148]=0x70B200`, `[0x70B20C]=1`.

### `bcctrl` → direct `func_00000030` calls (`ppu_recomp.cpp`)
Several `bctrl` instructions that target address `0x30` (the LV2 syscall gate) were lifted as trampoline-return stubs (`g_trampoline_fn = func_00000030; return`). These cause the calling function to return early instead of continuing. They were patched to `func_00000030(ctx); DRAIN_TRAMPOLINE(ctx);` so execution continues correctly after the call.

### SDU pool bump allocator (`func_00032E58`, `recompiled/ppu_recomp.cpp`)
`func_00032E58` (static stub, ~line 209) is the stwu-prologue wrapper for `func_00032E5C` (the SDU segregated free-list heap allocator at pool `0x27F8B8`). The full allocator is too complex to lift (uses `TODO: subfc`, multi-level free lists) and its backing pool is never initialized (the ctor at `0x14538` runs but only sets up function-pointer tables, not the heap). Replaced with a bump allocator at guest `0x8C0000+` (committed within main RAM, well past the game heap at `0x826000`). Callers only need a valid pointer; no metadata is written at the returned block.

### SDU worker-thread spawners (`recompiled/ppu_recomp.cpp`)
Two NID stubs promote from logging-only to real thread creation:

**`func_000F10FC`** — sdu_yah_size_check spawner. Called with `r7=OPD1, r8=OPD2, r9=priority, r10=thread_arg, r6=id_out_ptr` (sp+0x70 in caller frame). Spawns one thread via syscall 44 from OPD1 with `arg=r10`; writes thread_id to `r6`; returns thread_id in r3.

**`func_000F16DC`** — sdu_yah_all_list_delete spawner. Different register layout: `r3=id_out, r5=OPD1, r6=OPD2, r8=thread_arg`. Same logic.

**`func_000F211C`** — thread join. Called after the spawner with `r3=block, r4=thread_id`. Invokes syscall 43 (`sys_ppu_thread_join`) to block until the worker exits.

Worker stubs in `extra_funcs.cpp` (also in `extra_table`):
| Address | Name | Notes |
|---|---|---|
| `0x000EFE30` | sdu_yah_size_check worker A | OPD 0x1613A0, spawned by `func_000F10FC` |
| `0x000EFEA4` | sdu_yah_size_check worker B | OPD 0x1613A8, (not yet observed spawning) |
| `0x000EFBE8` | sdu_yah_all_list_delete worker A | OPD 0x161388, spawned by `func_000F16DC` |
| `0x000EFD0C` | sdu_yah_all_list_delete worker B | OPD 0x161390, (not yet observed spawning) |

### `func_000F205C` HLE stub (`extra_funcs.cpp`)
sysPrxForUser NID `0xA2C7BA64`, called by the C runtime startup wrapper `func_0003B244` with `r3=0`. Calls game main `func_00012420` (with manually-inserted stwu -0xF0 frame setup), guarded with `s_main_called` to prevent re-entrant calls from the dtor chain. The destructor walker still runs via the natural `func_000CE57C → func_000CE1E8 → func_000F0A78 → func_0003B4FC → func_0003B500` chain.

### GCM init force-successes (`recompiled/ppu_recomp.cpp`)
Three cellGcmSys helper functions fail with `0x80310002`/`0x80310005` because the RSX context pointer `TOC[-0x7FA0]` is null (no real RSX). All three are force-succeeded with early returns:

| Function | Purpose | Error without fix |
|---|---|---|
| `func_0004370C` | cellGcmInit | 0x80310005 — null RSX context |
| `func_00040C0C` | cellGcmGetContextSize | 0x80310002 — null RSX ctx read |
| `func_00040BD4` | cellGcmGetMemorySize | 0x80310002 — null RSX ctx read |

`func_00040BD4` also writes a dummy RSX IO size of `0x200000` (2 MB) to the output pointer (sp+0x84) so the subsequent `func_0004F82C` call gets a non-zero IO size.

### RSX REF register null backend (`runtime_glue.cpp: vm_write32`)
The RSX REF register (guest address `0x4`) is used by the game for synchronization:
1. Game writes `0xFFFFFFFF` as a "pending" sentinel
2. On real PS3, RSX processes a `SET_REFERENCE` command and writes the reference value back
3. CPU polls [0x4] until RSX clears it

Without real RSX, this spin never exits. Fix: writes to `0x4` are **discarded** (logged as `[RSX-REF]`), keeping [0x4] at 0 (BSS). The game reads 0 immediately after its write and the spin exits.

### Two-step entry call (`main.cpp`)
`func_0003B328` (`_start`) runs SPURS init + global ctors, then falls through into `func_0003B244` (C runtime / `main()` / `sys_process_exit`). We replicate this explicitly:
```cpp
func_0003B328(&ctx);    /* SPURS / global-ctor init phase */
DRAIN_TRAMPOLINE(&ctx);
func_0003B244(&ctx);    /* C++ runtime → main() → sys_process_exit */
DRAIN_TRAMPOLINE(&ctx);
```

### Lifter bug: dropped `stwu` prologues (SP runaway)
Some functions have an epilogue that does `gpr[1] += 0xN` but a prologue with no matching `gpr[1] -= 0xN` — the lifter dropped the `stwu r1, -N(r1)` instruction. Each call leaks +N bytes of SP growth; tight loops eventually overrun the committed stack region.

Confirmed instances:
- `func_000379BC` (epilogue `+= 0xE0`) — patched in `recompiled/ppu_recomp.cpp` line 58707 with the missing `vm_write64(gpr[1]-0xE0, gpr[1]); gpr[1] -= 0xE0;`.
- `func_000EFD1C` (epilogue `+= 0xB0`) — the OPD entry for the `sdu_yah_size_check` thread points to `0x000EFD18` (4 bytes before `func_000EFD1C`), exactly where the missing `stwu r1, -0xB0(r1)` would live. Workaround in `extra_funcs.cpp`: a small `func_000EFD18` wrapper that does the missing `stwu` then calls `func_000EFD1C`. Registered in `extra_table` at `0x000EFD18`.
- `func_000EFAD0` (epilogue `+= 0xC0`) — same pattern. The OPD entry for `sdu_yah_all_list_delete` is `0x000EFACC` (4 bytes before `func_000EFAD0`). Wrapper `func_000EFACC` in `extra_funcs.cpp` does `stwu r1, -0xC0(r1)` then calls `func_000EFAD0`. Registered in `extra_table` at `0x000EFACC`.

**When you see SP runaway / AV at `0xE000XXXX` or higher**, suspect another instance of this bug. Diagnostic recipe:
1. Note the AV's `gpr[1]` and the address it tried to access.
2. Inspect the function near the AV (or the most recently called function) — look for a prologue that writes to `gpr[1]+OFFSET` without first subtracting from `gpr[1]`, and an epilogue that does `gpr[1] = gpr[1] + N` somewhere.
3. Either patch the function in place (model after `func_000379BC`) or — if the missing instruction is at an OPD-entry-point address that's 4 bytes before the lifted function — add a wrapper in `extra_funcs.cpp` that does the `stwu` and calls the lifted body (model after `func_000EFD18`).

## Adding Hand-Lifted Functions

When `[ICALL]` logs an unresolved CTR address:
1. Declare in `recompiled/ppu_recomp.h`: `void func_XXXXXXXX(ppu_context* ctx);`
2. Implement in `extra_funcs.cpp` — lift the PPC instructions by hand, include `DRAIN_TRAMPOLINE(ctx)` after calls
3. Add `{ 0x000XXXXXULL, func_000XXXXX }` to the `extra_table[]` array in `extra_funcs.cpp`
4. `ppu_resolve_extra` picks it up automatically via the address table

## ps3recomp SDK

SDK root: `../../` (cmake var `PS3RECOMP_DIR`). Builds `ps3recomp_runtime.lib` into `../../build/`. If missing, CMake adds the SDK as a subdirectory and builds it first. Full SDK docs at `../../docs/`.
