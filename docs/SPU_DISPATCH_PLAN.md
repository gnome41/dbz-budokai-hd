# Real SPU Mailbox / Dispatch — Design & Roadmap

Goal: replace the **synthetic** SPURS dispatch with a **real** PPU↔SPU mailbox + DMA
mechanism, so the game's SPURS-coupled PPU init advances naturally and the event-driven
main loop (hence the menu) can run.

## Current architecture (what we have)

Two halves that **do not actually talk to each other**:

**SPU side** (`spu_interp.cpp` + `spu_spurs.cpp`): a working SPU interpreter runs the
embedded SPURS kernel ELF (guest 0x10BD00) in one `spu_ctx_t` (`g_spurs_ctx`) on a
Windows thread. Dispatch is **forced synthetically**:
- `LS[0x17C]` patched (`ilh r2,1`) to pass a type check.
- `LS[0x3BC]/[0x3C0]` patched to `lnop` to **bypass the mailbox-wait idle branch**.
- Return-address registers (r86/r70/r89) and the management-area EA (r3) are hand-fed.
- Workload descriptors at 0x70A000 are **synthetic sentinels**, not real ones.
- The kernel never blocks on `rdch SPU_RdInMbox`; it's force-marched through 15 slots.
- Net effect: produces the animated sphere via the EDGE geometry path. NOT real SPURS.

**PPU side** (`recompiled/ppu_recomp.cpp` + `main.cpp`): the game's SPURS init
(`func_000CE77C`), the SPURS state machine (`func_0003AAC8`), and the management-area
chains (0x70A000 / 0x70B000) are **faked / force-advanced**. The PPU **never issues the
real `sys_spu_thread_*` syscalls** — `lv2_syscall` handles **none** of them. So there is
no real mailbox traffic for the SPU side to consume.

The SPU `spu_ctx_t` already has mailbox fields (`inbound_mbox`/`outbound_mbox` +
counts; `rdch CH_SPU_RdInMbox` / `wrch CH_SPU_WrOutMbox`) — but the PPU never writes
`inbound_mbox`, and `rdch` on empty does **not block** (returns stale/zero).

## THE KEY FINDING (entanglement)

**Real SPU dispatch cannot be built in isolation.** Because the PPU side is faked and
does not use the real SPU syscalls, there is nothing to connect a real mailbox to.
Building real dispatch requires **un-faking the PPU SPURS side too** (real
`cellSpursInitialize` → real `sys_spu_thread_group_create/start`, real management-area
layout, real `cellSpursAddWorkload`), which is the *same* project-scale bring-up as the
"object-graph" route — approached from the SPU end. The two are one problem, not two.

Consequence: this is **not** a clean, self-contained "add an SPU emulator" task. It is
effectively building SPURS support comparable to a mature Cell emulator's, on top of a
heavily-faked PPU layer, **and it risks regressing the currently-working sphere render**
(which depends on the synthetic path). Realistic scale: large (weeks), high-risk.

## Incremental roadmap (lowest-risk ordering)

Each step is gated/parallel to the synthetic path so the working build is never broken
until a real path fully replaces it.

- **S0 — Multi-SPU context model + real blocking mailbox (infrastructure).**
  Generalise `spu_ctx_t` to N contexts; make `rdch SPU_RdInMbox` **block** (cooperative
  yield) until a producer sets it; add a thread-safe `spu_write_in_mbox(ctx, val)` and
  `spu_read_out_mbox(ctx)`. No behaviour change yet (synthetic path still drives).

- **S1 — Real LV2 SPU syscalls in `lv2_syscall`.** Implement the group the game's
  cellSpurs uses: `sys_spu_thread_group_create/initialize/start` (153/169/170/173-ish),
  `sys_spu_thread_write_spu_mb` (187), `sys_spu_thread_read_spu_mb`, `sys_spu_image_*`.
  Wire thread-group create to allocate real `spu_ctx_t`s and load the kernel image;
  wire write_spu_mb → `spu_write_in_mbox`. Exercised only once the PPU routes through it.

