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

`build_run.bat` currently configures cmake without an explicit `-DPS3RECOMP_DIR`, which defaults to `../../` from the project dir → `E:\Games\RecompLauncher` (wrong; the SDK is at `E:\Games\RecompLauncher\ps3recomp`). Configure fails, then `force_rebuild.bat`'s final link step also fails because there's no `build.ninja`.

**Working invocation (PowerShell):**

```powershell
cd E:\Games\RecompLauncher\ps3recomp\dbz-budokai-hd

# One-time configure (or after deleting build\):
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake -B build -G Ninja -S . -DPS3RECOMP_DIR=E:/Games/RecompLauncher/ps3recomp'

# Subsequent rebuilds:
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build'

# Run:
.\build\dbz-budokai-hd.exe ..\game\EBOOT.elf
```

`vcvars64.bat` MUST be sourced inside the same `cmd` session as the `cmake` call (PowerShell's `&` does not propagate child env vars back to the parent). Don't try to run MSVC from Git Bash — it mangles `/flag` arguments as paths.

If the cmake configure cache is broken (missing `build.ninja`), delete `build\CMakeCache.txt` and `build\CMakeFiles\` and reconfigure.

---

## Current status

| Phase | Status |
|---|---|
| ELF load + guest memory setup | ✅ Working |
| Static C++ constructors (via indirect CTR) | ✅ Working |
| SPURS / game-context initialization sequence | ✅ Working |
| `_start` + `func_0003AAC8` complete without abort | ✅ Working |
| SPURS workload state machine runs to completion (states 2 → 21) | ✅ Working |
| `sys_ppu_thread_create` fires; all 4 startup threads run their lifted bodies | ✅ Working |
| C runtime startup wrapper (`func_0003B244` → `func_000F205C`) | ✅ Stubbed-clean |
| C++ destructor walker (`func_000F0A78` → `func_0003B4FC` → `func_0003B500`) | ✅ Working |
| Process exits cleanly with no AV / no abort | ✅ Working |
| **Real LV2 sync primitives** (sem/mutex/lwmutex/event-queue actually block) | 🔲 Next milestone — worker threads currently exit immediately because stubbed waits return success |
| Graphics (RSX / cellGcm) | 🔲 Stubbed |
| Audio (cellAudio) | 🔲 Stubbed |
| Input (cellPad) | 🔲 Stubbed |

Current end-state of a run (`run_stderr.txt`): SPURS init → SPURS workload state machine runs to 21 → 4 worker threads spawn (`sdu_yah_size_check ×2`, `sdu_yah_all_list_delete`, `Terminate Thread`), each runs its lifted body and exits → C++ destructor walker runs → main returns → `[RUNTIME] all game threads finished`.

### Key patches applied

- **Pool-manager sentinel self-links** at `0x27BBD4`, `0x27BC3C`, `0x27BCA4` — the BSS-zeroed ELF doesn't run the C++ constructor that sets these up.
- **SPURS context stub** at `0x700000` — `func_0003AAC4` returns this synthetic page. Fields:
  - `struct+0 = 0x2083`: bits 0-1 (gate), bit 7 (slab path), bit 13 (CE77C real-init path)
  - `struct+4 = 2`: satisfies `func_000D5450`'s `>= 2` check for the syscall `0x324` path
  - `struct+0x10 = 0x700043`: sentinel so `func_000CE77C`'s `struct+8 < struct+0x10` comparison passes
  - `struct+0x44 → 0x700100`: sub-object pointer for `func_000D58C4` early-return path
- **SPURS workload dispatch chain** at `0x70A000` — `[0x27F81C]` is the SPURS workload list head. The SPURS init code zeros it; `vm_write32` intercepts that zero-write and redirects it back to `0x70A000`. A synthetic vtable → OPD chain at `0x70A000` lets `loc_0003AE74` dereference safely without a LOW-READ spin.
- **SPURS state machine wired up** — patched the `bctrl` in `loc_0003AE74` (originally lifted as a static call to `func_00000030`) to invoke `func_000379BC` directly. The state machine now advances `[0x28B050]` through `2 → 3 → 4 → 6 → 7 → 8 → 9 → 12 → 13 → 14 → 15 → 21` instead of being bypassed.
- **State 6 spin-wait skip** — on a real PS3, SPU tasks externally write `state=7` to break state 6's spin. Without SPU emulation we force `gpr[4]=7` in `loc_00037BF4`.
- **State 15 completion** — manual writes in `loc_00038194` set `[0x28B050]=21` (SPURS done) and `[0x27F830]=1` to break the outer dispatch loop.
- **States 13/14 struct chain** at `0x70B000` — `[0x27F814] → 0x70B000`, `[0x70B148] → 0x70B200`, `[0x70B20C] = 1` (`func_000355D4` is an equality check `==1`, not `>=2`).
- **`[0x27F814]` zero-write redirect** — `vm_write32` intercepts zero-writes to `0x27F814` (SPURS shutdown clears it) and redirects them to `0x70B000`, mirroring the existing `[0x27F81C] → 0x70A000` redirect. Keeps the states-13/14 struct chain alive across SPURS shutdown.
- **Pre-set state-machine flag**: `[0x28B050]=2` (start at state 2). `[0x27F831]` is intentionally left at BSS=0 — pre-setting it to 1 causes states 10/12 to spin (they call `func_00027090` which expects 0 = "condition clear").
- **Slab-free guard** in `extra_funcs.cpp` — skips freeing when the slab allocator at `0x2E45F0` is still uninitialized (its constructor was missed by the lifter), preventing a block-header abort.
- **LV2/TLS high region** `0xFFFF0000–0xFFFFFFFF` committed — the `func_0003AAC8` cleanup path writes to guest `0xFFFF9004` (PS3 LV2-mapped TLS area); without this the host throws an access violation.
- **Caller-frame headroom** at `0xE0000000` — one 4 KB page committed above the stack. PPC ABI saves LR into the *caller's* frame at `SP+0xF0` *before* the prologue decrements SP; with initial `SP=0xDFFFFE00` the first save lands at `0xE0000000`, which would AV without this page.
- **`bcctrl` → `func_00000030` direct calls** — the lifter emitted trampoline stubs for several `bctrl` instructions targeting address `0x30` (the LV2 syscall gate). These were replaced with direct `func_00000030(ctx)` calls to avoid incorrect early-return behaviour.
- **`func_000F205C` HLE stub** (`extra_funcs.cpp`) — sysPrxForUser NID `0xA2C7BA64`, called by the C runtime startup wrapper `func_0003B244`. Stubbed to `gpr[3] = 0`; the destructor walker still runs via the natural `func_000CE57C → func_000CE1E8 → func_000F0A78 → func_0003B4FC → func_0003B500` chain.
- **Two-step entry call** in `main.cpp` — the ELF prologue at `0x3B220` calls `func_0003B328` then falls through into `func_0003B244`. We replicate this explicitly: `func_0003B328(&ctx)` → `DRAIN_TRAMPOLINE` → `func_0003B244(&ctx)` → `DRAIN_TRAMPOLINE`.

### Lifter bugs fixed by hand

**Dropped `stwu r1, -N(r1)` prologues (SP runaway).** Some functions kept their epilogue's `gpr[1] += N` but lost the matching prologue subtract — each call leaks +N bytes of SP growth.

- **`func_000379BC`** (`recompiled/ppu_recomp.cpp`) — epilogue `+= 0xE0`, prologue patched in place to add the missing `vm_write64(gpr[1]-0xE0, gpr[1]); gpr[1] -= 0xE0;`.
- **`func_000EFD18` / `func_000EFD1C`** (`extra_funcs.cpp`) — OPD for `sdu_yah_size_check` points to `0x000EFD18`, 4 bytes before the lifted `func_000EFD1C`, exactly where the missing `stwu r1, -0xB0(r1)` would live. Wrapper at `func_000EFD18` does the `stwu` then calls `func_000EFD1C`.
- **`func_000EFACC` / `func_000EFAD0`** (`extra_funcs.cpp`) — same pattern. OPD for `sdu_yah_all_list_delete` is `0x000EFACC` but the lifter emitted `func_000EFAD0`; missing prologue is `stwu r1, -0xC0(r1)`. Wrapper added and registered in `extra_table`.

**Dropped `addc rD, rA, rB` instructions (~770 sites).** The lifter emitted `/* TODO: addc ... */` placeholders for the carry-producing add, silently leaving rD unmodified. This was breaking pointer arithmetic in the heap/list code that runs during C++ static-ctor init. All sites were filled with a uniform `(uint64_t)(uint32_t)rA + (uint64_t)(uint32_t)rB` expression that writes the 32-bit sum to rD and updates `XER[CA]`.

See `CLAUDE.md` for the diagnostic recipe used to find these.

---

## Acknowledgements

- **ps3recomp SDK** — the recompiler pipeline and HLE runtime this port is built on
- Inspired by the broader static recompilation scene (N64Recomp, zelda64recomp, etc.)
