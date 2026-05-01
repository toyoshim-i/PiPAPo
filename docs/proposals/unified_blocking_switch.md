# Unified Blocking-Switch Design

**Status:** proposed; ARM switch semantics still need design and proof.  This
document supersedes the failed "raise PendSV above SVC" activation path from
`arm_kernel_stack_change.md`, but it must not reintroduce that path under a
different name.

**Target arches:** all (arm_m, riscv, m68k, ia16, xtensa).
**Goal:** one design — shared C control flow, one tiny per-arch primitive — for
"a syscall blocks, the kernel switches to another runnable process from inside
the trap handler, and resumes the blocked one when its wake condition is met."

## 1. Why this proposal exists

Today, every arch implements the same idea three different ways with three
different names:

| Arch   | Cooperative-yield primitive (kernel→kernel) | Trap-return switch on flag |
|--------|---------------------------------------------|----------------------------|
| arm_m  | `arch_sched_switch` → pend PendSV (no flag) | (nothing — uses PendSV)    |
| riscv  | `arch_yield` sets `switch_pending`          | `riscv_do_switch(sp)`      |
| m68k   | `trap #1` and/or `switch_pending`           | inline in `trap.S`         |
| ia16   | `i16_sched_yield()` direct call             | inline in `trap.S`         |
| xtensa | direct call from `arch_sched_switch`        | inline in `xtensa_common.c`|

The shared globals `switch_pending`, `svc_restart[]`, `svc_saved_a0[]`,
`svc_set_restart()` already live in `kernel/core/syscall/`.  Four out of five
arches funnel their cooperative yield through `switch_pending`.  ARM is the
outlier: it uses PendSV (an ARM-specific exception) and ignores
`switch_pending`.  And the names — `svc_*` — are ARM-instruction-flavored
across what is supposed to be shared code.

This is not "every arch needs its own design."  This is one design that grew
five names because it was never refactored.  This proposal does the refactor.

## 2. The unified contract

Three pieces, top to bottom:

### 2.1 Cooperative yield from kernel context (`arch_sched_switch`)

When kernel code (a blocking syscall, a tracer stop, etc.) needs to give up
the CPU until something wakes it:

```c
mod_core.sched_switch();   /* common entry; arch_sched_switch under the hood */
```

`arch_sched_switch` does, on every arch, exactly:

1. Arrange for a real context switch at the point where it is called.
2. Return to its caller only after the same process has been switched away and
   later resumed.

Some architectures implement this by setting `switch_pending` and forcing a
trap-return switch.  Others call a direct switch primitive from kernel context.
The implementation detail is arch-specific; the semantic contract is not.

The point: the *caller* never sees a difference.  After `sched_switch()`
returns, the proc has been off-CPU for some duration and is now back on,
on its own kernel stack, with the same locals it had before.

### 2.2 Trap-return switch (`arch_ctx_switch`)

For trap-return based arches, the trap-return path ends with:

```text
if (switch_pending) {
    switch_pending = 0;
    next_sp = arch_ctx_switch(prev_pcb, sched_next());
    /* fall through to "restore from next_sp's trap frame, return from trap" */
}
```

`arch_ctx_switch(prev, next)` is the irreducible per-arch primitive.  ARM may
call it directly from `arch_sched_switch()` while already inside SVC; other
arches may call it from their trap-return path.  Its contract:

- **Input:** outgoing PCB (whose mid-trap state must be parked) and incoming
  PCB (whose previously-parked state must be resumed; or, if first-run,
  whose freshly-built initial frame must be entered).
- **Action:** save outgoing's callee-saved registers + kernel SP into
  `prev->kernel_sp`'s stack and `prev->sp`; load `next->sp` and resume on
  `next->kernel_sp`.
- **Output:** resumes the incoming saved continuation.  On trap-return based
  arches this may look like returning a trap-frame pointer to caller asm; on
  ARM it may instead return from a restored kernel continuation.

Implementations are ~10-30 lines of asm per arch.  Frame layouts differ;
the contract doesn't.  Today's `riscv_do_switch` is exactly this.  m68k,
ia16, xtensa each have inline equivalents.  ARM grows one (it currently has
only PendSV, which is an async-preemption mechanism and cannot safely be used
as the synchronous mid-SVC yield path).

### 2.3 Restartable syscalls (`syscall_set_restart`)

When a syscall is deliberately written as a replayable operation, and no
externally visible side effect has happened yet, it may wait by asking the trap
return path to re-execute the syscall later:

```c
mod_core.syscall_set_restart();   /* renamed from svc_set_restart */
current->state = PROC_BLOCKED;
mod_core.sched_switch();          /* §2.1 — arch_ctx_switch under the hood */
return -EAGAIN;                   /* placeholder; trap-return rewinds PC */
```

