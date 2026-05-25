# Kernel Thread-Safety Plan

**Status:** proposed.  This is a work plan for making common kernel code
safe under preemption and, where applicable, dual-core execution.

## Goal

Make shared kernel state safe when accessed from multiple process contexts,
timer/preemption points, and hardware IRQ contexts.  The immediate trigger was
VFS path corruption caused by an unlocked common scratch pool, but the same
class exists in several common subsystems.

The desired end state is:

- every shared mutable kernel object has an explicit owner lock or documented
  single-context lifetime;
- process-context code uses sleepable locks when it may block or call into
  drivers/filesystems;
- IRQ-context code never blocks and uses only bounded critical sections;
- process death releases owned sleepable locks so a killed holder cannot
  deadlock the system;
- allocator APIs make their synchronization contract obvious at call sites.

## Non-Goals

- Do not attempt full SMP scalability.  Coarse locks are acceptable when they
  remove races and match the current small-system design.
- Do not hold spinlocks across filesystem, driver, scheduler, or allocator
  calls unless the callee contract explicitly permits it.
- Do not convert target-specific hardware locks in this proposal except where
  they expose a common bug, such as x68k IOCS.
- Do not treat existing tests passing as proof of safety.  Most of these races
  are timing dependent.

## Current State

Some common subsystems already have reasonable locking boundaries:

- `page.c` wraps the page allocator with `SPIN_PAGE`.
- `proc.c` and `sched.c` use `SPIN_PROC` for process-table and scheduler
  state.
- `vfs.c` uses `SPIN_VFS` for mount/vnode pools.  The VFS scratch pool should
  be guarded by the same lock.
- `ufs.c` and `vfat.c` serialize shared filesystem buffers with `SPIN_FS`.
- `klog.c` serializes log output with `SPIN_UART`.

Recent neutral hardening work started covering:

- `fd.c`: descriptor pool allocation and reference counts.
- `pipe.c`: pipe pool and ring metadata.
- `tmpfs.c`: global inode table and data-page accounting now use a
  sleepable mutex.
- `x68k_iocs.c`: target IOCS traps are serialized with `kmutex_t`.
- `fd.c`: open-file state is pinned and serialized per `struct file`.
- `pipe.c`, `tty.c`, and `poll`: blocking paths use shared sleep helpers.

This is not enough to declare the common kernel thread-safe.

## Known Issues

### 1. Sleepable Lock Primitive Is Missing

Several resources need mutual exclusion across code that can block, call VFS
callbacks, call device drivers, allocate memory, or voluntarily schedule.
Spinlocks are the wrong tool for those paths.

Needed primitive:

```c
typedef struct kmutex {
  pcb_t *volatile owner;
  struct kmutex *next_held;
} kmutex_t;

void kmutex_init(kmutex_t *m);
void kmutex_lock(kmutex_t *m);
void kmutex_unlock(kmutex_t *m);
void kmutex_release_owned(pcb_t *p);
```

Required semantics:

- `kmutex_lock()` blocks only in process context.
- Lock acquisition and release use a spinlock only for the owner/list update.
- Waiters block on `wait_channel == m`.
- Unlock wakes waiters after clearing owner.
- Each process tracks held mutexes so `proc_free()` can release them.
- Lock attempts from hardware IRQ context panic or fail loudly.
- Unlock by a non-owner panics.
- Recursive lock by the same owner is either forbidden or explicitly counted;
  prefer forbidden until a real recursive user appears.

Open design point: add a common `arch_in_irq()` or `kernel_in_irq()` helper.
Targets without nested IRQ tracking can return false initially, but hardware
IRQ entry points should eventually maintain a nesting counter.

### 2. `fd.c` Shared File State Needs Follow-Up Audits

`fd_pool[]` allocation and refcounts are protected with `SPIN_FD`, and the
open-file instance now has a `kmutex_t` for mutable state:

- `struct file.offset`
- `struct file.flags`
- `struct file.vnode`
- `struct file.priv`
- close-vs-I/O lifetime

The first pass pins descriptor IDs before I/O, locks the file while reading or
mutating open-file state, and releases the temporary pin afterwards.  This
serializes shared offsets across `dup()` / fork-like descriptor sharing.

Remaining follow-up:

- audit killed-process cleanup for descriptor pins held by interrupted blocking
  file operations;
- decide whether `fcntl(F_SETFL)` should be allowed to update nonblocking mode
  while another shared operation is asleep;
- split per-driver blocking from file-state mutation if a driver needs
  concurrent operations on the same open-file instance.

### 3. Blocking Paths Need Continued Lost-Wakeup Audits

`pipe.c` protects ring metadata with `SPIN_PIPE`, and blocking pipe waits now
use `sched_sleep_current_unlock()` so the current process is marked blocked
before `SPIN_PIPE` is released.  TTY and poll waits use
`sched_sleep_current()` for the common block-and-switch sequence.

The remaining raw wait-channel writes are special cases:

- `kmutex.c` updates owner and wait state under `SPIN_PROC`;
- `vfork` blocks the parent before making the child runnable;
- signal and process teardown paths wake or clear blocked processes directly;
- timer timeout handling clears `wait_channel` during tick processing.

Remaining follow-up:

- document whether wakeups are level-triggered by rechecking the condition or
  edge-triggered;
- consider a debug assertion for `sched_sleep_current_unlock()` callers that
  pass an invalid spinlock ID;
- decide whether process teardown should use helper APIs for direct
  `PROC_BLOCKED` state changes.

