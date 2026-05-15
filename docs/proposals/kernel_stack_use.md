# Kernel Stack Use Reduction

**Status:** dropped for now.  Do not execute this work before the userland
subsystem migration decides which subsystem paths remain in the kernel.

## Goal

Reduce kernel-stack use if subsystem-side paths still create pressure after
the context-switch cleanup.  Ordinary PPAP syscall paths are already optimized
well enough that broad usage-tracking work is not an active cleanup task.

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

For now, keep this proposal only as background context.  It is not an active
cleanup plan.

## Current State

- ARM-M targets use fixed per-process kernel-stack slots.
- ARM-M user-process slots are currently 2 KB.
- ia16 uses fixed 1 KB user-process kernel-stack slots.
- Common PPAP syscall paths are not the main stack-risk area now.
- Any remaining pressure is expected to be in subsystem bridges, host shims,
  loaders, or emulator-facing glue.
- `docs/kernel/context_switch.md` and `docs/kernel/stack.md` document the
  per-process-kernel-stack context-switch model.

## Dropped Work

- Add broad stack-usage measurement just for this cleanup.
- Rewrite subsystem bridge code solely to reduce stack depth before the
  userland subsystem migration.
- Exercise deep subsystem paths as an active cleanup task:
   - shell startup
   - `execve` and `vfork`
   - VFS path lookup and directory reads
   - TTY and pipe blocking paths
   - CP/M subsystem calls
   - Human68k / m68k subsystem calls
   - DOS subsystem calls on ia16
- Shrink `PROC_KSTACK_SIZE` as part of this proposal.

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

- Do not shrink `PROC_KSTACK_SIZE` as part of this deferred proposal.
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
