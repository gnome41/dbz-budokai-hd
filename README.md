# dbz-budokai-hd — PS3 Static Recompilation Port

A work-in-progress static recompilation of **Dragon Ball Z: Budokai HD Collection** (`EBOOT.elf`, PS3 / BLES01658) to a native Windows x64 executable.

Built on top of the **[ps3recomp](https://github.com/sp00nznet/ps3recomp) SDK** — a PS3 PowerPC → C++ static recompilation framework.

> **This is a fork / game port built on ps3recomp.**  
> See the SDK repo for the recompiler pipeline, HLE library implementations, and runtime core.

---

## What this repo contains

| File/Dir | Description |
|---|---|
| `main.cpp` | Host entry point: reserves 4 GB guest VA, loads the ELF, applies game-specific patches, calls `_start` |
| `runtime_glue.cpp` | `vm_base`, `vm_read/write*` helpers, `lv2_syscall` dispatcher, `ps3_indirect_call` |
| `extra_funcs.cpp` | Hand-lifted PPU functions missed by the lifter (C++ ctors/dtors reached via indirect CTR calls) |
| `stubs.cpp` | Reference template for NID overrides (not compiled; documentation only) |
| `recompiled/ppu_recomp.cpp` | **Auto-generated** by `ppu_lifter.py` from `EBOOT.elf` — contains all lifted PPU functions (with manual diagnostic/guard patches) |
| `recompiled/ppu_recomp.h` | **Auto-generated** — declares `ppu_context`, memory helpers, all `func_XXXXXXXX` prototypes |
| `config.toml` | Lifter configuration: input ELF, output dir, HLE/LLE module choices |
| `CMakeLists.txt` | Build system (Ninja + MSVC) |
| `CMakeLists.txt` | Build system (Ninja + MSVC) |
| `build_run.bat` | Configure + build via Ninja |
| `force_rebuild.bat` | Force-recompile changed `.cpp` files then link (faster than full cmake rebuild) |
| `run.bat` | Run `build\dbz-budokai-hd.exe ..\game\EBOOT.elf` |

## What's not here (and why)

- **`EBOOT.elf`** and all game data — copyrighted by Bandai Namco / Spike Chunsoft. You must supply your own legally-obtained decrypted PS3 ELF.
- The `build/` directory — generated artifacts.

---

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 (for MSVC + Windows SDK)
- CMake 3.20+, Ninja
- The ps3recomp SDK cloned alongside this repo (see below)
- A decrypted `EBOOT.elf` from *DBZ Budokai HD Collection* (BLES01658)

## Directory layout expected

```
RecompLauncher/
└── ps3recomp/          ← SDK repo (https://github.com/sp00nznet/ps3recomp)
    ├── dbz-budokai-hd/ ← this repo (cloned here)
    └── game/
        └── EBOOT.elf   ← your decrypted ELF (not included)
```

## Build & Run

From a **VS 2022 Developer Command Prompt**:

```bat
cd dbz-budokai-hd
build_run.bat    # configure + build (echoes BUILD_EXIT=0 on success)
run.bat          # run against ..\game\EBOOT.elf
```

---

## Current status

| Phase | Status |
|---|---|
| ELF load + guest memory setup | ✅ Working |
| Static C++ constructors (via indirect CTR) | ✅ Working |
| SPURS / game-context initialization sequence | ✅ Working |
| `_start` + `func_0003AAC8` complete without abort | ✅ Working |
| `sys_ppu_thread_create` fires; threads run and join | ✅ Working |
| Game thread execution (rendering / gameplay loop) | 🔲 Next milestone |
| Graphics (RSX / cellGcm) | 🔲 Stubbed |
| Audio (cellAudio) | 🔲 Stubbed |
| Input (cellPad) | 🔲 Stubbed |

### Key patches applied

- **Pool-manager sentinel self-links** at `0x27BBD4`, `0x27BC3C`, `0x27BCA4` — the BSS-zeroed ELF doesn't run the C++ constructor that sets these up.
- **SPURS context stub** at `0x700000` — `func_0003AAC4` returns this synthetic page. Fields:
  - `struct+0 = 0x2083`: bits 0-1 (gate), bit 7 (slab path), bit 13 (CE77C real-init path)
  - `struct+4 = 2`: satisfies `func_000D5450`'s `>= 2` check for the syscall `0x324` path
  - `struct+0x10 = 0x700043`: sentinel so `func_000CE77C`'s `struct+8 < struct+0x10` comparison passes
  - `struct+0x44 → 0x700100`: sub-object pointer for `func_000D58C4` early-return path
- **SPURS workload dispatch chain** at `0x70A000` — `[0x27F81C]` is the SPURS workload list head. The SPURS init code zeros it; `vm_write32` intercepts that zero-write and redirects it back to `0x70A000`. A synthetic vtable → OPD chain at `0x70A000` lets `loc_0003AE74` dereference safely without a LOW-READ spin.
- **SPURS state bypass** — `[0x28B050]` pre-set to `21` (the exit state). The SPURS workload state machine (`func_000379BC`) never actually runs because the lifter compiled the `bctrl` in the dispatch loop to `func_00000030` statically. Pre-setting state 21 makes the dispatch loop exit cleanly via `loc_0003AED0` on its first pass.
- **States 13/14 struct chain** at `0x70B000` — `[0x27F814] → 0x70B000`, `[0x70B148] → 0x70B200`, `[0x70B20C] = 2` so both `func_000355D4` and `func_000355FC` return 1.
- **Pre-set flags**: `[0x28A160] = 1` (Thread 2 ready signal), `[0x28B090] = 1` (SPURS shutdown confirmation).
- **Slab-free guard** in `extra_funcs.cpp` — skips freeing when the slab allocator at `0x2E45F0` is still uninitialized (its constructor was missed by the lifter), preventing a block-header abort.
- **LV2/TLS high region** `0xFFFF0000–0xFFFFFFFF` committed — the `func_0003AAC8` cleanup path writes to guest `0xFFFF9004` (PS3 LV2-mapped TLS area); without this the host throws an access violation.
- **`bcctrl` → `func_00000030` direct calls** — the lifter emitted trampoline stubs for several `bctrl` instructions targeting address `0x30` (the LV2 syscall gate). These were replaced with direct `func_00000030(ctx)` calls to avoid incorrect early-return behaviour.

---

## Acknowledgements

- **ps3recomp SDK** — the recompiler pipeline and HLE runtime this port is built on
- Inspired by the broader static recompilation scene (N64Recomp, zelda64recomp, etc.)
