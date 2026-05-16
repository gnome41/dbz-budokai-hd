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
| `runtime_glue.cpp` | `vm_base` + `vm_read*/vm_write*` (big-endian, 32-bit truncated addresses); `lv2_syscall` dispatcher; `ps3_indirect_call` for CTR indirect branches |
| `extra_funcs.cpp` | Hand-lifted PPU functions missed by the lifter (ctors/dtors reached only via indirect calls); same `void func_XXXXXXXX(ppu_context*)` signature |
| `stubs.cpp` | Reference template for NID overrides/module hooks — not compiled; kept as documentation |
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

**The program runs cleanly through its full startup → shutdown lifecycle with no AV, no abort, no spin.** Verified end-state (7 threads run, then game main executes):

1. SPURS init runs (`func_0003B328`/`func_0003AAC4`/`func_0003AAC8`).
2. SPURS workload state machine advances `2 → 3 → 4 → 6 → 7 → 8 → 9 → 12 → 13 → 14 → 15 → 21`.
3. Four original SPURS-spawned threads start; each spawns a sub-worker via `func_000F10FC`/`func_000F16DC`:
   - Thread 1: `sdu_yah_size_check` @ `0x000EFD18` (arg `0x289590`) → spawns worker @ `0x000EFE30` → joins it → exits
   - Thread 2: `sdu_yah_size_check` @ `0x000EFD18` (arg `0x289B90`) → spawns worker @ `0x000EFE30` → joins it → exits
   - Thread 3: `sdu_yah_all_list_delete` @ `0x000EFACC` → spawns worker @ `0x000EFBE8` → exits
   - Thread 4: `Terminate Thread` @ `0x00039E20` → exits immediately (no pending requests)
4. Three sub-worker threads run and return (stubs).
5. C runtime wrapper (`func_0003B244` → `func_000F205C`) calls game main `func_00012420`.
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

**Why the game doesn't actually run gameplay:** All 7 PS3 threads are SDU management threads (not the game loop). The `UpdateThread` (game loop thread) is never created because the initialization chain that triggers it is not reached. Specifically, `func_000510E4` (zero-stub, called from game main) is supposed to register a callback that eventually leads to `func_000B8790`. Since `func_000510E4` is a stub, the callback never fires.

**Game loop thread chain (traced from ELF binary):**
- Game loop thread name: `"UpdateThread"` (string at 0xF5C38)
- Thread spawner: `func_000D2F90` (zero-stub at line ~978) → calls `sys_ppu_thread_create` (syscall 44) internally
- Called by: `func_000B89BC` (lines 370581+) → reads TOC[-0x6910] (UpdateThread name) and calls func_000D2F90
- Called by: `func_000B87D8` (line 699353) / `func_000B8794` (line 370428) → first calls syscall 133/130, then on error: calls func_000B89BC
- Called from: `func_000B8790` (stwu wrapper, line 370423) which is in the address table but never dispatched
- Root cause: `func_000510E4` (zero-stub, line 453) should register func_000B8790 as a callback but doesn't

**LV2 syscall gate fix (this session):** `func_00000030` was a zero-stub — every `sc`/`bctrl CTR=0x30` syscall from lifted code was silently dropped. Now forwards to `lv2_syscall`. Address `0x30 → lv2_gate` added to `extra_table` so that the 604 mass-replaced `ctx->ctr=...; ps3_indirect_call(ctx)` patterns still route correctly when CTR=0x30.

**Outstanding questions for the next iteration:**
- Lift `func_000510E4` (0x510E4, zero-stub) from binary — this function registers the callback chain that triggers `func_000B8790` and ultimately creates the UpdateThread game loop.
- Implement `func_000D2F90` (zero-stub) — the game's internal PPU thread creation wrapper: called with r3=config/name, r4=thread_name, internally loads thread entry from a global struct and calls sys_ppu_thread_create.
- Identify NID 0xA2C7BA64 (`func_000F205C`) to understand what the C runtime calls after init.
- Identify the 26 sysPrxForUser NIDs imported by this ELF.
- Implement a real RSX/graphics backend (currently fully null).

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
