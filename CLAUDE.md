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

When `cmake --build` fails or the Ninja config is stale, use `force_rebuild.bat` which compiles only `main.cpp` and `ppu_recomp.cpp` directly with `cl.exe` and then links all objects. This is the reliable fallback when the CMake/Ninja environment is broken.

## Source Files

| File | Purpose |
|---|---|
| `main.cpp` | Reserves 4 GB guest VA, loads ELF segments, patches pool-manager sentinels + game-context stub, calls `func_0003B328` (`_start`) |
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
  - `0xFFFF0000–0xFFFFFFFF` — PS3 LV2/TLS high area (thread-local storage, kernel-shared structs)
- Always use `vm_read*/vm_write*` helpers — never raw pointer casts (big-endian byte-swap required).
- `vm_write32` intercepts zero-writes to `0x27F81C` (the SPURS workload list head) and redirects them to `0x70A000` to preserve the synthetic dispatch chain — see below.

### LV2 Syscalls (`runtime_glue.cpp: lv2_syscall`)
Dispatches on `ctx->gpr[11]`. Currently handled:

| Syscall | Name | Notes |
|---|---|---|
| 41 | `sys_ppu_thread_exit` | calls `ExitThread` |
| 43 | `sys_ppu_thread_join` | `WaitForSingleObject` on the Windows thread handle |
| 44 | `sys_ppu_thread_create` | allocates guest stack, creates a real Windows thread running `ppu_thread_proc` |
| 45 | `sys_ppu_thread_yield` | `SwitchToThread()` |
| 48 | `sys_ppu_thread_get_id` | returns placeholder |
| 84–88 | semaphores | stubbed (returns 0 / yield) |
| 90–94 | mutexes | stubbed |
| 95–99 | condition variables | stubbed |
| 125–134 | event queues/ports | stubbed |
| 141 | `sys_time_get_system_time` | `GetTickCount64() * 1000` |
| 145 | `sys_time_get_timebase_frequency` | returns 79 800 000 |
| 403 | `sys_tty_write` | prints game debug output |
| 612–616 | lwmutex | stubbed |
| 988 | watchdog | `r3==4` sets `g_abort_called`, dumps call stack |

All others log and return 0. Add new cases directly to `lv2_syscall`.

### Thread Runtime (`runtime_glue.cpp`)
`thread_runtime_init()` initialises the critical section — call before `vm_init()` in `main.cpp`.  
`thread_runtime_join_all()` waits for all game threads — call after `func_0003B328` returns.

Each game thread gets a 1 MB guest stack committed on demand (bump-allocated downward from `0xCFFF0000`). The thread entry OPD is decoded (code ptr + TOC at `opd_ptr` / `opd_ptr+4`) and the resolved host function is called with a fresh `ppu_context`. `g_trampoline_fn` is `__declspec(thread)` so each Windows thread has its own trampoline slot.

### Indirect Calls
`ps3_indirect_call` resolves `ctx->ctr` via `ppu_resolve_addr` (generated table) then `ppu_resolve_extra` (hand-lifted). Unknown addresses log once and stub with `gpr[3] = 0`.

### Process Exit
`func_000F217C` calls `longjmp(g_process_exit_jmpbuf, 1)`; the `setjmp` in `main.cpp` catches it and exits cleanly.

### Trampoline Pattern
Cross-fragment tail calls set `g_trampoline_fn` (TLS). Always drain with `DRAIN_TRAMPOLINE(ctx)` after any call in `extra_funcs.cpp`.

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

### SPURS state machine bypass (`[0x28B050] = 21`)
`func_000379BC` is the SPURS workload state machine. It never actually runs because the lifter compiled the `bctrl` in `loc_0003AE74` as a static call to `func_00000030` (the LV2 syscall stub at address `0x30`) — CTR is set to `0x379BC` but ignored. Pre-setting `[0x28B050] = 21` (the exit state, `0x15`) causes the dispatch loop to exit via `loc_0003AED0` on the very first pass rather than spinning through `loc_0003AEF0` indefinitely.

### States 13 / 14 struct chain at `0x70B000`
`func_000355D4` and `func_000355FC` (used at SPURS states 13 and 14) read `[0x27F814] → P`, `[P+0x148] → Q`, `[Q+0xC]`. Both functions return 1 only when `[Q+0xC] >= 2`. Pre-set: `[0x27F814]=0x70B000`, `[0x70B148]=0x70B200`, `[0x70B20C]=2`.

### Other pre-set flags
- `[0x28A160] = 1` — Thread-2 ready signal checked by state 7 (`loc_00037D3C`)
- `[0x28B090] = 1` — SPURS shutdown confirmation; the spin at `loc_0003B088` exits immediately

### `bcctrl` → direct `func_00000030` calls (`ppu_recomp.cpp`)
Several `bctrl` instructions that target address `0x30` (the LV2 syscall gate) were lifted as trampoline-return stubs (`g_trampoline_fn = func_00000030; return`). These cause the calling function to return early instead of continuing. They were patched to `func_00000030(ctx); DRAIN_TRAMPOLINE(ctx);` so execution continues correctly after the call.

## Adding Hand-Lifted Functions

When `[ICALL]` logs an unresolved CTR address:
1. Declare in `recompiled/ppu_recomp.h`: `void func_XXXXXXXX(ppu_context* ctx);`
2. Implement in `extra_funcs.cpp` — lift the PPC instructions by hand, include `DRAIN_TRAMPOLINE(ctx)` after calls
3. `ppu_resolve_extra` picks it up automatically via the address table

## ps3recomp SDK

SDK root: `../../` (cmake var `PS3RECOMP_DIR`). Builds `ps3recomp_runtime.lib` into `../../build/`. If missing, CMake adds the SDK as a subdirectory and builds it first. Full SDK docs at `../../docs/`.
