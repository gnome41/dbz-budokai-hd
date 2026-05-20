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
| `spu_spurs.cpp` | SPURS/SPU orchestration: loads kernel ELF, patches LS, runs SPU burst loop, dispatches EDGE workloads; `spurs_render_sphere_tick()` generates the animated UV sphere per frame |
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

## Current Status

**Full lifecycle runs cleanly** (8 threads, no AV, no spin):
1. `func_0003B328`: SPURS init + global ctors; state machine advances 2→3→4→6→7→8→9→12→13→14→15→21
2. Four SDU threads spawn sub-workers, join, exit
3. `func_0003B244` → `func_000F205C` → game main `func_00012420`:
   - Loads sysmodules, force-succeeds GCM init, allocates display buffers, returns (pure init, no game loop)
   - `func_000510E4`: pre-patches OPD at `[TOC[-0x6158]]=0x27BBF8` → (0xD3020, 0x16A0F8), spawns UpdateThread via syscall 44
4. `func_000D3020` (UpdateThread): 16 ms idle loop on `g_threads_should_exit`
5. C++ destructor walker fires; program exits cleanly after 5-second window hold

**Game main is pure init** (`func_00012420`): no game loop. Real rendering is SPURS/SPU-driven. NID stubs return 0; no cellSpursAddWorkload calls in the init path. Real EDGE tasks come from the game loop (unreachable currently).

**Rendering** (dedicated thread at ~30fps):
- `render_thread_proc` in `main.cpp`: calls `spurs_render_sphere_tick()` + Sleep(33); started after background load
- Sphere vertex buffer at `vm_base+0xD0180000`; EDGE writes its own output to `0xD0100000` (separate, no conflict)
- `rsx_on_edge_write(put_end_ea, 0xBC80)`: range `> 0xD0180000 && <= 0xD0200000`; `vertex_base = put_end_ea - vtx_count*16`; depth `z > g_z_buf` (init -2.0f); calls `frame_begin` (background blit, z-buf clear) + `edge_rasterize_triangles`
- EDGE MFC PUT to 0xD0100000 calls memcpy only — `rsx_on_edge_write` removed from SPU PUT handler to prevent framebuffer corruption

**AFS textures** (LAUNCH/data.afs, 16 entries decoded by `load_a3t_entry()`):
- Entry 15: A8R8G8B8-LN, 2048×1024 — tournament stage BG (slot 0)
- Entries 1–4: A8R8G8B8-LN or R5G6B5-LN, 2048×1024 — publisher intro screens (slots 1–4)
- Entries 12,13: A8R8G8B8 swizzled (0x85), 512×512 — character portraits (corner overlays)
- Entries 2–7: R5G6B5-LN, 2048×1024 — character-select stage art (not all loaded)
- `load_a3t_entry()`: scans header bytes 0x18..0xC0 for CellGcmTexture struct; pixel data at `gcm_off+0x68`; Morton de-swizzle for format byte with bit 5 clear

**Other key notes:**
- `func_00000030` → `lv2_syscall` in `extra_table`; 604 CTR+`func_00000030` patterns replaced with `ps3_indirect_call`
- `sys_0x324` (syscall 804) seen during init — currently returns 0
- GCM context at 0x70E000 (TOC[-0x7FA0] = 0x162158)

**Outstanding next steps:**
- **SPURS mailbox signaling**: remove lnop patches at LS[0x03BC]/[0x03C0] (brhnz r36/r33), implement PPU→SPU mailbox — restart kernel with r79/r77 populated to enable real workload dispatch
- **SPURS management area**: kernel needs r79/r77 pre-populated at entry; management area at 0x70A000 sets up LS priority/sort tables (LS[0x1F5B0..0x1F62F]) but doesn't DMA at runtime
- `func_000D3020`: implement real bnusCore audio loop (currently 16 ms sleep stub)

### RSX / EDGE rendering infrastructure (`runtime_glue.cpp`)
- `rsx_process_fifo`: PPU RSX FIFO parser on PUT register writes (guest addr 0x10). Handles NV4097_SET_COLOR_CLEAR_VALUE (0x1820), NV4097_CLEAR_SURFACE (0x1D94), NV4097_DRAW_ARRAYS (0x1808).
- `rsx_on_edge_write(put_end_ea, ls_src)`: render thread only; `frame_begin` + `edge_rasterize_triangles(vertex_base, vtx_count)`.
- `edge_rasterize_triangles`: orthographic NDC→pixel, per-pixel barycentric + depth, BGRA8 output.

