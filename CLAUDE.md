# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

A static recompilation of *Dragon Ball Z: Budokai HD* (`EBOOT.elf`) to a native Windows x64 executable, built on top of the **ps3recomp SDK** located two levels up at `../../` (i.e., `E:\Games\RecompLauncher\ps3recomp\`). See that directory's CLAUDE.md for SDK-wide architecture.

The PS3 PPU instructions were lifted offline by `ppu_lifter.py` into `recompiled/ppu_recomp.cpp`; this project wires up the lifted code with a hand-written runtime, stubs LV2 syscalls, and patches game-specific data structures.

## Build & Run

```bat
build_run.bat    # configure + build via Ninja (echoes BUILD_EXIT=)
run.bat          # build\dbz-budokai-hd.exe ..\game\EBOOT.elf
```

The ELF must be a decrypted PS3 ELF64 at `E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf` (outside this repo). No automated tests — verify by reading stderr diagnostics.

## Source Files

| File | Purpose |
|---|---|
| `main.cpp` | Reserves 4 GB guest VA, loads ELF segments, patches pool-manager sentinels + game-context stub, calls `func_0003B328` (`_start`) |
| `runtime_glue.cpp` | `vm_base` + `vm_read*/vm_write*` (big-endian, 32-bit truncated addresses); `lv2_syscall` dispatcher; `ps3_indirect_call` for CTR indirect branches |
| `extra_funcs.cpp` | Hand-lifted PPU functions missed by the lifter (ctors/dtors reached only via indirect calls); same `void func_XXXXXXXX(ppu_context*)` signature |
| `stubs.cpp` | Reference template for NID overrides/module hooks — not compiled; kept as documentation |
| `recompiled/ppu_recomp.cpp` | **Auto-generated** — never edit. Re-run `ppu_lifter.py` to regenerate |
| `recompiled/ppu_recomp.h` | **Auto-generated** — declares `ppu_context`, memory helpers, all `func_XXXXXXXX` prototypes |
| `config.toml` | Lifter config: input ELF path, output dir, HLE/LLE module choices, memory/threading/graphics/audio settings |

## Key Runtime Details

### Guest Memory Layout
- `vm_base`: 4 GB `MEM_RESERVE` via `VirtualAlloc`. Guest 32-bit addresses index as `vm_base[uint32_t(addr)]`.
- Committed: `0x00010000–0x10010000` (256 MB main RAM) and `0xD0000000–0xE0000000` (256 MB stack).
- Always use `vm_read*/vm_write*` helpers — never raw pointer casts (big-endian byte-swap required).

### LV2 Syscalls (`runtime_glue.cpp: lv2_syscall`)
Dispatches on `ctx->gpr[11]`. Currently handled:
- `403` — `sys_tty_write`: prints game debug output
- `988` — watchdog; `r3==4` sets `g_abort_called` and dumps call stack

All others log and return 0. Add new cases directly to `lv2_syscall`.

### Indirect Calls
`ps3_indirect_call` resolves `ctx->ctr` via `ppu_resolve_addr` (generated table) then `ppu_resolve_extra` (hand-lifted). Unknown addresses log once and stub with `gpr[3] = 0`.

### Process Exit
`func_000F217C` calls `longjmp(g_process_exit_jmpbuf, 1)`; the `setjmp` in `main.cpp` catches it and exits cleanly.

### Trampoline Pattern
Cross-fragment tail calls set `g_trampoline_fn` (TLS). Always drain with `DRAIN_TRAMPOLINE(ctx)` after any call in `extra_funcs.cpp`.

## Game-Specific Patches (in `main.cpp`)

These manual fixups are applied before calling `_start`:
- Pool-manager sentinel self-links at `0x27BBD4`, `0x27BC3C`, `0x27BCA4`
- Game-context stub at `0x700000`: bits 0-1 of `struct+0` set (**not** bit 7), `struct+0x44` pointed at zero-initialized scratch at `0x700100`

### Why bit 7 must NOT be set in the game-context stub
`func_000D0694` inspects bit 7 of `struct+0`:
- bit 7 = 0 → routes to `func_000D0700` (initializes internal linked-list fields, safe for our stub)
- bit 7 = 1 → routes to the **slab-free** path (`func_000D90FC` → `func_000D5FD0`), which aborts because `0x700000` is not a real slab block

The gate check (`func_000CE5C4`) only needs bits 0-1, and it runs **before** `func_000D0700` zeroes `struct+0`, so clearing those bits in `D0700` is harmless.

### Slab-free guard in `func_000D9108` (`ppu_recomp.cpp`)
The slab allocator at `0x2E45F0` (accessed via `TOC[-0x603C]`) is in BSS and its constructor was missed by the lifter. Any slab-free attempt while `slab+0x10 == 0` (pool start uninitialized) hits a block-header abort in `func_000D5FD0`. A guard in `func_000D9108` detects this condition and silently skips the free, logging `[SLAB-SKIP]` to stderr.

## Adding Hand-Lifted Functions

When `[ICALL]` logs an unresolved CTR address:
1. Declare in `recompiled/ppu_recomp.h`: `void func_XXXXXXXX(ppu_context* ctx);`
2. Implement in `extra_funcs.cpp` — lift the PPC instructions by hand, include `DRAIN_TRAMPOLINE(ctx)` after calls
3. `ppu_resolve_extra` picks it up automatically via the address table

## ps3recomp SDK

SDK root: `../../` (cmake var `PS3RECOMP_DIR`). Builds `ps3recomp_runtime.lib` into `../../build/`. If missing, CMake adds the SDK as a subdirectory and builds it first. Full SDK docs at `../../docs/`.
