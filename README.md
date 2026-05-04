# dbz-budokai-hd — PS3 Static Recompilation Port

A work-in-progress static recompilation of **Dragon Ball Z: Budokai HD Collection** (`EBOOT.elf`, PS3 / BLES01658) to a native Windows x64 executable.

Built on top of the **[ps3recomp](https://github.com/gnome41/ps3recomp) SDK** — a PS3 PowerPC → C++ static recompilation framework.

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
└── ps3recomp/          ← SDK repo (https://github.com/gnome41/ps3recomp)
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
| Game-context initialization sequence | ✅ Working |
| `_start` completes without abort | ✅ Working |
| Game thread execution | 🔲 Next milestone |
| Graphics (RSX / cellGcm) | 🔲 Stubbed |
| Audio (cellAudio) | 🔲 Stubbed |
| Input (cellPad) | 🔲 Stubbed |

### Key patches applied

- **Pool-manager sentinel self-links** at `0x27BBD4`, `0x27BC3C`, `0x27BCA4` — the BSS-zeroed ELF doesn't run the C++ constructor that sets these up.
- **Game-context stub** at `0x700000` — `func_0003AAC4` is stubbed to return this synthetic page. `struct+0 = 0x0003` (bits 0-1 for gate check; bit 7 deliberately clear so `func_000D0694` takes the initializer path, not the slab-free path).
- **Slab-free guard** in `func_000D9108` — skips freeing when the slab allocator at `0x2E45F0` is still uninitialized (its constructor was missed by the lifter), preventing a block-header abort.

---

## Acknowledgements

- **ps3recomp SDK** — the recompiler pipeline and HLE runtime this port is built on
- Inspired by the broader static recompilation scene (N64Recomp, zelda64recomp, etc.)
