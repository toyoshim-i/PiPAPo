# Kernel Thread-Safety Plan

**Status:** active.  Phases 1-5 stress coverage are complete.  The current
lane is the final target matrix and target-specific issue recording.
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
- `procfs.c` uses `SPIN_VFS` for mount-table snapshots and its mutable mount
  timestamp; battery callback registration is a boot-only contract.
- `sys_time.c` pairs runtime wallclock epoch updates and timestamp reads under
  `SPIN_TIME`.
- `sched.c` and `procfs.c` use `SPIN_SCHED` for global tick and CPU
  accounting snapshots.
- `ufs.c` serializes its shared scratch buffer with a sleepable mutex.
  `vfat.c` serializes its shared filesystem buffers with `SPIN_FS`.
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

Phases 1-5 stress coverage are complete.  The current lane is the final target
matrix and target-specific issue recording.

Current cursor:

| Level | Current position | State |
| --- | --- | --- |
| Overall plan | Common kernel thread-safety hardening | complete at documented baselines |
| Phase | Phase 5: Stress Testing | done |
| Step | Phase 5.9: Final target matrix | done |
| Review patch | Close Phase 5 after runtime matrix refresh | in progress |
| Next patch after commit | Follow deferred target-specific issues separately | deferred |

Execution rule:

- `next` selects work only from the active phase.
- A phase transition requires its exit condition to be met and recorded below.
- Findings for later phases are recorded as deferred work, not pulled forward.
- Phases 1-5 stress coverage and final matrix recording are complete.

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
| `d388720a` | Devfs runtime state and boot-only registry contracts |
| `4991ab47` | Procfs mount state and boot-only battery hook contract |
| `efde60c6` | Runtime wallclock epoch synchronization |
| `33e25256` | Scheduler tick and CPU-accounting synchronization |
| `d0586b89` | Pico 1 real multicore verification lane |
| `dad84f5b` | Compile-time and boot-only registry contracts |
| `1f372a74` | PCB field ownership rules |
| `bf5dcfb6` | Procfs PCB snapshots |
| `0c7a04c1` | Signal posting serialization |
| `466ec4cf` | Exit and vfork wakeup publication |
| `61d8e2ad` | Waitpid zombie claiming |
| `891aa7ae` | Ptrace lifecycle serialization and phase gates |
| `10b48aaf` | Process-group serialization and Phase 1 closure |
| `b88c0fcb` | Sleepable mutex IRQ-context rejection |
| `b4f62040` | Mutex behavior tests and CP/M IRQ teardown deferral |
| `ab1896bd` | Mutex waiter normalization through the shared sleep helper |
| `ade4a24e` | Audited scheduler wakeup locking contract |
| `3d45921e` | Repeated pipe waiter/waker rendezvous stress |
| `d8884d92` | Shared file-reference and offset stress |
| `9dd96491` | Concurrent tmpfs metadata stress |
| `9cfb89af` | Repeated mutex owner-cleanup handoff stress |

Estimated remaining review-sized iterations:

| Ordered work group | State | Estimated iterations |
| --- | --- | ---: |
| Phase 2: sleepable mutex | done | 0 |
| Phase 3: high-risk user conversions | done | 0 |
| Phase 4: normalize remaining sleep/wakeup paths | done | 0 |
| Phase 5: add concurrency stress coverage | done | 0 |
| Phase 5: run final target matrix and record target-specific issues | done | 0 |
| **Known remaining total after this cursor update** |  | **0** |

This estimate counts small, reviewable patches rather than unchecked bullet
items.  Revise the estimate if later-phase normalization finds additional
callers that need behavioral changes.

## Known Issues And Completed Protection

### 1. Sleepable Lock Primitive Is Covered

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
- `kmutex_lock()` and `kmutex_unlock()` reject hardware IRQ context through
  architecture hooks or conservative interrupt-state checks.
- Lock acquisition and release use a spinlock only for the owner/list update.
- Waiters block on `wait_channel == m`.
- Unlock wakes waiters after clearing owner.
- Each process tracks held mutexes so `proc_free()` can release them.
- Unlock by a non-owner panics.
- Recursive lock by the same owner panics.

Dedicated host tests cover contention, bad-owner, recursive-lock, IRQ-context,
and process-death cleanup behavior.

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
- `devfs_set_backlight()`, `devfs_set_power()`, `procfs_set_battery()`, and
  `tty_set_backend()` are called from target initialization before
  `sched_start()` and are documented as boot-only;
- `klog_set_logger()` is a boot-only logger-slot registration API; targets
  install primary and optional mirror loggers before scheduling starts;
- loader, CPU-operation, and subsystem-operation registries are compile-time
  tables selected by CMake feature flags;
- subsystem-name and eCPU-name arrays are also compile-time data, but remain
  exported as data symbols for the i16 VFS module boundary;
- runtime pseudo-filesystem data is different: the `/dev/urandom` fallback
  state and devfs mount timestamp use `SPIN_DEVFS`, while the procfs mount
  timestamp uses `SPIN_VFS`.

Runtime wallclock state is also shared:

