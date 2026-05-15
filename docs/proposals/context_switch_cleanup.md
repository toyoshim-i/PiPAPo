# Context Switch Cleanup Follow-Ups

**Status:** mostly complete.  The fixed per-process kernel-stack migration is
done for the supported architectures.  The steady-state model now lives in
[`../kernel/context_switch.md`](../kernel/context_switch.md),
[`../kernel/stack.md`](../kernel/stack.md), and
[`../kernel/memory.md`](../kernel/memory.md).

This proposal remains only as a short tracker for cleanup work that should not
be lost while the stack model settles.

## Completed Direction

PPAP now uses one long-term context-switch contract:

- Every supported architecture has a per-process kernel stack for suspended
  kernel continuations.
- `sched_switch()` performs a real cooperative switch when called from kernel
  code; when the process runs again, it resumes after that call.
- Restartable syscalls remain a separate replay path for syscalls that are
  explicitly safe to re-execute.
- User stacks, subsystem storage, data pages, heap pages, and fixed kernel
  stacks are tracked as distinct resources.
- Non-syscall Xtensa `sched_switch()` callers have been audited.  The remaining
  callers outside `xtensa_syscall_on_kstack()` are bootstrap, idle, or
  exception-cleanup paths rather than resumable process continuations.
- Syscall-exit switching is guarded by the
  `ARCH_EXIT_SWITCH_IN_SYSCALL_EPILOGUE` capability.  Xtensa enables it because
  its syscall body can switch after `sys_exit()` marks the process zombie;
  other architectures switch directly inside `sys_exit()`.
- The userland empty-pipe blocking regression passes on the runnable emulator
  targets as of 2026-05-15: `qemu_arm`, `qemu_m68k`, `qemu_rv32`, and `pcxt`
  with the generated HDD test image.

The common fixed-region kstack helper initializes `pcb_t.kernel_sp` from the
target-reserved `__kstack_region_base` area.  ARM-M, ia16, m68k, RISC-V, and
Xtensa all use fixed kstack slots for process kernel continuations.

## Stable Documentation

The detailed architecture material was moved out of this proposal:

- `docs/kernel/context_switch.md` documents the synchronous
  `sched_switch()` contract, restart-vs-continuation behavior, and per-arch
  switch mechanisms.
- `docs/kernel/stack.md` documents fixed kstack slot geometry, per-arch stack
  pointer roles, canaries, and `KSTACK_USAGE_TRACK`.
- `docs/kernel/memory.md` documents page-pool ownership, `stack_page_id`,
  `user_stack_page`, and why fixed kstacks are not process RSS.

## Remaining Follow-Ups

1. Get reliable `xtensa_cc` hardware test runs for the blocking-continuation
   suite.  The build works, but the flash/monitor path can fail before tests
   start.
2. Use common `KSTACK_USAGE_TRACK` measurements before shrinking stack sizes.
   m68k currently uses 2 KB process slots with a TODO to shrink to 1 KB if the
   high-water margin is safe.
3. Keep the deferred subsystem stack-use work in
   [`kernel_stack_use.md`](kernel_stack_use.md).  Much of that effort may be
   obsoleted by moving subsystem work to userland.

## Constraints

- Do not reintroduce ARM PendSV preempting active SVC execution.  ARM's
  synchronous blocked-syscall path is `arm_kernel_sched_switch()`.
- Keep restart/replay state separate from continuation-blocking state.
- Native interrupt stacks are optional optimization layers.  They must not be
  the only place where a blocked syscall continuation can live.
- Avoid arch/target `#ifdef` policy in shared code.  Prefer named
  architecture capabilities, shared helpers, or arch-local implementations.
