# Event-Cycle Build-Out Plan

The plan for getting DBZ Budokai HD past pure-init and into its event-driven game/menu loop.

## Why this is needed (the wall)

Confirmed from six independent angles (static address scans, CRT decode, ELF relocation check,
runtime OPD-build trace, thread-spawner backtrace, event-infra check):

- The CRT makes exactly **one** `main` call: `_start (func_0003B328) → func_000CE578 →
  func_0003B244 → func_000F205C (import hijack) → func_00012420`. `func_00012420` is **pure init**
  (sysmodules, force-GCM, display buffers) and **returns 0**. Then destructors run; process exits.
  There is no loop anywhere in the PPU entry path.
- After init the render layer is **completely dormant**: no `sys_event_queue_create` (syscall 125),
  no `cellGcmSetVBlankHandler`/`SetFlipHandler`, no RSX FIFO PUT writes, no `cellSpursAddWorkload`.
- The game is meant to be driven by **RSX-generated VSYNC/flip events** delivered through event
  ports/queues to consumer threads that then submit SPURS/EDGE render work. Our HLE never produces
  those events, and — critically — the consumer threads + handlers are **registered in
  subsystem-init code that is never reached** after `func_00012420` returns.

So this is not an incremental-unstub problem. It needs a coordinated build-out of the event cycle
**and** a way to make the gated subsystem-init code actually run.

## What already works (build on these)

- Event queue/port LV2 syscalls 125–134 are implemented (`runtime_glue.cpp`): circular buffer +
  counting semaphore + critical section per queue; `sys_event_port_send` (134) enqueues.
- `cellGcmTickVBlank` / `cellGcmTickFlip` (`libs/video/cellGcmSys.c`) are called every ~33 ms from
  `render_thread_proc` (`main.cpp`) and dispatch a registered guest handler via `g_ps3_guest_caller`.
- The software rasterizer + RSX FIFO parser + AFS texture loader are in place — once the game emits
  draw commands or EDGE workloads, there is a path to the screen.
- Real bnusCore (audio) init runs to completion (`BNUSCORE_REAL_INIT`).

## Phase A — FINDING: the root divergence is the stubbed GCM/RSX context

(2026-06) Located the root cause of the dormant event cycle:

- The game does **not** use the cellGcmSys HLE imports at all (zero `[cellGcmSys]` HLE calls in a
  full run). It drives GCM through its **own internal wrappers** — `func_0004370C` (cellGcmInit),
  `func_00040C0C` (GetContextSize), `func_00040BD4` (GetMemorySize) — which we **force-succeed with
  early returns**.
- The real `func_0004370C → func_00043778` reads the RSX/GCM context pointer at `[TOC-0x7FA0]`
  (= guest `0x162158`, our synthetic context `0x70E000`) and dereferences its fields
  (`[ctx+0x18]`, `[ctx+0x1C]`, `[ctx+0x48]`, etc.). Because `0x70E000` is a **stub without the real
  fields**, the real init fails/crashes — hence the force-success.
- **Consequence:** force-succeeding GCM init skips the game's entire real display setup — creating
  display buffers, the flip queue, and registering the **vblank/flip mechanism that is the source of
  the event cycle**. So there are no events to deliver and no handler to receive them. The cycle is
  dead at its source.

**Revised priority:** the event-cycle build-out is really a **GCM-completion** task. The most
direct path:
1. Build a functional-enough GCM context object at `0x70E000` (decode the field layout the real
   `func_0004370C`/`func_00043778`/`func_00040C0C`/`func_00040BD4` read and write).
2. Un-force-succeed those wrappers (one at a time) and let the real GCM init run.
3. Trace what it sets up — display buffers, flip queue, and crucially the vblank/flip registration —
   then wire our 30fps render tick to drive that real flip/vblank path (Phase B).
4. With real vblank events flowing, the game's consumer/menu thread should be spawned and its
   handlers fire (Phase C/D).

`func_00043778` field reads to satisfy: `[ctx+0x18]` (checked != 0), `[ctx+0x1C]` (written 0),
`[ctx+0x48]` (written `[ctx+0x18]`), plus the error-code paths (`0x80310003`/`0x80310006`). Decode
the full struct before un-stubbing.

### Phase C progress (WIP — gated behind `GCM_REAL_INIT`, default OFF)

The real cellGcmInit now **runs** against our synthetic context and makes real progress:

1. **`func_0004370C` un-force-succeeded** (`#ifndef GCM_REAL_INIT` guard in ppu_recomp.cpp).
2. **GCM context fix**: set `main.cpp [0x70E000+0x18]=0` (and `+0x1C=0`). With `+0x18` non-zero the
   real init bailed `0x80310006` ("already initialized"); with 0 it proceeds. (Default build keeps
   `+0x18=0x200000` for the force-success path — flip to 0 only with `GCM_REAL_INIT`.)
3. **Memory allocators implemented**: `func_000F1F7C` / `func_000F20FC` were stubs returning 0;
   now bump-allocate from a committed pool at `0x0A000000` (`sysmem_alloc`). Verified called by the
   real init: `func_000F20FC(0x110000)=0x0A000000`, `func_000F1F7C(0x1080)=0x0A110000`,
   `func_000F1F7C(0x1C8)=0x0A112000`.

