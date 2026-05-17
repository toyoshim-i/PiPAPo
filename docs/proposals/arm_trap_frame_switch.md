# Proposal: ARM Trap-Frame Switch (Retire PendSV-Centric Scheduling)

> **Status**: Phase A landed (commit `6a7cc31a` — `kernel_sp` slot-top vs
> saved-PSP separation).  Phase B (retire PendSV for SysTick preemption)
> is the remaining open work this document covers.

## Summary

ARM Cortex-M is the only PPAP target whose kernel-mode scheduling does not
look like every other arch.  m68k, RISC-V, and Xtensa all use a single
shape: a syscall or timer trap pushes the full trap frame onto the
per-process kernel stack, the handler runs (and may switch by swapping
trap-frame pointers between PCBs), and trap exit pops whichever frame is
on top.  ARM instead splits the work between an `SVC_Handler` that runs on
the per-process MSP slot and a `PendSV_Handler` that tail-chains after the
SVC return, with a third path (`arm_kernel_sched_switch`) for synchronous
kernel-mode continuation blocking.

This proposal collapses ARM's three paths into the same single trap-frame
swap that every other arch uses.

## Motivation

- **Two scheduling paths instead of one.**  `arch_sched_switch()` on ARM
  branches on IPSR and `arm_can_kernel_sched_switch()`, which checks
  `current->state == PROC_BLOCKED`.  Every reader of `sched_switch` on
  ARM has to keep this decision tree in mind.
- **`kernel_context` flag is a bandage** over the IPSR / mode mismatch.
  Other archs don't need it because they don't have two different
  blocking shapes.
- **Mental-model coherence.**  PPAP already documents that "every arch
  uses a single per-process kernel stack with a trap-frame swap."  ARM
  is the one exception, and removing the exception simplifies the kernel
  contract globally.

## Current state

| Path | Triggered by | Restore mechanism |
|------|--------------|--------------------|
| `arm_kernel_sched_switch` | `sched_switch()` in Handler mode with `PROC_BLOCKED` | Direct save of MSP frame base into PCB; PendSV later does `bx lr` to a saved C addr |
| PendSV from Thread mode | `arch_yield()` from user / Thread-mode kernel | PendSV preempts at the next instruction boundary |

These two coexist because no single shape handles every blocking
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
- **PendSV is only used for SysTick preemption** in Phase A — same
  handler, narrower scope.  Phase B retires it entirely.
- **Cross-arch consistency.**  ARM's diagram in
  [`context_switch.md`](../kernel/context_switch.md) collapses to the
  same row as the m68k / riscv / xtensa entries.

## Phase A — Slot-top / saved-PSP split  **(landed in commit `6a7cc31a`)**

The shipped Phase A is narrower than the original proposal envisioned.
Rather than restructuring SVC entry/exit and retiring
`arm_kernel_sched_switch` outright, the fix kept the existing two-path
shape and just made `pcb_t.kernel_sp` strictly immutable (the slot top)
while moving the saved PSP into the kernel-continuation SW frame:

- `pcb_t.kernel_sp` is planted once by `proc_kstack_init_slot()` and
  never written by save paths.  SVC entry's MSP-into-slot swap and
  PendSV restore both read it as "slot top."
- `pcb_t.sp` carries the saved SW-frame base — on PSP for Thread/PSP
  preempted processes, on MSP for kernel-continuation processes,
  distinguished by `pcb_t.kernel_context`.
- The kernel-continuation SW frame grows from 36 to 40 bytes, the
  extra word holding the saved PSP that previously lived in
  `pcb_t.sp`.

That was enough to fix the kernel_sp ratchet that was eating the
kernel slot one SVC at a time and causing the `test_cpm` HardFault.
`arm_kernel_sched_switch`, the `kernel_context` flag, and the
two-path shape are still in place.

The full SVC-trap-frame restructure described in the original Summary
is therefore folded into Phase B below.

## Phase B — Retire PendSV and `arm_kernel_sched_switch`

The remaining open work.  Replaces both PendSV-for-preemption and
`arm_kernel_sched_switch`-for-cooperative-blocking with the single
trap-frame-swap shape described in the Summary.

### Files touched

- `src/arch/arm_m/kernel/core/trap.S` — SVC entry/exit restructuring.
- `src/arch/arm_m/kernel/core/switch.S` — replace `arm_kernel_sched_switch`
  with a small trap-frame-swap helper.  PendSV handler deleted.
- `src/arch/arm_m/kernel/core/arch.h` — new `arch_sched_switch()` shape.
- `src/arch/arm_m/kernel/core/arm_m_common.c` — drop
  `arm_can_kernel_sched_switch`, `arm_mark_kernel_context`,
  `arm_take_kernel_context`.
- `src/kernel/common/core/proc_info.h` — drop `pcb_t.kernel_context`.
- SysTick handler — set `switch_pending` (cross-arch flag) instead
  of pending PendSV; SysTick trap exit performs the trap-frame swap
  inline if the flag is set.

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
exit), no FPU lazy-state dance specific to this path.

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

No exec_pending check (sys_execve does its own full-context-restore by
writing `current->sp` directly).

### Removed symbols

- `arm_kernel_sched_switch` (asm function)
- `arm_can_kernel_sched_switch` (C function)
- `arm_mark_kernel_context`, `arm_take_kernel_context` (C functions)
- `pcb_t.kernel_context` (field)
- `PendSV_Handler` (asm function)

### Validation gates

- Existing qemu_arm tests must remain green (24/24).
- Existing pico1 / pico2 hardware boot test must remain green.
- Stack-usage check (`KSTACK_USAGE_TRACK`) — verify the new frame
  layout doesn't bloat per-process kernel stacks beyond their slot.

## Migration plan

1. **Add the trap-frame-swap path side by side.**  Introduce the new
   SVC entry / swap / exit code under a build flag or alongside the
   existing path.  Verify it compiles and links.
2. **Switch one cooperative-blocking syscall to the new path.**  TTY
   read is a good first cut — it already exercises continuation
   blocking and has direct test coverage.  Run qemu_arm tests; expect
   no regressions.
3. **Switch the remaining blockings.**  Pipe, sleep, poll, waitpid,
   ptrace.
4. **Convert SysTick preemption.**  SysTick sets `switch_pending` and
   the trap exit handles the swap; remove `PendSV_Handler`.
5. **Delete the old path.**  Remove `arm_kernel_sched_switch`, the
   `kernel_context` flag, and the related symbols listed above.

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
  exit path that exposed the latent kernel_sp bug stops existing.
  That migration is its own proposal.
- **Other arch changes.**  m68k, RISC-V, Xtensa, ia16 are already on
  the trap-frame-swap model and need no changes.

## Relationship to other proposals

- [`hires_timer.md`](hires_timer.md) — independent.  The deadline-driven
  wakeup primitive doesn't care which ARM scheduling shape is in use.
