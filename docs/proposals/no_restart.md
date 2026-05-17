# Proposal: Deprecate Syscall Restart

## Summary

PPAP currently has two mechanisms for blocking syscalls:

- **Continuation blocking** — the syscall body calls `sched_switch()` and
  resumes from the same C statement after wakeup.  Used by TTY, pipe, and
  subsystem code.
- **Syscall restart** — the syscall sets `syscall_restart[core]`, blocks,
  yields; the trap-return path rewinds the saved user PC to the syscall
  instruction and restores the first argument so the syscall re-executes from
  scratch.  Used by sleep/poll/wait.

Restart predates per-process kernel stacks.  Now that every architecture has
a per-process kernel stack and a synchronous `sched_switch()` path, the
restart mechanism is no longer needed.  This proposal converts the remaining
restart call sites to continuation blocking and removes the supporting
infrastructure.

## Motivation

Restart was useful when a syscall could not safely preserve kernel state
across `sched_switch()`.  Today it costs:

- **Two parallel mechanisms** for the same job — TTY/pipe authors learn one,
  sleep/poll/wait authors learn the other.
- **An unenforced correctness rule** ("only before externally-visible side
  effects").  Violations are silent.
- **Per-arch trap-return logic** to rewind PC and restore arg0.  Every port
  carries it.
- **Global state** (`syscall_restart[]`, `syscall_saved_arg0[]`) that two
  architectures (ARM, ia16) consult and three (m68k, rv32, xtensa) replace
  with a per-process loop because the globals are unsafe under synchronous
  switching.
- **Special cases** like the `__m68k__` `arch_yield()` branch in
  `trace_stop_current` to avoid nested TRAP #1.

Continuation blocking is one mechanism, one mental model, no unenforced
rules, no trap-return tricks.

## Inventory

**Restart call sites (6):**

1. `src/kernel/core/syscall/sys_time.c:126` — `sys_nanosleep`
2. `src/kernel/core/syscall/sys_time.c:255` — second sleep variant
3. `src/kernel/core/syscall/sys_time.c:301` — third sleep variant
4. `src/kernel/core/syscall/sys_poll.c:110` — `sys_poll` / `select`
5. `src/kernel/core/syscall/sys_proc.c:2053` — `sys_waitpid`
6. `src/kernel/core/syscall/sys_proc.c:316` — `trace_stop_current` (ptrace)

**Infrastructure to retire:**

- `syscall_restart[2]` / `syscall_saved_arg0[2]` globals
  (`src/kernel/core/syscall/syscall.c:37-38`)
- `syscall_set_restart()` (`src/kernel/core/syscall/syscall.c:51`)
- `syscall_restart_loop()` (`src/kernel/core/syscall/syscall.c:495`)
- `pcb_t.syscall_needs_restart` field and its `_Static_assert`
  (`src/kernel/common/core/proc_info.h:200`,
  `src/kernel/core/proc/proc.c:55`)
- `mod_core.syscall_set_restart` slot
  (`src/kernel/common/mod/mod_core.h:191` and the five-file `mod_core` sync)
- ARM trap-return PC-rewind block
  (`src/arch/arm_m/kernel/core/trap.S:93-171`)
- ARM kernel-switch guard on `syscall_restart`
  (`src/arch/arm_m/kernel/core/arm_m_common.c:47`)
- ia16 trap-return reset (`src/arch/i16/kernel/core/trap.S:163`)
- riscv `syscall_restart_loop` dispatch
  (`src/arch/riscv/kernel/core/trap.S:212-227`)
- xtensa `syscall_restart_loop` dispatch
  (`src/arch/xtensa/kernel/core/xtensa_common.c:147-154`)
- m68k target wrappers calling `syscall_restart_loop`
  (`src/target/x68k/kernel/core/target_x68k.c:70`,
  `src/target/qemu_m68k/kernel/core/target_qemu_m68k.c:56`)
- vfork reset of globals (`src/kernel/core/syscall/sys_proc.c:1935-1938`)
- `__m68k__` `arch_yield()` special case in `trace_stop_current`
  (`src/kernel/core/syscall/sys_proc.c:307-315`)

## Migration pattern

Every restart site already has a "wakeup re-entry" prologue that re-checks
the wait predicate.  Conversion turns that prologue into a loop body around
a synchronous `sched_switch()`:

```c
long sys_nanosleep(uintptr_t req_ptr, uintptr_t rem_ptr) {
  /* compute deadline once */
  struct timespec ts;
  if (sys_copy_from_user(&ts, req_ptr, sizeof(ts)) < 0) return -(long)EFAULT;
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
    return -(long)EINVAL;
  uint32_t ticks =
      (uint32_t)ts.tv_sec * PPAP_TICK_HZ + (uint32_t)ts.tv_nsec / NS_PER_TICK;
  if (ticks == 0u) ticks = 1u;
  current->sleep_until = sched_get_ticks() + ticks;

  for (;;) {
    if (current->sig_pending & ~current->sig_blocked) {
      if (timespec32_write_remaining(rem_ptr) < 0) return -(long)EFAULT;
      current->sleep_until = 0;
      return -(long)EINTR;
    }
    if ((int32_t)(sched_get_ticks() - current->sleep_until) >= 0) {
      current->sleep_until = 0;
      return 0;
    }
    current->state = PROC_SLEEPING;
    sched_switch();
  }
}
```

`sched_switch()` is synchronous on every architecture, so the local variables
and the return value flow naturally — no frame rewind, no globals, no
mod_core entry.

## Phased plan

All four phases below have landed.  Per
[reference_proposal_lifecycle](../../README.md), this proposal is kept
only as the migration record and can be deleted at any time.

### Phase 1 — Convert call sites  **(landed)**

1. `sys_nanosleep` and the two sister sleep paths.
2. `sys_poll` / `select`.
3. `sys_waitpid` (unblocked after `arm_trap_frame_switch.md` Phase A
   fixed the latent `arm_kernel_sched_switch` bug that the
   continuation-blocking loop pattern was triggering on ARM).
4. `trace_stop_current` (also dropped the `__m68k__` `arch_yield()`
   special case and made `trace_before_syscall` / `trace_before_subsys`
   return `void`).

### Phase 2 — Retire arch trap-return restart paths  **(landed)**

- ARM Cortex-M: `syscall_saved_arg0` save and `.Lcheck_restart` block
  removed from `trap.S`; `arm_can_kernel_sched_switch` guard simplified.
- ia16: `.Lcheck_restart` / `.Lno_restart` block and
  `PCB_SVC_NEEDS_RESTART_OFFSET` constant removed.
- riscv: `syscall_dispatch` called directly (was `syscall_restart_loop`).
- xtensa: same.
- m68k targets (`x68k`, `qemu_m68k`): same in their `target_*.c`.

### Phase 3 — Delete infrastructure  **(landed)**

- `syscall_restart[2]`, `syscall_saved_arg0[2]`, `syscall_set_restart()`,
  `syscall_restart_loop()` — deleted from `syscall.c` / `syscall.h`.
- `pcb_t.syscall_needs_restart` and its `_Static_assert` — deleted.
- `mod_core.syscall_set_restart` slot — removed across the five-file
  sync (`mod_core.inc`, `mod_core.h`, `core.c`, pcxt's `core.c` and
  `target_pcxt.c`).
- vfork's reset of `syscall_restart[]` in `sys_proc.c` — deleted (the
  `exec_pending` reset stays, that flag still exists).