- **S2 — Decode the SPURS management-area format.** RE exactly what the kernel reads
  from the management area (0x70A000) to pick a workload: the sort table (LS[0x1F5B0]),
  prio table (LS[0x1F5F0]), the `r79`/`r33`/`r36` dispatch math (CLAUDE.md "SPURS kernel
  dispatch"). Produce a **valid** descriptor we can write so the kernel's REAL sort/
  dispatch (with the `lnop` patches REMOVED) finds it. Milestone: kernel dispatches a
  workload via its own logic from a real mailbox signal, no force-march.

- **S3 — Un-fake the PPU SPURS init incrementally.** Replace the faked `func_000CE77C` /
  state-machine force-advances with the real path now that the syscalls + mailbox exist,
  one wait/handshake at a time, verifying each against the real SPU producing the state
  the PPU polls (e.g. the `func_000D9470` poll of [0x10BC28]).

- **S4 — Real `cellSpursAddWorkload`.** Write real descriptors from the (now-reached)
  game flow; the real kernel dispatches them. At this point the SPURS-coupled init
  completes naturally and the event cycle / menu path becomes reachable.

## S2 RESULT (landed) — faithful interpreter + real kernel loop

**The interpreter was systematically misdecoding.** Audited against the
binutils/RPCS3 canonical SPU opcode table, the original `spu_interp.cpp` had a
wrong opcode map plus three core defects: RRR-format destination/RC fields
swapped (so `selb`/`shufb`/`fma`/`fms`/`fnms`/`mpya` wrote the wrong register,
and `selb`↔`shufb` were transposed); `lqd`/`stqd` raw-copied big-endian LS bytes
while the arithmetic ops read `u32` lanes host-native (every value loaded from LS
data arrived byte-swapped); and `shufb` control-byte semantics were wrong.

`spu_interp.cpp` was rewritten from that canonical table (register storage now
documented as raw SPU byte order; element access via `spu_reg_get_w/set_w` etc.
in `spu_interp.h`). Result: **zero UNIMPL opcodes**, and the SPURS kernel now
runs its **real** logic instead of the panic spin the misdecode produced.

Findings from the faithful run (all the old CLAUDE.md dispatch notes — `r79`/
`r33`/`r36`/`selb r42`, the `ilh r2,1` type-check hack, the `LS[0x3BC]/[0x3C0]`
"brhnz→lnop" patches, the EDGE return-register values — were artifacts of the
misdecode and are now **obsolete/unreliable**):

- **Management-area EA contract:** kernel main (LS[0x19C28]) validates arg1 with
  `shlqbyi r4,r3,2; ceqi r2,r4,0x30` — it requires the management-area EA to have
  the form `0x0030xxxx`. The old `0x70A000` EA failed this and parked the kernel
  at the panic loop LS[0x19C98]. Arg1 is passed in the register's **preferred
  doubleword** (r3 bytes 4-7, big-endian), not word 0.
- With EA = `0x300000` the kernel enters its **authentic dispatch/poll loop**:
  DMA-GET the management/control block → `rdch MFC_RdTagStat` → `wrch` outbound
  mailbox (ch 28) + outbound-intr mailbox (ch 30) → `rdch` inbound mailbox
  (ch 29, blocks) → `rdch SigNotify1` (ch 3) → loop; then `stop 0` (yield to PPU)
  when no work is present. **No lnop force-march, no fake type field.**
- The `LS[0x17C]`/`[0x3BC]`/`[0x3C0]` patches are now gated behind
  `SPURS_LEGACY_LS_PATCHES` (default OFF) — under the faithful decode 0x3BC/0x3C0
  are `stqr` stores, not branches.
- The synthetic EDGE dispatch (`spurs_run_workload`, driven by the old fake
  dispatch stops at LS[0x04..0x3C]) no longer fires. **The sphere render is
  unaffected** — `spurs_render_sphere_tick()` runs on the independent
  `render_thread_proc` timer and writes `0xD0180000` directly. No regression.

**Status vs the S2 exit criterion:** the kernel's *own* dispatch logic runs with
no force-march (criterion largely met), but it does not yet *select a workload*
because (a) the management block at `0x300000` is zeroed BSS — the DMA-GET reads
a null control pointer (`ea=0`), and (b) no PPU mailbox signal arrives. Closing
that is S3/S4: populate a real management/instance block and wire the PPU→SPU
mailbox (`sys_spu_thread_write_spu_mb`) so `rdch InMbox` unblocks with a real
command. The hard "is the dispatch math tractable" question is answered **yes** —
it's a normal DMA + mailbox handshake, not the opaque `selb r42` puzzle the
misdecode made it look like.

## Exit criterion for "worth continuing"

After **S2** we'll know if the kernel's real dispatch is tractable to drive (the
CLAUDE.md notes flag the `r79`/`selb r42` math as the hard part). If S2 lands, the rest
is mechanical-but-long. If S2 proves the kernel needs state only the real PPU side can
produce, then S3 (un-faking PPU) must come first and the effort balloons further —
decision point to re-confirm the investment.

## Risk register

- Regressing the sphere/EDGE render (synthetic path is load-bearing). Mitigate: keep
  synthetic path until a real path fully replaces it; gate behind a flag.
- The `r79`/`r33`/`r36` dispatch math may require real workload state we can't yet
  produce (CLAUDE.md already hit this). S2 is the make-or-break.
- Multi-thread mailbox races; needs careful cooperative blocking, not busy-wait.
