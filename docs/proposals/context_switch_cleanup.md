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

1. Keep the userland empty-pipe blocking test passing on all runnable targets.
   It is the main regression test for continuation blocking without adding
   test-only kernel code.
2. Get reliable `xtensa_cc` hardware test runs for the blocking-continuation
   suite.  The build works, but the flash/monitor path can fail before tests
   start.
3. Keep syscall-epilogue switch behavior behind an architecture capability.
   Xtensa uses this for `sys_exit()` because the syscall body returns to an
   epilogue that switches after seeing `current->state != PROC_RUNNABLE`.
4. Audit non-syscall Xtensa `sched_switch()` callers.  Syscall-path blocking
   runs on the fixed kstack through `xtensa_syscall_on_kstack()`.  Any future
   process-continuation path outside that wrapper must either run on the fixed
   kstack or be documented as bootstrap/idle-only.
5. Use common `KSTACK_USAGE_TRACK` measurements before shrinking stack sizes.
   m68k currently uses 2 KB process slots with a TODO to shrink to 1 KB if the
   high-water margin is safe.
6. Keep the deferred subsystem stack-use work in
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