## SPU Interpreter (`spu_interp.cpp`)

All opcodes needed by SPURS kernel + EDGE geometry processor are implemented (zero UNIMPL messages).

### Completed opcode fixes

| op11 | Mnemonic | Note |
|------|----------|------|
| `0x1D4` | `rotqbii` RI7 | SPURS workload init loop |
| `0x1D6` | `rotqbyi` RI7 | EDGE geometry |
| `0x1DB` | `rotqbi` RR | added |
| `0x1DC` | `rotqbybi` RR | ceqi dispatch path |
| `0x1FC` | `rotqby` RR | was mislabelled |
| `0x07F` | `shlqbyi` RI7 | was no-op |
| `0x1DD` | `shlqbi` RR | SPURS 0x0560 |
| `0x1DF` | `shlqbybi` RR | SPURS 0x0574 |
| `0x1E0–0x1E2` | `cuflt`/`csflt` RI7 | EDGE vertex pack |
| `0x1E4–0x1E7` | `cfltu`/`cflts` RI7 | EDGE vertex unpack |
| `0x1EB–0x1EF` | `fesd`/`frds`/`dfceq` | EDGE double-precision |
| `0x1F8`,`0x1F9` | `hbrr`/`hbrp` (nop) | EDGE branch hints |
| `0x1FD` | `lqx` RR — `LS[(RA+RB)&~15]→RT` | EDGE task descriptor load; without it MFC_EAL=0 |
| `0x3F8` | `orx` RR — OR-reduce RA into RT.u32[0] | EDGE result accumulation |

### SPURS kernel dispatch — key facts

**LS patches applied at load time** (`spu_spurs.cpp`):
- `LS[0x17C]` = `ilh r2, 1` (0x40000082) — forces dispatch branch at 0x184 (ceqi check can't pass naturally)
- `LS[0x03BC]` = `lnop` — bypasses `brhnz r36` (register is r36, NOT r12)
- `LS[0x03C0]` = `lnop` — bypasses `brhnz r33` (register is r33, NOT r13)

**Dispatch check (LS[0x0390–0x03C4])**:
```
rotmahi r12, r79, 239    ; r12 = r79>>17 (workload-found bitmask)
selb r42, r41, r14, r12  ; blend workload ID using r12
selb r42, r11, r43, r13  ; blend using r13 → r42 = dispatch target
brhnz r12, 0x298D0       ; [lnop'd] idle if r12.high≠0
brhnz r13, 0x298E0       ; [lnop'd] idle if r13.high≠0 — ALWAYS taken without patch
; dispatch workload r42  ; reached when both lnop patches allow fall-through
```
With r79=0, r13=0x1F5F0 → high halfword=1 → both brhnz need lnop to force dispatch.

**For real mailbox dispatch**: populate r79/r77 before kernel entry so the sort loop finds actual workloads and r13 gets updated to a valid workload ID (high halfword=0).

**LS data structures**:
| Address | Contents |
|---|---|
| `0x1F5B0..0x1F5EF` | Sort order permutation table (64 bytes, 0x00–0x3F) |
| `0x1F5F0..0x1F62F` | Priority lookup table |
| `0x0C41C..0x0C45B` | Sorted dispatch table (64 workload slots) |

**Workload 1 (0x142900) = Sony EDGE SPU library** (entry=0x3050):
- Issues `stop 0x3FFF` from LS[0x30FC]: r3=0x1000100 (task code), r4=0xADD0 (context LS addr)
- `spurs_run_workload()` handles: writes descriptor at LS[0xADD0], redirects to geometry processor LS[0x3108]
- EDGE geometry processor: LS→LS GET from 0xFE000000+offset, DMA PUT to output EA; `lqx` at LS[0x35FC] loads PUT EA from stream descriptor
- Workload 2 (0x14AE80): also EDGE-based, same entry structure

**EDGE return register values** (required for clean workload exit — set in `spurs_run_workload`):
- `gpr[88]=0x80` → `rotmai r0, r88, -1` → r0=0x40
- `gpr[19]=0x40` → `rotmai r0, r19, 0` → r0=0x40
- `gpr[114]=0x4000` → `rotmai r0, r114, -8` → r0=0x40
Without these, r0=0 → workload branches to LS[0x0000] → replays stop signals indefinitely.

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