`syscall_set_restart()` sets `syscall_restart[core] = 1` and stashes the
original a0/d0/x10/etc. into `syscall_saved_arg0[core]`.  Every arch's
trap-return path checks `syscall_restart[core]` *before* exception-return:
if set, it rewinds the trap return PC by the size of the syscall
instruction (svc 0 = 2B, ecall = 4B, trap #0 = 2B, int 0x80 = 2B, etc.) and
restores arg0.  When the proc next runs, its syscall re-executes with
original arguments.

This is Linux's `ERESTARTSYS`, named by what it is rather than by the ARM
instruction.  Mechanism is unchanged from today's `svc_restart`; only the name
changes.  It is not the foundation for general blocking syscalls: any syscall
with multiple blocking points, partially completed work, or irreversible side
effects must use the synchronous switch contract in §2.1 instead.

## 3. Naming cleanup

ARM-flavored names that actually live in shared code:

| Today                | Renamed                  |
|----------------------|--------------------------|
| `svc_restart[]`      | `syscall_restart[]`      |
| `svc_saved_a0[]`     | `syscall_saved_arg0[]`   |
| `svc_set_restart()`  | `syscall_set_restart()`  |
| `svc_exc_return[]`   | (stays — ARM-only, stays in arch_/arm_m_) |
| `svc_saved_msp[]`    | (stays — ARM-only)        |
| `exec_pending[]`     | (stays — semantics neutral) |

`mod_core.svc_set_restart` → `mod_core.syscall_set_restart`.  Comments in
shared code using "SVC" to mean "the syscall trap" → "syscall trap."

The rename is mechanical and bisectable.  It's a separate phase so the
behavior change phases stay reviewable.

## 4. ARM's piece (the largest delta)

Per-process kernel stack (`docs/proposals/arm_kernel_stack_change.md`
Phases 1-3) is the prerequisite and is mostly landed in the current unpushed
series.  The failed PendSV-priority activation is not part of this proposal.

What this proposal adds on ARM:

1. `arm_ctx_switch(prev, next)` — new assembly primitive for synchronous
   kernel-context switching.  It saves the current kernel continuation on
   `prev->kernel_sp`, saves the associated PSP/trap-frame state in `prev->sp`,
   chooses or accepts `next`, restores `next->kernel_sp` and `next->sp`, and
   resumes the incoming continuation.
2. `arch_sched_switch` from an SVC/syscall body calls that primitive directly.
   It must not merely set `switch_pending` and wait for the SVC tail, because
   internal blocking loops do not return to the SVC tail until after
   `sched_switch()` returns.
3. `SVC_Handler` keeps its existing post-dispatch handling for `exec_pending`,
   restartable syscalls, signal delivery, and final exception return.  It may
   share restore helpers with `arm_ctx_switch`, but it is not the place where
   mid-syscall blocking is first consumed.
4. `SysTick_Handler`/PendSV can remain the asynchronous user-preemption path
   while the synchronous primitive is developed.  PendSV stays lowest priority
   and must not preempt SVC.
5. Only after the synchronous path is proven should ARM decide whether PendSV
   is still useful for timer preemption or should be folded into the same
   context-switch helper.

Net effect: ARM joins the semantic pattern the other arches already expose:
`sched_switch()` is a real yield point even from inside a syscall body.

## 5. Other arches' deltas

Mostly cosmetic — names align, bodies don't move.

- **riscv**: `riscv_do_switch` → `riscv_ctx_switch` (rename only).  Caller
  in `trap.S` updated.
- **m68k**: extract the inline switch from `trap.S` into a tiny
  `m68k_ctx_switch` (callee-saved save/restore around `sched_next`); call
  site stays at the same point.  Same for `switch.S`.
- **ia16**: `i16_sched_yield` → `i16_ctx_switch` (rename + slight signature
  alignment).
- **xtensa**: extract from `xtensa_common.c` into `xtensa_ctx_switch`.
  Same body.

After this lands, every arch directory has exactly one symbol named
`<arch>_ctx_switch` doing exactly the contract in §2.2.  Any future arch
port has a single named hook to fill in.

## 6. Effect on blocking-syscall code (tty, pipe, poll, time, waitpid…)

Today's pattern works on riscv/m68k/ia16/xtensa and is broken on arm_m.
After this proposal, arm_m matches:

```c
while (!data_ready(t)) {
    if (signal_pending(current)) return -EINTR;
    current->wait_channel = t;
    current->state = PROC_BLOCKED;
    mod_core.sched_switch();      /* really yields, on every arch */
    /* on wake, loop and re-check */
}
```

`syscall_set_restart` is *not* required for this in-loop pattern — the
loop re-checks the condition itself.  It is required for the
"return-and-replay" pattern used by `sys_poll` (timeout management) and
`sys_nanosleep` (deadline management) where the syscall body wants the
trap to re-enter cleanly with original args.  Both patterns coexist and
both work uniformly across arches.

The post-`5720940` work stands.  No regression of that flow.

## 7. Migration plan

Bisectable phases.  Each is a separate commit.  All on-target tests
(qemu_arm, qemu_m68k, qemu_rv32, pcxt) green at every phase.

| #  | Change                                                          | Risk |
|----|-----------------------------------------------------------------|------|
| 1  | Rename `svc_restart`/`svc_saved_a0`/`svc_set_restart` → `syscall_*`.  Update mod_core entry.  No semantic change. | low (mechanical) |
| 2  | ARM: add a synchronous `arm_ctx_switch` and make `arch_sched_switch()` from SVC call it directly.  PendSV remains low-priority for async preemption. | high |
| 3  | Add/enable a blocking-yield test that proves a syscall can suspend in an internal loop and another process runs before the syscall returns. | medium |
| 4  | Hardware gate: fix and verify booted-shell keyboard input on pico1calc-class targets. | high |
| 5  | Audit tty/pipe/poll/time/waitpid: confirm internal-loop vs restart patterns are used intentionally; remove any pico1calc-regression-era workarounds in `wait.h`. | medium |
| 6  | riscv: rename `riscv_do_switch` → `riscv_ctx_switch`.  Update callers. | low (rename) |
| 7  | m68k: extract inline switch → `m68k_ctx_switch`.                | low |
| 8  | ia16: rename `i16_sched_yield` → `i16_ctx_switch`.               | low |
| 9  | xtensa: extract inline switch → `xtensa_ctx_switch`.             | low |
| 10 | Document the contract in `docs/getting_started/` (port guide for new arches). | trivial |

Phases 1, 6-9 are pure rename/extract and should not be mixed with the ARM
behavior change unless the branch is first made bisect-safe.  Phases 2-4 are
the load-bearing ARM work.  Phase 5 is cleanup after the behavior is proven.

After Phase 4 lands, ARM has a synchronous cooperative-switch path that does
not depend on PendSV preempting SVC.  After Phase 9, every arch has the same
named contract, though the ARM implementation may remain structurally
different because Cortex-M has PSP/MSP and exception-return constraints.

## 8. Risks / open questions

- **ARM `arch_sched_switch` from handler mode.**  When called from inside an
  SVC body, it must perform the switch before returning to the syscall code.
  Setting `switch_pending` and waiting for the SVC tail is insufficient for
  internal blocking loops and would recreate the pico1calc deadlock.
- **Known failed path.**  PendSV priority `0x40` preempting SVC priority
  `0x80` reproducibly crashes qemu_arm during `runtests` startup.  Keep PendSV
  low-priority unless this crash is separately root-caused and fixed.
- **Booted-shell keyboard regression.**  Current tip can pass qemu tests while
  accepting no keyboard input at a booted shell.  Hardware or target-level
  input verification is mandatory before claiming completion.
- **Double-trap depth on ARM.**  With `cpsid i` masking IRQs in the SVC
  body (committed as `f8bbff8`), SysTick can't fire mid-SVC.  Good — no
  nested handler mode, kernel stack stays bounded.  But it means a
  long-running SVC body delays time-slice end; that's a fairness concern,
  not correctness.  Worth quantifying once the new path lands.
- **xtensa windowed save.**  `xtensa_ctx_switch`'s save/restore needs to
  flush register windows (Xtensa-specific).  Existing code already does
  this; extraction is just code motion.
- **m68k SSP/USP swap.**  `m68k_ctx_switch` runs in supervisor mode; SSP
  is the kernel stack, USP is user.  PCB stores SSP in the
  per-proc kernel-stack region.  Existing code already correct; rename
  only.
- **MPU reprogram timing.**  On arm_m and m33, MPU regions must be
  reloaded on switch.  Today this happens in PendSV (`switch.S`); it
  moves into `arm_ctx_switch`.  Same code, different call site.

## 9. Out of scope

- Changing `switch_pending` to per-core (it already is, via the existing
  arrays).  If multi-core ever needs different semantics, a separate
  proposal.
- New scheduling policies.  This proposal preserves whatever `sched_next()`
  does today.
- User-visible syscall ABI.  All changes are kernel-internal.

## 10. References

- `docs/proposals/arm_kernel_stack_change.md` — Phase 1-3 prerequisite.
- `src/arch/riscv/kernel/core/riscv_common.c:198` — reference impl of
  `arch_ctx_switch`.
- `src/kernel/core/syscall/syscall.h` — current shared globals (rename
  target).
- `src/kernel/common/mod/mod_core.h` — mod_core entry table (rename
  target).
- `docs/notes/arm_phase4_status.md` — failed PendSV-priority-inversion
  attempt that motivated the unified design.
