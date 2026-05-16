# Proposal: ARM Trap-Frame Switch (Retire PendSV-Centric Scheduling)

## Summary

ARM Cortex-M is the only PPAP target whose kernel-mode scheduling does not
look like every other arch.  m68k, RISC-V, and Xtensa all use a single
shape: a syscall or timer trap pushes the full trap frame onto the
per-process kernel stack, the handler runs (and may switch by swapping
trap-frame pointers between PCBs), and trap exit pops whichever frame is
on top.  ARM instead splits the work between an `SVC_Handler` that runs on
the per-process MSP slot and a `PendSV_Handler` that tail-chains after the
SVC return, with a third path (`arm_kernel_sched_switch`) for synchronous
kernel-mode continuation blocking.  Three paths, run-time-gated by
`syscall_restart` and IPSR, with their own continuation flag
(`pcb_t.kernel_context`) and a fragile `bx lr`-to-C-address restore that
breaks when two such blockings chain through one user-mode trip.

This proposal collapses ARM's three paths into the same single trap-frame
swap that every other arch uses.  It is the long-term goal that motivates
the [`no_restart`](no_restart.md) cleanup — once ARM follows the common
model, `syscall_restart` has no reason to exist, and steps 3–4 of that
proposal (waitpid, ptrace) unblock immediately.

## Motivation

- **Three scheduling paths instead of one.**  `arch_sched_switch()` on ARM
  branches on IPSR and `arm_can_kernel_sched_switch()`, which itself
  branches on `current->state` and `syscall_restart`.  Every reader of
  `sched_switch` on ARM has to keep this decision tree in mind.
- **A latent bug exposed by the `no_restart` migration.**  The
  `arm_kernel_sched_switch` restore ends with `bx lr` to a saved C return
  address, *not* an `EXC_RETURN`.  The CPU stays in Handler mode without
  unwinding the PendSV exception activation.  A single use (e.g. vfork)
  tolerates this; chaining a second use through one user-mode round-trip
  (e.g. vfork → execve → waitpid against a Thread-mode kernel child)
  accumulates broken active-exception bookkeeping and the second blocked
  syscall never resumes correctly.  This is the concrete blocker for
  `no_restart` steps 3–4.
- **`kernel_context` flag is a bandage** over the IPSR / mode mismatch.
  Other archs don't need it because they don't have two different
  blocking shapes.
- **Trap-IP rewind machinery** in [`trap.S`](../../src/arch/arm_m/kernel/core/trap.S)
  exists solely to support `syscall_restart`.  It carries the cost of
  every SVC return path, even for syscalls that never restart.
- **Mental-model coherence.**  PPAP already documents that "every arch
  uses a single per-process kernel stack with a trap-frame swap."  ARM
  is the one exception, and removing the exception simplifies the kernel
  contract globally.

## Current state

| Path | Triggered by | Restore mechanism |
|------|--------------|--------------------|
| `arm_kernel_sched_switch` | `sched_switch()` in Handler mode with `PROC_BLOCKED && !syscall_restart` | Direct save of MSP+PSP into PCB; PendSV later does `bx lr` to a saved C addr (does not `EXC_RETURN`) |
| PendSV tail-chain after SVC | `arch_yield()` + SVC handler return when `syscall_restart` was set | PC rewound to SVC-2 by trap.S, then PendSV picks next process |
| PendSV from Thread mode | `arch_yield()` from user / Thread-mode kernel | PendSV preempts at the next instruction boundary |

These three coexist because no single shape handles every blocking
scenario cleanly under the current design.  Collapsing them requires
restructuring the SVC entry and exit so that the full trap frame lives
on the kernel stack, identifiable by `pcb_t.sp`.

## Target state

One shape, matching m68k / RISC-V / Xtensa:

```
SVC entry   → push HW frame (PSP) + SW frame (r4-r11) — the full trap frame
            → record &frame in current->sp
            → run syscall body on the per-process kernel stack
syscall body
   may call sched_switch() → save current->sp = MSP, load next->sp,
                              MSR msp = next->sp, return to next's body
SVC exit    → pop SW frame off MSP, then EXC_RETURN pops the HW frame
              off PSP and returns to whichever process's user state is
              on top.
```

Properties:

- **No `kernel_context` flag.**  The trap frame *is* the saved
  continuation; there is nothing else to track.