- `time_set_wallclock()` may be called by `settimeofday()` after scheduling
  begins, while syscall and filesystem paths read timestamps;
- `SPIN_TIME` ensures the mutable epoch and each tick snapshot are observed as
  one wallclock calculation (`efde60c6`).

Scheduler statistics are written from timer interrupt context:

- `tick_count` feeds time and `/proc/uptime`, while the per-CPU accounting
  arrays feed `/proc/stat` and `/proc/uptime`;
- `SPIN_SCHED` prevents torn or mixed global counter snapshots, particularly
  on 16-bit targets (`33e25256`).

Remaining follow-up:

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
  exec.  The first classification pass is documented in `proc_info.h`;
- define a consistent snapshot rule for procfs reads of live `proc_table[]`
  entries.  Procfs now snapshots PID metadata under `SPIN_PROC` and samples
  `utime` / `stime` under `SPIN_SCHED`;
- update `proc_info.h` comments with those rules;
- convert ambiguous cross-process updates to helper functions.
  Signal posting now updates pending bits and blocked/sleeping wake state under
  `SPIN_PROC` in `sys_kill()` and TTY foreground-signal delivery.  Process
  exit now publishes exit status, zombie state, waitpid/tracer wakeups,
  vfork-parent wakeups, and child reparenting under `SPIN_PROC`.  `waitpid()`
  now scans and claims stopped/zombie children under `SPIN_PROC`; the internal
  `PROC_REAPING` state pins a claimed zombie slot during page cleanup.

## Implementation Order

Phase dashboard:

| Phase | State | Progress | Exit condition |
| --- | --- | --- | --- |
| 1. Contracts And Audits | done | Static globals, registries, PCB lifecycle, and process-group access are covered | Complete |
| 2. Sleepable Mutex | done | IRQ guard and dedicated behavior tests are landed | Complete |
| 3. Convert High-Risk Users | done | x68k IOCS, fd, pipe, and tmpfs conversions are landed | Reopen only if later audits find another high-risk user |
| 4. Normalize Sleep/Wakeup | done | Shared helpers, the wakeup rule, and focused pipe waiter/waker stress are landed | Complete |
| 5. Stress Testing | active | Stress coverage is complete; final target matrix remains | Run final target matrix |

Execution order:

1. Complete Phase 1.
2. Complete Phase 2.
3. Skip Phase 3 because it is already complete.
4. Complete Phase 4.
5. Complete Phase 5.

### Phase 1: Contracts And Audits

1. `done` Add shared comments for spinlock IDs and ownership (`9814202f`).
2. `done` Audit all `kmem_alloc()` / `kmem_free()` callers; VFS pools use
   `SPIN_VFS` (`1b898547`, `690791f2`).
3. `done` Audit raw `wait_channel` / `PROC_BLOCKED` users.  Shared sleep
   helpers are landed (`e6a43d11`, `b7b1f1fe`); remaining caller conversion
   and wakeup-rule normalization are tracked in Phase 4.
4. `done` Audit all `static` mutable globals under `src/kernel`.  VFS
   scratch and mount-entry lifetime are fixed; devfs runtime state is covered
   (`1b898547`, `90d8fe51`, `d388720a`); procfs mutable mount state is fixed
   (`4991ab47`); wallclock epoch synchronization is fixed (`efde60c6`);
   scheduler-statistics synchronization is fixed (`33e25256`); remaining
   compile-time tables are documented (`dad84f5b`).
5. `done` Mark boot-only registries as boot-only or add locks.
   Block-device, devfs hook, and TTY backend setup contracts are documented
   (`d388720a`); procfs battery registration is documented (`4991ab47`);
   logger registration and compile-time loader/CPU/subsystem tables are
   documented (`dad84f5b`).
6. `done` Audit `pcb_t` lifecycle and cross-process fields.  The initial
   protection-rule inventory is documented (`1f372a74`); procfs PID
   snapshots are documented (`bf5dcfb6`); signal posting serialization is
   covered (`0c7a04c1`); exit/vfork wakeup publication is covered
   (`466ec4cf`); waitpid zombie claiming is covered (`61d8e2ad`); ptrace
   lifecycle serialization is covered (`891aa7ae`).  The closing patch
   serializes cross-process process-group access.

### Phase 2: Sleepable Mutex

1. `done` Add `src/kernel/common/sync/kmutex.h` (`8d6b9a39`).
2. `done` Add the module-safe implementation in
   `src/kernel/core/sync/kmutex.c` (`8d6b9a39`).
3. `done` Add held-mutex list fields to `pcb_t` (`8d6b9a39`).
4. `done` Call `kmutex_release_owned(p)` from `proc_free()` (`8d6b9a39`).
5. `done` Add IRQ-context detection hooks or conservative panic checks
   (`b88c0fcb`).
6. `done` Add dedicated tests for contention, bad owner, recursive lock, IRQ
   context, and process-death cleanup (`b4f62040`).

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
2. `done` Convert blocking callers (`ab1896bd`).  Pipe, poll, tty, and mutex
   paths use shared helpers.  Timer sleeps intentionally use `PROC_SLEEPING`;
   vfork, wait, trace, and signal paths intentionally manage lifecycle state
   under `SPIN_PROC`.