### 4. `tmpfs.c` Uses a Coarse Sleepable Mutex

`tmpfs` has global metadata:

- `inodes[]`
- `data_pages_used`
- file `size`, timestamps, parent/name links

The current coarse `kmutex_t` protection removes simple metadata races without
holding a spinlock across allocator or page I/O calls.  This keeps tmpfs
conservative and correct while allowing page allocation and page copies to stay
inside the same serialized operation.

Remaining follow-up:

- make the lock per mount if multiple tmpfs instances become supported;
- split per-inode locks only if contention becomes meaningful;
- keep `SPIN_FS` only for filesystems with short nonblocking shared-buffer
  critical sections.

### 5. `x68k_iocs.c` IOCS Serialization Is Target-Local

The x68k IOCS guard now uses `kmutex_t` for process-context serialization, so
holder death is handled by `proc_free()` through the common held-mutex cleanup.
Early boot calls before `current` exists remain single-context and do not take
the mutex.

Remaining follow-up:

- add an IRQ-context assertion once a common `arch_in_irq()` helper exists;
- keep crash-output bypass behavior for same-owner recursive crash logging;
- consider a hardware-level serial fallback for crash logs that fault while
  IOCS is already held.

### 6. `kmem_alloc()` Contract Is Too Easy To Violate

`kmem_alloc()` and `kmem_free()` are intentionally low-level and unlocked.
Callers are expected to choose the correct subsystem lock.  The VFS scratch
bug came from one caller missing that convention.

Plan:

- document `kmem_alloc()` as unlocked in `kmem.h` and `mod_core.h`;
- audit every direct `kmem_alloc()` / `kmem_free()` caller;
- prefer subsystem wrapper APIs so callers do not touch raw pools directly;
- optionally add debug assertions for known pools if a lock-tracking facility
  appears.

### 7. Boot-Only Registries Need Explicit Contracts

Some global registries are currently unprotected because they are expected to
be initialized before user scheduling starts:

- `blkdev_table[]` in `blkdev.c`;
- devfs hardware hook pointers for backlight/power/battery style devices;
- target driver registration tables.

Plan:

- document boot-only registration requirements;
- assert or guard if runtime registration is expected later;
- use simple spinlocks if lookup and registration can happen concurrently.

### 8. Process-Local State Still Needs Lifecycle Audit

Process fields are often assumed to be touched only by `current`, but signals,
exit/reparenting, wait, vfork, and scheduler paths can touch another process:

- `fd_map[]`
- `cwd`
- signal masks and pending sets
- parent/child links
- `wait_channel`, `sleep_until`, and state transitions
- page tracking arrays

Plan:

- list every `pcb_t` field and assign a protection rule:
  `current-only`, `SPIN_PROC`, file mutex, signal lock, or immutable after
  exec;
- update `proc_info.h` comments with those rules;
- convert ambiguous cross-process updates to helper functions.

## Implementation Order

### Phase 1: Contracts And Audits

1. Add a shared document section or comments for spinlock IDs and ownership.
2. Audit all `kmem_alloc()` / `kmem_free()` callers.
3. Audit all raw `wait_channel` / `PROC_BLOCKED` users.
4. Audit all `static` mutable globals under `src/kernel`.
5. Mark boot-only registries as boot-only or add locks.

### Phase 2: Sleepable Mutex

1. Add `src/kernel/common/sync/kmutex.h`.
2. Add `src/kernel/common/sync/kmutex.c` or keep it inline if module
   boundaries require that.
3. Add held-mutex list fields to `pcb_t`.
4. Call `kmutex_release_owned(p)` from `proc_free()`.
5. Add IRQ-context detection hooks or conservative panic checks.
6. Add unit or kernel tests for lock, unlock, contention, bad owner, and
   process-death cleanup.

### Phase 3: Convert High-Risk Users

1. Convert x68k IOCS from busy flag to `kmutex_t`.
2. Add per-file mutex/pinning in `fd.c`.
3. Revisit pipe blocking with common sleep helpers.
4. Convert tmpfs to a sleepable per-filesystem lock if allocator-under-spinlock
   becomes a problem.

### Phase 4: Normalize Sleep/Wakeup

1. Introduce sleep helper APIs.
2. Convert pipe, poll, nanosleep, vfork/wait, and target locks.
3. Define the rule for when `sched_wakeup()` may be called with or without the
   resource lock held.
4. Add stress tests that intentionally interleave waiters and wakers.

### Phase 5: Stress Testing

Add tests that create real concurrency instead of only single-thread syscall
coverage:

- parallel opens/closes/dups of shared files;
- concurrent `read()`/`lseek()` on one descriptor;
- pipe reader/writer/closer interleavings;
- tmpfs create/unlink/rename/read stress;
- forced process kill while holding a `kmutex_t`;
- repeated multi-getty startup on x68k;
- optional qemu timer-frequency increase to expose preemption races.

## Acceptance Criteria

The proposal is complete when:

- all common mutable globals have an explicit protection rule;
- no raw pool allocator use lacks a documented lock;
- no blocking path holds a spinlock across a scheduler, VFS, driver, or
  allocator call unless proven nonblocking;
- sleepable locks are released on process death;
- raw sleep/wakeup patterns are centralized or documented;
- qemu_arm, qemu_rv32, qemu_m68k, pcxt, and x68k run their normal test suites
  without regressions;
- stress tests cover fd, pipe, tmpfs, and process-death lock cleanup.
