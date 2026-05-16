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

### Phase 1 — Convert call sites

One PR per group, easy to bisect.

1. `sys_nanosleep` and the two sister sleep paths — **landed**.
2. `sys_poll` / `select` — **landed**.
3. `sys_waitpid` — **blocked**, see below.
4. `trace_stop_current` — **blocked**, same reason.

Each PR rewrites the body to a loop, removes the `syscall_set_restart()`
call, and deletes the now-obsolete "re-entry" comments.  Test coverage:
existing sleep, poll, waitpid, and ptrace tests on ARM, m68k, rv32, xtensa.

**Blocked on:** [`arm_trap_frame_switch.md`](arm_trap_frame_switch.md)
Phase A.  Attempting step 3 (waitpid) on ARM exposes a latent bug in
`arm_kernel_sched_switch`'s restore path (uses `bx lr` to a saved C
return address instead of `EXC_RETURN`, leaving exception-state
bookkeeping skewed).  vfork tolerates a single use; chaining vfork +
waitpid through one user-mode round-trip — which any CP/M-subsystem
test does — hangs reliably.  Converting ARM SVC blocking to the same
trap-frame swap that m68k / riscv / xtensa already use eliminates the
mechanism, the bug, and the dependency.  Steps 3–4 resume immediately
afterwards.  m68k, rv32, xtensa, ia16 are unaffected by this dependency
and could in principle migrate first, but bundling under one ARM cleanup
is cleaner.

### Phase 2 — Retire arch trap-return restart paths

- ARM Cortex-M: delete the `syscall_restart` check and PC-rewind block in
  `trap.S`; drop the `syscall_restart[core_id()] == 0` guard in
  `arm_m_common.c`.
- ia16: drop the `movw $0, syscall_restart` in `trap.S`.
- riscv: drop the `syscall_restart_loop` call; dispatch the syscall directly.
- xtensa: same in `xtensa_common.c`.
- m68k targets (`x68k`, `qemu_m68k`): same in their `target_*.c`.

### Phase 3 — Delete infrastructure

- `syscall_restart[2]`, `syscall_saved_arg0[2]`,
  `syscall_set_restart()`, `syscall_restart_loop()`
- `pcb_t.syscall_needs_restart` and its `_Static_assert`
- `mod_core.syscall_set_restart` slot (the five-file sync the memory warns
  about) and the `pcxt` target patch wiring
- vfork reset of the globals in `sys_proc.c`

### Phase 4 — Documentation

- `docs/kernel/context_switch.md` — delete the "Restartable Syscalls"
  section (or keep one paragraph as a historical note).
- Cross-references in `vfs/pipe.c`, `vfs/tty.c`, `mod/mod_vfs.h` comments.
- Remove the corresponding `MEMORY.md` line for the five-file `mod_core`
  sync once the slot is gone.

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