- **No `syscall_restart` rewind.**  Blocking syscalls loop in C around
  `sched_switch()` and complete by returning a real value through the
  same trap-exit path.  Restart becomes dead code that the `no_restart`
  proposal then removes.
- **PendSV is only used for SysTick preemption** in Phase A — same
  handler, narrower scope.  Phase B retires it entirely.
- **Cross-arch consistency.**  ARM's diagram in
  [`context_switch.md`](../kernel/context_switch.md) collapses to the
  same row as the m68k / riscv / xtensa entries.

## Phase A — Trap-frame swap for SVC blocking

The core of this proposal.  Lands ARM's SVC handling on the common
model and unblocks `no_restart` Phases 1–3.

### Files touched

- `src/arch/arm_m/kernel/core/trap.S` — SVC entry/exit restructuring.
- `src/arch/arm_m/kernel/core/switch.S` — replace `arm_kernel_sched_switch`
  with a small trap-frame-swap helper; keep `PendSV_Handler` for SysTick
  preemption.
- `src/arch/arm_m/kernel/core/arch.h` — new `arch_sched_switch()` shape.
- `src/arch/arm_m/kernel/core/arm_m_common.c` — drop
  `arm_can_kernel_sched_switch`, `arm_mark_kernel_context`,
  `arm_take_kernel_context`.
- `src/kernel/common/core/proc_info.h` — drop `pcb_t.kernel_context`.
- Per-target `proc_setup_kernel_stack` paths — verify the initial frame
  shape matches the new trap-frame layout (ARM-only change; other archs
  unaffected).

### SVC entry

At SVC entry the CPU has already pushed the HW exception frame to PSP
(8 words: r0-r3, r12, lr, pc, xpsr).  The handler:

```asm
push  {r4-r11, lr}            @ SW frame on MSP — lr = EXC_RETURN value
mrs   r0, psp                 @ r0 = base of HW frame (user PSP)
str   r0, [<current>, #PSP_OFF]   @ remember PSP separately
mov   r1, sp                  @ r1 = base of SW frame on MSP
str   r1, [<current>, #SP_OFF]    @ current->sp = trap-frame top
```

`current->sp` now points to the SW frame on the kernel stack; PSP holds
the HW frame.  The two are linked by the saved `EXC_RETURN` in the SW
frame.

### Synchronous switch

```c
void arch_sched_switch(void) {
  pcb_t *next = sched_next();
  if (next == current) return;
  current_core[core_id()] = next;
  current->sp = read_msp();
  write_msp(next->sp);
}
```

In assembly this is a few MRS/MSR instructions plus the C call to
`sched_next`.  No `cpsid` games, no MPU-restore-now (deferred to trap
exit), no FPU lazy-state dance specific to this path — those concerns
live in PendSV for preemption and don't apply to a cooperative swap.

The incoming process's MSP points at *its* SW frame.  When the SVC
handler eventually pops `{r4-r11, lr}` and does `bx lr`, the EXC_RETURN
value unwinds the incoming process's HW frame from its PSP, transitioning
to Thread mode at *its* user PC.  Exactly the same shape m68k and riscv
already use.

### SVC exit

```asm
pop   {r4-r11, lr}            @ SW frame off MSP — lr = EXC_RETURN
bx    lr                      @ HW frame pops off PSP, return to user
```

No restart-PC-rewind block.  No exec_pending check (sys_execve does its
own full-context-restore by writing `current->sp` directly).  No
syscall_restart consultation.

### Removed symbols

- `arm_kernel_sched_switch` (asm function)
- `arm_can_kernel_sched_switch` (C function)
- `arm_mark_kernel_context`, `arm_take_kernel_context` (C functions)
- `pcb_t.kernel_context` (field)
- `syscall_restart[]` consult / write in `trap.S`
- `syscall_saved_arg0[]` consult / write in `trap.S`
- The PC-rewind branch in `trap.S`

### Validation gates

- Existing qemu_arm tests must remain green (24/24).
- Existing pico1 / pico2 hardware boot test must remain green.
- New ktest case: chain two `arm_kernel_sched_switch`-style blockings
  through a single user-mode round-trip, which used to expose the
  latent bug.  This is essentially what `test_cpm` does today; it
  should pass post-migration even without `klogf` masking.
- Stack-usage check (`KSTACK_USAGE_TRACK`) — verify the new frame
  layout doesn't bloat per-process kernel stacks beyond their slot.

