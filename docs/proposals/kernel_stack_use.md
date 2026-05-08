# Kernel Stack Use Reduction

**Status:** deferred proposal.  Do not execute this work yet unless stack
measurements show an urgent blocker.

## Goal

Reduce kernel-stack use on architectures with small per-process kernel-stack
budgets, especially ARM-M and ia16, by measuring and simplifying deep syscall
paths before shrinking stack sizes.

The main suspected pressure point is the subsystem call path: loaders,
bridges, host shims, VFS translation, and emulator-facing glue can build deep
kernel call chains.  On ARM-M this matters because all ARM targets now use
fixed per-process kernel-stack slots; on ia16 it matters because the mature
1 KB slots leave little room for accidental growth.

## Timing

This is not the best near-term cleanup to execute.

Subsystems are planned to move out of the kernel and into userland runners via
the vCPU interface described in
[`userland_subsys.md`](userland_subsys.md).  If that architectural shift
lands, much of the current kernel-side subsystem call path will disappear or
become much shallower.  Stack-reduction work done before that move may become
obsolete, or worse, may optimize code that should be deleted instead.

For now, keep this proposal as a measurement and contingency plan.

## Current State

- ARM-M targets use fixed per-process kernel-stack slots.
- ARM-M user-process slots are currently 2 KB.
- ia16 uses fixed 1 KB user-process kernel-stack slots.
- `KSTACK_USAGE_TRACK` can measure fixed-region high-water marks for ia16 and
  ARM-M when enabled at build time.
- `docs/proposals/context_switch_cleanup.md` tracks the broader
  per-process-kernel-stack context-switch model.

## Deferred Work

1. Add an easy build switch, such as `PPAP_KSTACK_USAGE_TRACK=ON`, instead of
   requiring manual `CMAKE_C_FLAGS=-DKSTACK_USAGE_TRACK=1`.
2. Measure high-water marks on:
   - `qemu_arm`
   - `pico1`
   - `pico2`
   - `pico1calc`
   - `pcxt`
3. Exercise deep paths:
   - shell startup
   - `execve` and `vfork`
   - VFS path lookup and directory reads
   - TTY and pipe blocking paths
   - CP/M subsystem calls
   - Human68k / m68k subsystem calls
   - DOS subsystem calls on ia16
4. Record high-water marks in `docs/kernel/stack.md`.
5. Only after measurement, decide whether ARM-M can reduce
   `PROC_KSTACK_SIZE` below 2 KB on any target.
6. If stack pressure remains after subsystem userland migration, reduce the
   remaining deep kernel paths with targeted changes.

## Possible Reduction Tactics

These are intentionally notes, not an implementation plan:

- Avoid large automatic arrays in syscall and VFS paths.
- Move temporary buffers to explicit pages or small reusable kernel buffers.
- Split helper functions only when it reduces live stack depth in measured
  hot paths.
- Flatten bridge/host call chains where they exist only for historical
  layering.
- Prefer continuation blocking over restart replay for paths that would
  otherwise duplicate deep setup frames.
- Keep logging out of measured critical sections unless the measurement is
  explicitly for debug builds.

## Non-Goals

- Do not shrink `PROC_KSTACK_SIZE` based on intuition.
- Do not rewrite subsystem bridge code solely for stack use before the
  userland subsystem plan is resolved.
- Do not introduce architecture-specific stack tricks for ARM-M unless the
  common per-process kernel-stack model remains intact.

## Resume Criteria

Resume this proposal only if one of these becomes true:

- A target overruns or comes close to overrunning its kernel stack in normal
  use.
- The userland subsystem effort is delayed and subsystem stack use blocks
  ARM-M or ia16 work.
- Measurements after userland subsystem migration still show excessive
  kernel-stack use.
- A target needs a smaller `PROC_KSTACK_SIZE` for memory-map reasons and the
  measured margin is not yet sufficient.