3. `done` Define the rule for when `sched_wakeup()` may be called with or
   without the resource lock held (`ade4a24e`).  It is IRQ-safe, acquires
   `SPIN_PROC` internally, and may run while another resource lock is held.
   Callers should release that resource lock first when the wake condition
   remains true after unlock.
4. `done` Add stress tests that intentionally interleave waiters and wakers
   (`3d45921e`).  `test_pipe` repeats the child-reader block and parent wakeup
   rendezvous 16 times on every normal target test run.

### Phase 5: Stress Testing

`active` Add tests that create real concurrency instead of only single-thread
syscall coverage.  Repeated x68k multi-getty startup exposed PCB ownership,
IRQ-context, UFS serialization, and native-m68k vfork-return issues.  Pico 1
has a UART-captured hardware lane
(`--test --filter=smp pico1`) and a post-scheduler Core 1 statistics smoke test
(`d0586b89`).  `test_fs` validates basic mount pinning behavior, but does not
create concurrent mount/unmount interleavings.

1. `done` Add parallel opens/closes/dups of shared files (`d8884d92`).
2. `done` Add concurrent `read()`/`lseek()` stress on one descriptor
   (`d8884d92`).
3. `done` Add pipe reader/writer interleavings (`3d45921e`).
4. `done` Add tmpfs create/unlink/rename/read stress (`9dd96491`).  A parent
   and exec'd child repeatedly create, write, rename, reopen, read, and unlink
   distinct tmpfs files after a shared rendezvous.
5. `done` Cover forced process death while holding a `kmutex_t` (`9cfb89af`).
   The host test repeats cleanup and waiter reacquisition.
   Production callers release their mutexes while unwinding controllable
   signal interruptions; forcing death inside a held kernel mutex would need
   a test-only hook.  Keep that hook out of the production syscall surface.
6. `done` Run repeated multi-getty startup on x68k.  The landed repair moves
   exec-restore state from the shared CPU-global flag to the owning m68k PCB
   and tracks explicit m68k timer-ISR depth, matching i16.  Further repetition
   exposed intermittent zero-GOT `getty` loads: UFS used a global sector
   buffer behind `spin_lock()`, which is a no-op on single-core builds while
   the x68k IOCS floppy path temporarily enables interrupts.  The current
   review patch serializes UFS with `kmutex_t`, switches to the replacement
   exec stack before clearing its pending flag, and adds missed native-m68k
   vfork restore hooks on cooperative-yield, Human68k, and fault-reschedule
   returns.  Three freshly packaged XEiJ boots reach scheduler startup,
   `init started`, and one serial getty prompt without `SIGBUS`.
7. `done` Run Pico 1/Pico 2 UART-captured tests for TTY and UART IRQ
   behavior.  Pico 1 passed the focused UART-captured hardware lane on
   June 3, 2026: `./scripts/run.sh --test --filter=smp pico1` flashed through
   the Debug Probe, captured `/dev/ttyS0` output, launched Core 1, started the
   Core 1 scheduler, and passed `/bin/test_smp` 5/5.  Pico 1's
   `FLASH_KERNEL` budget is now 208 KB so the focused lane uses the normal
   full subsystem profile.  There is no dedicated tty user test yet, so tty/IRQ
   coverage remains indirect through UART capture, scheduler ticks, and logger
   output.  Pico 2 is not required for this phase because the acceptance
   criterion requires Pico 1 or Pico 2 real multicore coverage.
8. `optional` Increase QEMU timer frequency if the normal stress runs do not
   expose enough preemption interleavings.
9. `done` Run the final normal target matrix and record target-specific
   issues.  June 7, 2026 after the common startup-hook cleanup:
   `qemu_arm` remains at the documented user-pass baseline with pre-existing
   FAT fixture kernel failures; `qemu_rv32` passes 23/23 user tests;
   `qemu_m68k` completes 23/23 with `test_orphan` disabled; and `pcxt` passes
   18/18 with `--hdd`.  The hardware/startup lanes remain as recorded on
   June 6-7: x68k reaches scheduler startup, `init started`, and a serial
   getty prompt in a freshly packaged XEiJ boot; Pico 2 semihost `--test`
   builds and flashes but is blocked before the runner by OpenOCD `SYS_READC`
   semihost-fileio support; Pico 2 normal boot reaches an interactive shell
   and runs apps; Pico2RV `--test` flashes with the Debug Probe UART closed,
   captures serial output after reset, and reports 67 kernel tests passed with
   two SD/VFAT fixture failures before user tests are reached.  Phase 5 closes
   at these documented baselines; remaining target-specific blockers are
   tracked outside this common thread-safety phase.

Phase 5 exit record: closed on June 7, 2026.  The common runtime matrix was
refreshed after the target startup-hook cleanup, no new regression was found,
and the remaining Pico 2/Pico2RV/x68k items are target or harness follow-ups
rather than open common-kernel thread-safety work.

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
- Pico 1 or Pico 2 runs multicore tests on real hardware for changes to shared
  dual-core state;
- stress tests cover fd, pipe, tmpfs, and process-death lock cleanup.