## Phase B — Retire PendSV entirely (deferred)

Optional follow-up after Phase A lands.  Not required for `no_restart`.

- SysTick handler sets `switch_pending` (the cross-arch flag) instead
  of pending PendSV.
- SysTick trap exit checks `switch_pending` and performs the same
  trap-frame swap inline.
- `PendSV_Handler` deleted.  ARM's "Async preemption" row in
  [`context_switch.md`](../kernel/context_switch.md) collapses to
  "SysTick checks `switch_pending`," identical to m68k / riscv / xtensa.

Scope: ~50–100 lines.  Independent of `no_restart`.  Worth its own
proposal at the time, but the same `arm_trap_frame_switch.md` design
applies.

## Migration plan

1. **Add the trap-frame-swap path side by side.**  Introduce the new
   SVC entry / swap / exit code under a build flag or alongside the
   existing path.  Verify it compiles and links.
2. **Switch one cooperative-blocking syscall to the new path.**  TTY
   read is a good first cut — it already exercises continuation
   blocking and has direct test coverage.  Run qemu_arm tests; expect
   no regressions.
3. **Switch the remaining blockings.**  Pipe, sleep, poll, then waitpid
   and ptrace as part of `no_restart` steps 3–4.
4. **Delete the old path.**  Remove `arm_kernel_sched_switch`, the
   `kernel_context` flag, the restart-rewind block, and the related
   symbols listed above.
5. **`no_restart` Phase 3** can then proceed: delete the globals,
   `pcb_t.syscall_needs_restart`, the `mod_core.syscall_set_restart`
   slot, and the per-arch wrappers.

## Risk notes

- **Stack discipline.**  Other archs already keep the trap frame on the
  per-process kernel stack and pay the same byte cost (HW frame on user
  stack, SW frame on kernel stack).  ARM's HW frame is on PSP, SW frame
  moves to MSP — same total, just re-arranged.  Per-process kernel-stack
  sizing already accommodates this on TTY / pipe blocking paths.
- **Interrupt nesting with SysTick.**  Until Phase B, SysTick can still
  pend PendSV.  PendSV preempting an in-progress SVC swap is fine —
  PendSV saves on the *incoming* process's MSP via the existing
  full-save-and-restore path, then exits via EXC_RETURN, dropping back
  into the SVC body running on the new MSP slot.  The two paths don't
  share state.
- **`sys_execve` context restore.**  Currently `exec_pending` triggers
  a special trap-return path that does a full PendSV-like restore.
  Under the new model, sys_execve writes the new frame directly to
  `current->sp` (and uses the synchronous swap mechanism to "switch
  to" the new process state).  Same end result, no special trap-return
  flag needed.
- **Vfork parent unblock.**  Today the parent's vfork uses
  `arm_kernel_sched_switch`; after the migration it uses the new
  synchronous swap.  Behaviour is identical because the wake-up path
  (direct `state = PROC_RUNNABLE` + later schedule pick) is unchanged.
- **Hardware breakpoint / MPU switching.**  PendSV currently calls
  `mpu_switch` and `trace_arm_hwbp_on_switch` between processes.  The
  new synchronous swap path needs the same calls in the same order.
  Easy to miss; the new `arch_sched_switch` body should mirror the
  PendSV body for these side-tasks until PendSV is retired entirely.

## Out of scope

- **Userland subsystem migration.**  CP/M, S-OS, Human68k are slated to
  move into userland separately.  Once they do, the Thread-mode-kernel
  exit path that exposed the latent bug stops existing.  That migration
  is its own proposal; this proposal stands on its own ARM-cleanup
  merits regardless.
- **Phase B (PendSV retirement).**  Tracked here as future work; not
  required to land `no_restart`.
- **Other arch changes.**  m68k, RISC-V, Xtensa, ia16 are already on
  the trap-frame-swap model and need no changes.

## Relationship to other proposals

- [`no_restart.md`](no_restart.md) — Phase A of this proposal unblocks
  steps 3–4 (waitpid, ptrace) of `no_restart`, and makes Phase 3
  (delete the globals) tractable.  Without Phase A, `no_restart` can
  ship steps 1–2 (sleep, poll, committed) but stalls there.
- [`hires_timer.md`](hires_timer.md) — independent.  The deadline-driven
  wakeup primitive doesn't care which ARM scheduling shape is in use.
