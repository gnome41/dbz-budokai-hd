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

## Phase A — Map the divergence (instrumentation)

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