### Phase 4 — Documentation  **(landed)**

- `docs/kernel/context_switch.md` "Restartable Syscalls" section deleted.
- `docs/kernel/syscall.md` waitpid / pipe / blocking-mechanism sections
  rewritten to describe continuation blocking.
- `docs/kernel/modules.md` mod_core function list updated.
- `docs/targets/arm_m.md`, `rv32.md`, `xtensa.md` — per-arch trap-return
  restart sections deleted.
- `mod_vfs.h` fd_read doc updated to describe the loop pattern.
- Code-comment cross-references in `vfs/pipe.c`, `vfs/tty.c`,
  `sys_proc.c` kept — they still correctly describe the design intent.

## Risk notes

- **Kernel stack depth**: blocking syscalls now keep their frame live across
  `sched_switch()`.  Per-process kstack sizing already assumes this for
  TTY/pipe; sleep/poll/wait frames are small, so no resize is expected.
  A `KSTACK_USAGE_TRACK` pass on ARM during Phase 1 confirms this.
- **Wakeup races**: the predicate re-check must be *inside* the loop, not
  before it.  Mechanical mistake to watch for during review.
- **Signal delivery ordering on ARM**: today an ARM signal interrupting a
  restartable syscall benefits from PendSV deferring the trap-return
  decision.  With direct kernel switching, the signal-pending check at the
  top of the loop is the new single source of truth — same observable
  behavior, simpler to reason about.

## Out of scope

- No change to `fork()` / `vfork()` / `execve()` semantics.
- No change to TTY, pipe, or subsystem continuation-blocking code — they
  already use the target model.
- No change to `arch_yield()` or `sched_check_preempt()` — async preemption
  is orthogonal.
- Sleep / poll-timeout / wait-timeout resolution stays at 10 ms.  The
  continuation-blocking loops introduced here are forward-compatible with
  a deadline-driven wakeup primitive; that work is tracked separately in
  `hires_timer.md`.
- ARM's PendSV-centric scheduling shape is not changed by this proposal.
  The latent issue it has only matters when chained Handler-mode blockings
  occur, which is exactly what makes steps 3–4 hard.  See
  `arm_trap_frame_switch.md` for the fix.
