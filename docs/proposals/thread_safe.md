# Kernel Thread-Safety Plan

**Status:** active.  Progress tracked through `90d8fe51` on May 26, 2026.
This is a work plan for making common kernel code safe under preemption and,
where applicable, dual-core execution.

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
- `vfs.c` uses `SPIN_VFS` for the vnode and VFS scratch pools and for mount
  mutation.  Path resolution pins an active mount through its root vnode.
- `devfs.c` uses `SPIN_DEVFS` for runtime pseudo-device state; callback and
  block-device registration APIs are boot-only contracts.
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

## Progress Tracking

Status labels used below:

- `done`: implementation and normal target verification landed;
- `active`: the current audit/fix lane;
- `partial`: useful implementation landed, but defined follow-up remains;
- `todo`: no reviewed implementation has landed yet.

The current lane is the static mutable-global audit in Phase 1.  The VFS
scratch pool, VFS allocator contract, and mount-table lifetime are covered;
the current pending change protects devfs runtime state and documents its
boot-time registry contracts.  Process-owned state and remaining registries
are next.

Completed implementation commits:

| Commit | Result |
| --- | --- |
| `9814202f` | Initial common VFS, fd, pipe, and tmpfs hardening plan |
| `1b898547` | VFS scratch-pool serialization and allocator contract |
| `e6a43d11`, `b7b1f1fe` | Shared blocking/sleep helper paths |
| `8d6b9a39` | Process-owned sleepable mutex primitive |
| `5701a06d` | x68k IOCS serialization through `kmutex_t` |
| `0df96d04` | Open-file pinning and per-file mutex protection |
| `14488135` | Sleepable tmpfs metadata protection |
| `690791f2` | VFS `kmem` lock-ownership audit |
| `90d8fe51` | VFS mount-entry lifetime and regression coverage |

Estimated remaining review-sized iterations:

| Work group | State | Estimated iterations |
| --- | --- | ---: |
| Finish static-global and boot-registry audit | active | 1-2 |
| Audit `pcb_t` lifecycle and cross-process fields | todo | 1-2 |
| Finish fd and sleep/wakeup lifecycle follow-ups | partial | 1-2 |
| Add mutex IRQ-context guard and dedicated cleanup tests | partial | 1-2 |
| Add concurrency stress coverage for protected subsystems | todo | 2-3 |
| **Known remaining total** |  | **6-11** |

This estimate counts small, reviewable patches rather than unchecked bullet
items.  The static-global and process-lifecycle audits may identify additional
required fixes, so the upper bound should be revised as those inventories are
completed.

## Known Issues And Completed Protection

### 1. Sleepable Lock Primitive Exists; IRQ Guard And Tests Remain

Several resources need mutual exclusion across code that can block, call VFS
callbacks, call device drivers, allocate memory, or voluntarily schedule.
Spinlocks are the wrong tool for those paths.  `kmutex_t` is now implemented
and releases held mutexes from `proc_free()`.

Implemented primitive:

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

Implemented semantics:

- `kmutex_lock()` blocks only in process context.
- Lock acquisition and release use a spinlock only for the owner/list update.
- Waiters block on `wait_channel == m`.
- Unlock wakes waiters after clearing owner.
- Each process tracks held mutexes so `proc_free()` can release them.
- Unlock by a non-owner panics.
- Recursive lock by the same owner panics.

Remaining follow-up:

- add a common `arch_in_irq()` or `kernel_in_irq()` helper, then make lock
  attempts from hardware IRQ context panic or fail loudly;
- add dedicated contention, bad-owner, recursive-lock, and process-death
  cleanup tests.

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

### 6. `kmem_alloc()` Contract Audit

`kmem_alloc()` and `kmem_free()` are intentionally low-level and unlocked.
Callers are expected to choose the correct subsystem lock.  The VFS scratch
bug came from one caller missing that convention.  The allocator headers now
state the unlocked contract explicitly.

The current production caller audit found only the VFS pools:

- `vnode_pool` allocations, releases, and free-count queries are under
  `SPIN_VFS`;
- `vfs_scratch_pool` allocations and releases are under `SPIN_VFS`;
- `vfs_umount()` frees its root vnode directly only while already holding
  `SPIN_VFS`, avoiding recursive acquisition through `vfs_vnode_release()`.

Remaining follow-up:

- prefer subsystem wrapper APIs if additional pools appear;
- optionally add debug assertions for known pools if a lock-tracking facility
  appears.

### 7. VFS Mount Entry Lifetime