**Result so far:** the real cellGcmInit does **~150+ real RSX control-register writes** (guest
`0x0`/`0x8`/`0xFDC`/`0xFF0`, IO-mapping table `0x1B4..0x208`) and completes its first allocations,
but still returns **`0x80310005`** at a **deeper requirement** — the next blocker is most likely
`func_000F1EFC` (a `sys_*` mutex/sync-create import, called at func_00043778 +~0xBC/+~0xDC) returning
error. The init also calls `func_000424E8` (an internal helper) and reads globals `[TOC-0x7F30]`,
`[TOC-0x7F2C]`, `[TOC-0x7F28]`, `[TOC-0x7F24]`.

**Next steps to keep going:** enable `GCM_REAL_INIT` + `[0x70E000+0x18]=0`, then implement
`func_000F1EFC` (lwmutex/event create → valid handle) and any further imports `func_00043778` needs,
iterating until cellGcmInit returns 0. Once it succeeds, trace whether it registers the flip/vblank
mechanism and sets up the display buffers — then wire the render tick to that real path (Phase B).

### Phase C progress (update 2): the flip/vblank thread + the current blocker

**Major find:** `func_00043778` (real cellGcmInit) **spawns a thread** via `func_000F1EBC`
(`sys_ppu_thread_create`) at its `loc_0004396C` path — args: `opd=[TOC-0x7F24]`, `name=[TOC-0x7F20]`,
`stack=0x8000`. This is almost certainly the **GCM flip/vblank thread = the event source** we need.
Getting init to reach and pass this spawn is the immediate goal.

**Current blocker (pinpointed via `ps3_debug_backtrace` from `func_000430B0`):** the error fires at
`func_00043778+0x164D` — **before** the flip-thread create — returning `0x80310005`. The real init
gets through: size computation (`func_00042CEC/D18/D40`), 3 allocations (`func_000F20FC(0x110000)`,
`func_000F1F7C(0x1080)`, and `func_000424E8`'s internal `func_000F1F7C(0x1C8)`), buffer init
(`func_000424E8`→`func_000424EC`), and the lwmutex setup — then a check fails. The most likely cause:
`func_000F1EFC` (called 2–3× with `r3 = ctx+0x508 / ctx+0x4E0 / ctx+0x30`) is a **stub returning 0
without initializing the lwmutex struct at `r3`**, so a subsequent handle/state check fails.

**Immediate next task:** implement `func_000F1EFC` as a real `sys_lwmutex_create` — initialize the
lwmutex struct at `r3` (zero it + valid sentinel/handle) and return 0. Then re-enable `GCM_REAL_INIT`
and re-run; the error should advance past `+0x164D` toward the `func_000F1EBC` flip-thread create.
Reusable tool: `ps3_debug_backtrace("tag")` in runtime_glue.cpp names the recompiled call chain.

## Phase A (continued) — Map the divergence (instrumentation)

Goal: find the exact function(s) that, on real hardware, would (1) create the event queue, (2)
register the VBlank/flip handler, and (3) spawn the menu/consumer thread — and find why our run
never reaches them.

1. **Full indirect-call + branch trace** of the whole init (gate behind a flag). For every
   `ps3_indirect_call`, log `CTR` + a host backtrace once per unique target. Diff against the set of
   functions we *expect* to run (anything referencing `cellGcmSetVBlankHandler`, `sys_event_queue_create`,
   `sys_event_port_connect_local`, or `func_000F1EBC` thread spawns).
2. **Find the handler-registration call site**: locate the import thunk for `cellGcmSetVBlankHandler`
   (NID `0xA547ADDE`) and `cellGcmSetFlipHandler` (`0xD9B7653E`), then find the lifted function that
   calls it. That function is the render/display setup; trace backward (what should call it) to find
   the gate that skips it.
3. **Enumerate the unspawned thread group**: "Initialize Thread"/"Regist Context Thread"/"Unlock
   Thread" (names @ 0xF3200/0xF3214/0xF322C). Find their `func_000F1EBC` spawn sites and the
   conditions gating them. ("Regist Context Thread" is the most render-relevant — register RSX context.)

## Phase B — Event source

Goal: produce a steady VSYNC/flip event the game can consume.

1. Implement `sys_event_port_connect_local` (132 already partial) and the full
   create→connect→send path so a guest queue can receive port events.
2. From `cellGcmTickVBlank`/`cellGcmTickFlip`, in addition to the direct handler call, **post a
   VSYNC/flip event** (`sys_event_port_send`-equivalent) to the GCM-registered event port/queue once
   the game has created one. Keep the cadence at the existing ~30 fps render tick (later: 60).
3. Maintain the GCM flip-completion label/REF semantics so any flip-wait spin sees completion.

## Phase C — Make the gated init run

Goal: get the registration + consumer-thread spawn to actually execute.

Two complementary approaches (use Phase-A findings to pick):
- **Fix the gate**: if a specific stub/return-0 (e.g., a sysutil/GCM/event NID or a subsystem-init
  function) diverts the path away from the registration, give it a plausible non-zero/success value
  so the real path is taken.
- **Force-run**: if the registration/spawn function is cleanly identified, invoke it directly after
  `func_00012420` returns (from `main.cpp`, before `thread_runtime_join_all`) with correct args, then
  let the event source (Phase B) drive it.

## Phase D — Iterate the live loop

Once a handler fires on VSYNC (or the consumer thread wakes on a queued event):
1. Trace what it does — it should advance game state and submit render work
   (`cellSpursAddWorkload` and/or RSX FIFO writes).
2. Implement `cellSpursAddWorkload` for real (load the SPU ELF, run it via `spu_spurs`/`spu_interp`)
   and/or extend the RSX FIFO parser for the methods the menu emits.
3. Fix the next gap revealed; repeat until the menu renders.

## Milestones / definition of done

- **M1**: a guest event queue is created and a VBlank/flip handler is registered (diagnostics show
  `[GCM] handler=SET` and a `sys_event_queue_create`).
- **M2**: that handler/consumer fires every frame (not just once) and advances game state.
- **M3**: the game submits render work each frame (RSX FIFO draws or `cellSpursAddWorkload`).
- **M4**: the launcher background + UI render — the "choose which game" menu is visible (text may be
  blank initially; `cellFont` is a later track).

## Risks / unknowns

- The registration may be gated on a real subsystem response we still stub (sysutil callback, save-data
  check, NP status) — Phase A must find which.
- Force-running a thread/registration out of its intended order may corrupt state; prefer fixing the
  gate over force-running where possible.
- Some render work is SPU/EDGE-only; reaching M3 may require real `cellSpursAddWorkload` execution,
  which is its own sub-project (the SPURS/EDGE pipeline already exists for the sphere).

## Reference (addresses / tooling)

- Entry chain: `func_0003B328` → `func_000CE578` (0xCE578) → `func_0003B244` → `func_000F205C` →
  `func_00012420`. Real bnusCore init: `func_000510E4` (`BNUSCORE_REAL_INIT`).
- bnusCore root object pointer: `[0x279B08]` (guard in `vm_write32`).
- Thread spawner: `func_0003AAC8` (SPURS state machine) spawns "Terminate Thread" via `func_000F1EBC`.
- ELF tools (repo root): `_disasm_d3020.py`, `_readptr.py`, `_scan.py`, `_scanbl.py`,
  `_scanconst.py`, `_relas.py`, `_strings.py`. OPD-build trace: `g_opd_trace` in `runtime_glue.cpp`.
- Full investigation log: `memory/project-game-loop-architecture`.

## Phase C update — `func_00043778` is the nuSound2 audio init, NOT cellGcmInit (dead-end for the menu)

The `GCM_REAL_INIT` experiment was pursued to completion enough to identify the function
definitively. Findings:

- With the memory allocators implemented (`func_000F1F7C` size=r5/align=r4, `func_000F1F3C`
  size=r4, `func_000F20FC`) and `func_000F1EFC` writing a real `sys_lwmutex_t`, the init runs
  far past the old `0x80310005` wall (the wall was `func_000484B4`→`func_00048520` calling the
  stubbed `func_000F1F3C`, which returned 0).
- It then **spawns a chain of audio worker threads whose bodies were never lifted**:
  `_sys_MixerChStripMain` (entry `0x47F70`, ctx arg `0x0A112000`),
  `_sys_mixerSurBusReq` (entry `0x41F8C`, arg `0x70E4D0`), and more. Each uses a
  producer/consumer handshake: the init spawns the thread, then **spin-waits** on a ready flag
  (`[mixer_ctx+0x90]` for the first one) that the thread is supposed to raise. Because the
  thread bodies aren't lifted, the runtime stubs them ("UNRESOLVED entry") and the flag never
  rises → silent CPU spin.
- Stubbing one thread's ready flag (`func_00047F70` in `extra_funcs.cpp` writes `[arg+0x90]=1`)
  releases that spin and the init advances to the **next** thread's spin (`func_00042754`). This
  is an open-ended sequence of audio worker threads.

**Conclusion:** `func_00043778`/`func_0004370C` are the **nuSound2 / MultiStream AUDIO** init
(the args to `func_0004370C` — `r4=0x190, r5=4, r6=1, r7=2, r8=3` — do not match
`cellGcmInit(context, cmdSize, ioSize, ioAddress)`). This is **not** the menu path and not the
flip/vblank event source. Game-main `func_00012420` already completes WITHOUT this init: in the
default build it fails gracefully with `0x80310005` and the program still reaches clean exit
(221102 stderr lines, "all game threads finished"). The menu blocker remains the missing event
cycle (no `sys_event_queue_create`, no VBlank/flip handler registration, no
`cellSpursAddWorkload`), per M1–M4 above.

**State:** all of the above is kept behind `GCM_REAL_INIT` (default OFF, commented in
`ppu_recomp.cpp`). The diagnostics added this phase — `GCM_SPIN_DETECT` (a hammered-poll-address
backtracer in `vm_read32`) and the `[GCM-42C2C]` trace — are likewise gated off. Default build is
the verified clean baseline. Next real step is Phase A/B above (find/stand up the event cycle),
not further audio unstubbing.

## Phase A result — the gate that spawns "Initialize Thread" is the `func_00024DE0` stub

Traced the exact condition that decides whether the orchestration threads ("Initialize
Thread"/"Regist Context Thread"/"Unlock Thread", name strings @0xF3200/0xF3214/0xF322C) spawn:

- They are spawned by the virtual methods `func_000317EC` / `func_00031850` / `func_000318A4`
  (each calls `func_000F1EBC` = `sys_ppu_thread_create`). These are vtable slots 3/4/5 of a
  thread-manager class (vtable @0xF6D18; the vtable's OPDs live at 0x161160..; RTTI text
  "vector<T> too lo..." at vtable+0x30).
- That manager is constructed by `func_00031548`, called via `bl 0x31544` (= ctor-4) at 0x2563C
  inside **`func_00024DE4`** — the game's "full game-world init" (allocates world objects via
  `func_00033944`, wires sub-objects, then builds the thread-manager).
- `func_00024DE4` is invoked from the **running** SPURS state machine `func_0003AAC8` at
  `loc_0003AD94` via `bl 0x24DE0` (= `func_00024DE4 - 4`). The lifter dropped the leading
  `stdu r1,-0x160(r1)` (raw 0xF821FEA1 @0x24DE0; 0x24DE4 = `mflr`), so the call target 0x24DE0
  is not a real function entry. A prior fix lifted it as a **stub** `func_00024DE0` returning 1
  (returning 0 made `func_0003AAC8` take an early-exit branch and skip game-world init), but
  returning 1 **skips the real body entirely** — so the manager is never constructed and the
  threads never spawn. THIS is the gate.

**The real-condition fix** (gated `GAMEWORLD_REAL_INIT`, default OFF in `ppu_recomp.cpp`):
restore the dropped `stdu` and run the real body —
`vm_write64(r1-0x160, r1); r1 -= 0x160; func_00024DE4(ctx); drain;`. Enabling it makes the
game-world init actually run, but it is a deep bring-up: it currently **AVs at guest 0x10010000**
(main-RAM ceiling) in the bnusCore path (`func_00012420 -> func_0004BA74 -> func_0005E2B0 ->
func_0005E09C -> func_0005D564 -> func_0005D2B0 -> func_000D242C -> vm_write8`) — i.e. the partial
world init leaves state the later audio path overruns (likely another unlifted accessor returning
0 used as a buffer base, same shape as the `func_0005AA3C` family fixes). So the gate is found and
the door is open, but walking through it is the project-scale game-world bring-up, one dependency
at a time, starting from that AV.

Note: every function in this subsystem uses the off-by-4 (`func-4`) OPD/bl entry where the lifter
dropped the `stdu` — when `GAMEWORLD_REAL_INIT` is on, expect to add missing-`stdu` wrappers for
the thread-manager methods too (0x317E8/0x3184C/0x318A0/0x316D4/0x31544, etc.), mirroring the
sdu-worker wrappers (`func_000EFD18`).

## Phase A bring-up start — the systemic off-by-4 dropped-stdu bug (687 functions)

Working through the game-world bring-up surfaced the root mechanism behind the whole
"OPD dispatch hits a stub" family. `_scan_offby4.py` finds **687** functions where the lifter
dropped the leading `stdu/stwu r1,d(r1)` prologue: the OPD/bl points to `func-4`, lifted as a
one-line `{gpr[3]=0}` stub, with the real body at `func`. Every OPD/virtual/function-pointer/
direct-bl dispatch therefore hits the stub and returns 0 instead of running the function. This is
why individual cases were hand-wrapped before (func_000F205C for game main, func_000EFD18 for the
sdu workers). `_fix_offby4.py` converts all of them (minus game-main 0x1241C) to gated
missing-stdu wrappers (`#ifdef GAMEWORLD_REAL_INIT`: do the dropped stdu + call func+4; `#else`:
original stub) so the default baseline is byte-identical.

Progress with `GAMEWORLD_REAL_INIT` ON:
1. func_00024DE0 wrapper runs the real game-world init func_00024DE4 and forces r3=1 (func_00024DE4
   still returns 0 = incomplete, but returning 0 made func_0003AAC8 early-exit and skip the bnusCore
   root init). With r3 forced to 1, [0x279B08] now initialises and the path-string strcat-overrun
   AV at 0x10010000 is cleared; new threads spawn (cri_dlg x2).
2. Next AV: garbage read 0x87068D9C in func_0001BAE0 (bnusCore), reached from func_0003AAC8
   @loc_0003ADE8 via the now-unstubbed func_0001BADC.

**Key learning:** enabling the blanket un-stub wholesale is too broad for the current partial
emulation — it runs functions in unrelated subsystems (bnusCore) that AV on un-set-up state, and
func_00024DE4 still doesn't truly succeed. The next step is to **scope** the un-stub to the
game-world/thread-manager subtree only (func_00024DE4's call-tree + the thread-manager methods
0x317E8/0x3184C/0x318A0/0x316D4/0x31544), excluding bnusCore, OR to make func_00024DE4 genuinely
succeed. Tooling + gated fix are committed (default OFF); baseline verified clean (221k lines,
all game threads finished, no AV).

## Phase A bring-up — deeper progress and the SPURS-coupling wall

Continuing with the deny-list approach (keep bnusCore funcs that AV stubbed):

1. Re-stubbed func_0001BADC (bnusCore, AV'd reading garbage 0x87068D9C) via _restub.py.
   With it denied, the game-world init runs MUCH further (reads low-mem structs, does
   indirect dispatches, slab-allocs; cri_dlg threads spawn) instead of AV'ing.
2. New blocker: a CPU spin (confirmed ~1.3 cores) in a parser loop func_000D9470 polling
   byte [0x10BC28] forever, 11 frames deep:
   func_0003AAC8 -> func_0001D41C -> func_0001CDE0 -> func_000EB670 -> func_000E7870 ->
   func_000E8844 -> func_000D9470.  The loop re-invokes the REAL SPURS init func_000CE77C
   (and func_000CE9A0) and only advances when SPURS makes progress — which our
   partially-faked SPURS (synthetic dispatch chains, lnop LS patches, no real SPU mailbox)
   does not do.  Also seen: unresolved ICALLs 0xEDCDC (unlifted), 0xEB2E0 (= func_000EB2E4-4,
   off-by-4 with no stub), 0xEB214; and a null CTR=0 virtual dispatch (uninitialised vtable).

**Assessment:** the game-world init (func_00024DE4 subtree) is tightly coupled to real
SPURS/SPU execution and inter-thread coordination that we only partially emulate.  Forcing
it forward yields progressively more-broken state (AV -> re-stub -> spin in faked SPURS).
This is NOT tractable by incremental force-forward; reaching the menu this way needs either
(a) real SPU mailbox/dispatch emulation so func_000CE77C-driven loops advance, or (b) a
principled map of exactly which init steps spawn the orchestration threads, then satisfying
only those.  Both are major efforts beyond this phase.

Tooling added: _restub.py (deny-list re-stub), vm_read8 spin detector (gated GCM_SPIN_DETECT).
All experiments gated OFF; baseline verified clean (221102 lines, all game threads finished).

## Phase A BREAKTHROUGH — the "SPURS wall" was a lifter bug; surgical 64-bit add fix clears it

Diff-checking the upstream SDK (sp00nznet/ps3recomp) revealed the game-world spin and the
strcat AV were NOT SPURS coupling — they were the upstream-fixed lifter correctness bugs:
- 0a7c56e: PPC add/subf emitted 32-bit-truncated; the SWAR word-at-a-time strlen never sees
  the high-word terminator and scans forever. Our generated code had 11687 add + 3374 subf
  truncated sites.
- Also: prologue detection (off-by-4) and load-with-update (ldu base never advances).

Rather than a full base-swap (the auto re-lift restructures ~1000 functions; even with
--functions it leaves ~940 externals), applied the add/subf fix SURGICALLY in place via
_apply_addfix.py: 11687 add + 3374 subf -> full 64-bit form. Verified the clean baseline is
unchanged (221102 lines, all threads finished) — the corrected arithmetic is a safe no-regression.

RESULT with GAMEWORLD_REAL_INIT on: the func_000D9470 spin is GONE (the add fix fixed its SWAR
sub-call func_000D0468, so r26 += r30 now advances). The game-world init runs deeper into REAL
game logic and hits the game's own abort()/assert (func_000D91E4 = the abort handler, syscall
988 r3=4). Lead-up: parse pointer r29 advanced to 0x10BC20 (SPURS embedded-data region), slab
bump alloc 0x10000 -> 0x702000, unresolved ICALLs 0xEDCDC/0xEB2E0(=func_000EB2E4-4)/0xEB214
returning 0. The failing assertion's caller is in the unsymbolized host stack (LR=0).

This VALIDATES the surgical-fix approach: with correct arithmetic the bring-up advances from a
spin to real game code. Next blockers (the deep game-world bring-up, now on a correct base):
symbolize the abort caller; resolve the off-by-4 ICALLs (0xEB2E0->func_000EB2E4 etc.); relocate
slab bump pools off the 0x700000-0x70F000 SPURS area if it proves to matter. The full clean
re-lift remains scheduled as follow-up (also brings load-with-update + prologue fixes natively).

---

# PHASE 1 (NEXT, concrete) — Establish a correct execution base for the game-world init

## Why this is the real first phase (re-framed by the 2026-06-09 lvlx session)

The M1–M4 event-cycle scaffolding above assumes the game-world init *computes correct values* and
only lacks an event source. The 2026-06-09 drilling showed that assumption is **not yet true**: with
`GAMEWORLD_REAL_INIT` on, the init advances through real menu-tree construction (3+ nodes built) but
then a geometry/math routine (`func_00068998`: int→float scale, `sqrt`, sin/cos) produces a **garbage
double `1.404447762e+306`**, which the shared float formatter (`func_000E2594`→`func_000E25F0`→
`func_000E2A3C`) then tries to print digit-by-digit for ~10^16 iterations (effective hang). Guarding
that (clamp |v|≥1e16→0, baseline-safe, 0 baseline hits) drilled one step further and hit an **AV writing
unmapped guest `0xBFFFF380`** inside C++ unwind (`_NLG_Return2`), with a **sign-extended stack pointer
`gpr[1]=0xFFFFFFFFDFFFF730`** (= `(int64_t)(int32_t)0xDFFFF730`).

This session alone added FIVE surgical lifter fixes (subfc 657, stfiwx 422, lvlx 341, lvrx 17, off-by-4
×3) and **each advanced the bring-up a concrete step**. That pattern, plus the two new blockers both
having the *signature of lifter bugs* (garbage float, sign-extended SP) rather than uninitialised RAM,
makes the central question:

> **Is the GAMEWORLD garbage caused by remaining lifter-correctness bugs, or by genuinely
> out-of-order init reading uninitialised memory?**

The answer decides everything: if lifter, keep fixing the base (cheap, baseline-verifiable, the menu
becomes reachable WITHOUT heavy event scaffolding); if init-order, build Phases B–D. **Phase 1 is to
answer that question and act on it.** Do NOT build event scaffolding until the base computes correct
values — it would be scaffolding on garbage.

## Step 1 — Decide the garbage source (one focused experiment, ~½ day)

1a. **Trace the garbage double to birth.** The value enters the formatter at `func_000E2594(fpr[1])`.
   Walk UP one frame at a time (its caller computes `fpr[1]`); at each frame log the fpr inputs +
   the TOC-constant loads (`vm_read64(gpr[2]-0x5Cxx)` are double constants). Stop at the first frame
   where the value is already absurd vs the frame's *inputs* — that frame contains the bug.
   Tool: `ps3_debug_backtrace` + a throttled fprintf probe (reverted-clean pattern from this session).

1b. **Check for load-with-update bugs (prime suspect).** Upstream sp00nznet/ps3recomp fixed
   "load-with-update: base never advances". Grep the recomp for `lfdu`/`lfsu`/`lwzu`/`ldu`/`lhau`
   emission and verify each writes back the updated base register. A broken `lfdu` in the geometry
   loop would read constants from the wrong offset → garbage. This is the single most likely cause.

1c. **Get the exact bit pattern of `1.404447762e+306`** (printf `%a` / hex of the u64). Recognisable
   patterns tell the story: two packed 32-bit floats = a double-vs-float misread; a low-32 guest
   pointer = type confusion; `0xCCCC…`/`0xBAAD…` = uninitialised. (`0x7F8…` finite-large = a real
   computation gone wrong → almost certainly 1b.)

**Exit criterion for Step 1:** a one-line verdict — "garbage = lifter bug X" or "garbage =
uninitialised field Y that init-order-Z should have set."

## Step 2 — Inventory the remaining lifter gaps (parallel, ~½ day)

2a. **`vmx_x265`** (19 sites, lines ~16k/47k) — last remaining `/* TODO */` instruction class.
   Identify (VMX xo=265) and implement, mirroring the lvlx/subfc transform-script workflow
   (`_apply_*.py`), verifying baseline stays 221102 after each.

2b. **Audit SP sign-extension.** Stack lives at `0xD0000000–0xE0000000` — every stack address has the
   high bit set, so any `gpr[1] = (int64_t)(int32_t)(…)` on a stack-pointer update yields
   `0xFFFFFFFF_Dxxxxxxx`. Reads/writes survive (they truncate to `uint32_t`), but any *full-64-bit*
   use of SP (address arithmetic that keeps the high word, or a compare) breaks — a candidate for the
   `0xBFFFF380` AV. Grep SP-update sites; confirm whether the AV's faulting address derives from a
   sign-extended SP. If so, this is a universal fix (drop the sign-extension on SP, or mask to 32-bit).

2c. **Diff against upstream lifter.** List sp00nznet/ps3recomp lifter commits since our base
   (add/subf, prologue/off-by-4, load-with-update are known); confirm each is applied surgically or
   pending. Produce the definitive "remaining lifter fixes" checklist.

## Step 3 — Choose the base strategy and execute (decision)

- **Option A — keep surgical (proven this session).** Apply each remaining lifter fix in-place via
  a `_apply_*.py` transform, baseline-verify 221102 after each, re-run GAMEWORLD to confirm the
  bring-up advances. Pro: incremental, low-risk, every step is a checkpoint. Con: manual, finite list.
- **Option B — finish the re-lift migration** (`relift-migration` branch; builds/links/runs today).
  Brings ALL upstream lifter fixes natively at once. Pro: durable, future-proof. Con: needs the
  layered-divergence debugging that stalled it (func_00032CA8 AV from corrected lifting changing
  global state).
- **Recommendation:** **Option A now, Option B as the endpoint.** Drive Option A until GAMEWORLD's
  init reaches either (i) a clean return, or (ii) a blocker that is provably *not* a lifter bug
  (Step 1's "uninitialised field" verdict). Only THEN does the init-order / event-cycle work
  (Phases B–D above) become the correct next investment.

## Phase 1 exit / definition of done

- **P1-DONE-a:** verdict in hand — the `1.4e306` garbage and the `0xBFFFF380` AV are each classified
  (lifter bug → fixed surgically + baseline-verified; or init-order → documented with the exact
  uninitialised field and which skipped init sets it).
- **P1-DONE-b:** with the classified fixes applied, `GAMEWORLD_REAL_INIT` advances **past** both the
  dtoa point and the `0xBFFFF380` AV to a new, deeper blocker — OR reaches a clean game-world-init
  return (which would unblock the orchestration-thread spawn that Phase A identified, i.e.
  "Initialize/Regist Context/Unlock Thread").
- **P1-DONE-c:** baseline still 221102, all probes reverted/gated, fixes committed under gnome41.

Only after P1 do we know whether the menu needs (a) just a correct base + the already-found gate
(`func_00024DE0`→`func_00024DE4`), or (b) the full event-source scaffolding (Phases B–D). P1 is the
cheap, decisive bet that prevents building event infrastructure on a miscompiled base.

---

# PHASE 1 RESULT (2026-06-16) — base is correct; verdict = option (b), event scaffolding needed

P1 is **answered**. The lifter-correctness pass (64-bit add/subf, subfc, stfiwx, lvlx/lvrx, fcmp NaN,
stfd-as-stfs, vector `rA|0`, ldu/lbzu, off-by-4 stdu) plus two premature-teardown guards
(GCM flip-drain skip in `func_00042E78`, bnusCore-dtor skip in `func_00039E24`) take
`GAMEWORLD_REAL_INIT` to a **clean EXIT=0** (~223k lines, no AV). So the garbage/AVs were lifter bugs,
not init-order — **P1-DONE-a/b/c met**. The base now computes correct values.

**But the menu still does not appear, and Phase-A scanning (GAMEWORLD on) shows every event-cycle
primitive is STILL absent:**
- No `sys_event_queue_create` (syscall 125) anywhere.
- `cellGcmSetVBlankHandler` (`func_000F0C1C`) is called only with **handler = 0** (cleared) — twice.
  Both calls come from `func_0002AF90` (display setup) reached via the **"Terminate Thread"
  `func_00039E24` TEARDOWN**, i.e. the *un*-register, not a real registration. (Registration chain:
  `func_0002AF90 → func_0003C148 → func_0003C0F0/F4 → func_000F0C1C`; `func_0003C0F4` itself also
  calls `func_000F0C1C` — hence two handler=0 calls.)
- No `cellSpursAddWorkload`.
- `func_0003A4D4` (per-frame menu UI handler) runs **once** with a null scene (`r3=0`).

**Why `r3=0`:** `func_0003A4D0`/`A4D4`'s `r3` = `func_0003AAC8`'s arg. `func_0003AAC8` is called from
`_start` (`func_0003B328` @0x3B4A8) with `r3 = r28 = r31` = the **top-level SPURS/process context**,
not a menu scene. The active menu scene is created later, in the game's **main flow** — which is only
entered after the SPURS init dispatch loop (`loc_0003AE74`, states 2→21) completes and the game would
normally spawn its menu/consumer thread and register the live handler. Our partial SPURS/SPU emulation
runs the init dispatch loop to completion and the process then tears down (the "Terminate Thread" is
engine cleanup that runs because nothing keeps the game alive), so the main flow is never entered.

**Verdict: the menu needs option (b) — build/force the event cycle.** A correct base alone is not
enough; the registration + scene + queue genuinely live in code the partial emulation doesn't reach.
The decisive next sub-task (Phase C "force-run") is to identify the game's **display/menu setup**
function that registers a *live* vblank handler + creates the menu scene (distinct from the teardown
`func_0002AF90` we currently reach), and either fix the gate that skips it or invoke it directly after
init, then let the existing 30 fps `cellGcmTickVBlank` drive it. `g_vblank_handler_opd` capture
(`func_000F0C1C`, committed) is already in place to receive a real handler once that path runs.

---

# Phase C — vblank-registration wall cleared; orchestration threads now spawn (2026-07-31)

After the faithful SPU-interpreter rewrite (S2, commit 8378ea2), re-running with
`GAMEWORLD_REAL_INIT` on advanced the frontier substantially. Two findings:

**1. The live vblank-registration chain now runs and hits a real handshake wait.** With the
faithful interpreter, `func_0003AAC8 → func_00027044 (un-stubbed live vblank registration) →
func_0002B6D0 → func_00027E50 → func_00027D0C` reaches `func_000CCD3C`, which **spin-yields
waiting for `[0x27F868]` to become non-zero** (backtrace via `GCM_SPIN_DETECT`). `func_000CCD3C`
is a yield-poll: `r31 = r3 = 0x27F83C` (a fixed global SPURS descriptor = `0x280000-0x7C4`);
it loops `while ([r31+0x2C] == 0) { syscall 0x8D; }` — i.e. waits for field `+0x2C` (`[0x27F868]`),
a "ready" flag, which no reached code sets.

**2. Forcing `[0x27F868]=1` spawns the real orchestration threads.** Writing the flag at startup
(gated `EVENTCYCLE_PROBE` in `main.cpp`, enable with `GAMEWORLD_REAL_INIT`) advances the
registration past that wait; the existing `[FORCE-SPAWN]` path (thread-mgr `0x8003E0`) then creates
**"Initialize Thread"** (code `0x31364` → body `func_00031368`) and **"Regist Context Thread"**
(code `0x313AC` → body `func_000313B0`) — **two of the three Phase-A orchestration threads** that
were the whole blocker. Thread count jumps 7 → 21; audio middleware threads (`cri_adxm_*`) and the
SDU workers spawn too. This is the closest the port has come to the live event cycle.

**Current blocker (new frontier):** after spawning, one thread busy-spins at ~100% of a core, but
**not** through `vm_read8/16/32/64` (the `GCM_SPIN_DETECT` counters never trip) — so it is a
host-side wait inside a runtime import, not a lifted poll. The orchestration-thread bodies
themselves are short: `func_00031368` and `func_000313B0` each read the globals `[0x27F880]`/
`[0x27F884]` (+`[0x254C48]`) and call `sys` imports `func_000F151C`/`14BC`/`15FC`/`20DC` then exit.
The spinner is inside one of those imports (a lock/cond/join wait). Also: the vblank handler is
still only ever **cleared** (`cellGcmSetVBlankHandler(handler=0)`) — the *live* handler registration
is presumably gated behind the orchestration threads completing, which the spin blocks.

**Next steps:**
1. Identify the spinning import — instrument `func_000F151C`/`func_000F14BC`/`func_000F15FC`
   (and `func_000F20DC`) with entry/exit logging + which `lv2_syscall` they hit; find the host-side
   wait that never completes (likely a lwcond/lwmutex/event-queue receive with no producer).
2. Provide the producer (Phase B event source) or satisfy the wait so "Regist Context Thread"
   completes and registers a **non-zero** vblank handler (watch `[GCM-VBLANK] handler_opd != 0` +
   `g_vblank_handler_opd`).
3. Then wire the 30 fps `cellGcmTickVBlank` to invoke it; iterate toward M2–M4.

Repro: set `#define GAMEWORLD_REAL_INIT 1` (ppu_recomp.cpp) + `#define EVENTCYCLE_PROBE 1`
(main.cpp, or `-DEVENTCYCLE_PROBE`) + `#define GCM_SPIN_DETECT 1` (runtime_glue.cpp) to see the
backtraces. All OFF by default; baseline stays clean (EXIT=0, ~2.5k lines).

## Phase C — the spinner IDed: vblank-sync wait on a NULL object; func_000F0C1C misidentified

Ran down the post-spawn spinner (added `SPIN-SYSCALL`/`SPIN-ICALL` detectors in `ps3_indirect_call`
+ `lv2_syscall`, gated `GCM_SPIN_DETECT`). Of the 21 spawned threads, **20 exit; THREAD 17 = the
"Terminate Thread" (`func_00039E20`/`E24`) spins**. Exact stack:

```
func_00039E24 -> func_00026854 -> func_00026858 -> func_0002AF8C -> func_0002AF90 (display setup)
             -> func_0003C148 -> lv2_syscall(sysnum=141, r3=0x1E)   [spin: syscall-yield loop]
```

`func_0003C148` is the **vblank-sync wait**: after `func_0003C0F0` (gets an object) + `func_000F0C1C`,
it loops `while ([obj+0x8] != r31) { syscall 141; }` (r31 = target = 1). It reads `[obj+0x8]` via
`vm_read32` each iteration but yields, so it never reaches the 80M `vm_read32` threshold — only the
syscall detector (2M) caught it.

**Root: `func_0003C0F0`→`func_0003C0F4`→`func_000F0C1C` returns obj = NULL.** With obj=0 the loop
polls low-memory `[0x8]` (garbage bump-pointer from the earlier table build) which never equals 1
(`[C148-VSYNC] obj=0x00000000 [obj+8]=0x0027F888 target=1`). Satisfying the wait (write `[obj+8]=r31`,
gated probe in `func_0003C148`) lets the Terminate Thread **finish cleanly** ("all game threads
finished") but produces **no** live handler and **no** menu — proof this whole chain is teardown
running on a null object graph.

**KEY CORRECTION — `func_000F0C1C` is NOT `cellGcmSetVBlankHandler`.** The earlier session
(commit d44fc12) identified it as the vblank-handler setter and stubbed it to `gpr[3]=0`, reading the
`handler_opd=0 [cleared]` log as "handler unregistered". But `func_0003C0F4` **uses `func_000F0C1C`'s
return value as an allocated object pointer** (`r28 = r3`; later `[r28+0x0] = ...`; returns `r28`).
So `func_000F0C1C` is really an **object allocator / getter** in the display-setup path; stubbing it
to 0 is exactly why the vblank object graph is null. The `g_vblank_handler_opd` capture is therefore
capturing the wrong thing.

**Next layer (deeper object-graph bring-up):**
1. Identify `func_000F0C1C`'s real import (NID) — it returns an allocated display/vblank object, not
   a CELL_OK. Same for `func_000F0B1C` (called right after in `func_0003C0F4`, writes `[obj+0]`).
2. Make it return a real object (allocate a small struct; populate `[obj+0x8]` as the vblank counter).
   Then `func_0003C148`'s wait becomes real: drive `[obj+0x8]` from the 30 fps `cellGcmTickVBlank`.
3. Re-check whether "Regist Context Thread" (`func_000313B0`) then does real work — its imports
   `func_000F14BC/151C/15FC/20DC` are also one-line `gpr[3]=0` stubs, so it currently no-ops+exits;
   they likely need real implementations too (the orchestration threads spawn but are hollow).

Gated artifacts kept (all OFF by default): `[0x27F868]` force (`EVENTCYCLE_PROBE`, main.cpp);
`func_0003C148` vsync-wait satisfy (`GAMEWORLD_REAL_INIT`); `SPIN-SYSCALL`/`SPIN-ICALL` detectors
(`GCM_SPIN_DETECT`). Baseline verified clean (EXIT=0, ~2.5k lines).