Mount entries are shared by path lookup, open descriptors, `statfs`, and
`/proc/mounts`, while `mount()` and `umount()` can mutate or reuse entries.

Current protection:

- `MNT_STATE_MOUNTING` reserves a slot before a mount callback runs and keeps
  partial state invisible to readers;
- duplicate or concurrent mounts at the same path return `-EBUSY`;
- `vfs_mount_find()` takes a reference on the selected mount root, so path
  walking and `statfs` cannot race with unmount entry reuse;
- non-root vnodes and additional root references make `umount()` return
  `-EBUSY`;
- `/proc/mounts` formats the table while holding `SPIN_VFS`.

Remaining follow-up:

- add a targeted concurrent mount/unmount stress test once process-level
  concurrency test helpers exist.

### 8. Boot-Only Registries Need Explicit Contracts

Some global registries are currently unprotected because they are expected to
be initialized before user scheduling starts:

- `blkdev_table[]` in `blkdev.c`;
- devfs hardware hook pointers for backlight/power/battery style devices;
- target driver registration tables.

Current audit result:

- `blkdev_register()` and current `loopback_setup()` callers run only during
  bootstrap or pre-scheduler kernel tests; headers now prohibit runtime
  registration without a new synchronization/lifetime design;
- `devfs_set_backlight()`, `devfs_set_power()`, and `tty_set_backend()` are
  called from target initialization before `sched_start()` and are documented
  as boot-only;
- runtime `devfs` data is different: the `/dev/urandom` fallback state and the
  mount timestamp now use `SPIN_DEVFS`.

Remaining follow-up:

- complete the inventory of any remaining target registration or hook tables;
- add a guard or lock if a future runtime registration user is introduced.

### 9. Process-Local State Still Needs Lifecycle Audit

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

1. `done` Add shared comments for spinlock IDs and ownership (`9814202f`).
2. `done` Audit all `kmem_alloc()` / `kmem_free()` callers; VFS pools use
   `SPIN_VFS` (`1b898547`, `690791f2`).
3. `partial` Audit raw `wait_channel` / `PROC_BLOCKED` users.  Shared sleep
   helpers are landed; lifecycle rules and special cases remain
   (`e6a43d11`, `b7b1f1fe`).
4. `active` Audit all `static` mutable globals under `src/kernel`.  VFS
   scratch and mount-entry lifetime are fixed; devfs runtime state is covered
   by the current pending change; continue the inventory (`1b898547`,
   `90d8fe51`).
5. `partial` Mark boot-only registries as boot-only or add locks.  The current
   pending change documents block-device, devfs hook, and TTY backend setup
   contracts; remaining target registries still need inventory.

### Phase 2: Sleepable Mutex

1. `done` Add `src/kernel/common/sync/kmutex.h` (`8d6b9a39`).
2. `done` Add the module-safe implementation in
   `src/kernel/core/sync/kmutex.c` (`8d6b9a39`).
3. `done` Add held-mutex list fields to `pcb_t` (`8d6b9a39`).
4. `done` Call `kmutex_release_owned(p)` from `proc_free()` (`8d6b9a39`).
5. `todo` Add IRQ-context detection hooks or conservative panic checks.
6. `todo` Add dedicated tests for contention, bad owner, recursive lock, and
   process-death cleanup.

### Phase 3: Convert High-Risk Users

1. `done` Convert x68k IOCS from busy flag to `kmutex_t` (`5701a06d`).
2. `done` Add per-file mutex/pinning in `fd.c` (`0df96d04`).
3. `done` Convert pipe blocking to the shared unlock-and-sleep helper
   (`b7b1f1fe`).
4. `done` Convert current tmpfs shared metadata to a coarse sleepable mutex
   (`14488135`).  A per-mount split remains conditional on multiple tmpfs
   instances.

### Phase 4: Normalize Sleep/Wakeup

1. `done` Introduce sleep helper APIs (`e6a43d11`, `b7b1f1fe`).
2. `partial` Convert blocking callers.  Pipe, poll, and tty paths use shared
   helpers; audit the remaining timer, process, vfork/wait, and target paths.
3. `todo` Define the rule for when `sched_wakeup()` may be called with or
   without the resource lock held.
4. `todo` Add stress tests that intentionally interleave waiters and wakers.

### Phase 5: Stress Testing

`todo` Add tests that create real concurrency instead of only single-thread
syscall coverage.  `test_fs` now validates basic mount pinning behavior, but
does not create concurrent mount/unmount interleavings.

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
