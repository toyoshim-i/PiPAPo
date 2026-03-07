# Phase 9: Dual-Core Scheduling — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 3 weeks
Prerequisites: Phase 8 (PIE Binary Optimization) complete
**Status: Not started**

---

## Context

Core 1 of the RP2040 is currently idle — `core1_io_worker()` runs a stub echo
loop.  This phase enables both cores to execute user processes, doubling
effective CPU throughput for concurrent workloads (e.g. shell pipelines).

This requires: hardware spinlock API, spinlock protection on shared kernel
state, per-core `current` pointer and SVC/PendSV state, a core-aware
scheduler, and a Core 1 entry point that mirrors Core 0's `sched_start()`.

## Design Decisions

**Per-core `current`:**
```c
extern pcb_t *current_core[2];
static inline uint32_t core_id(void) {
    return *(volatile uint32_t *)0xD0000000u;  /* SIO_CPUID */
}
#define current (current_core[core_id()])
```
Every existing use of `current` resolves to the correct per-core pointer
automatically.  Assembly code (switch.S, svc.S) reads SIO_CPUID to index
`current_core[]`.  Single MMIO load, ~1 cycle.

**Hardware spinlocks:** RP2040 provides 32 hardware spinlocks at SIO+0x100.
Read = try-acquire (returns 0 if already held), write any value = release.

| Lock | # | Protects |
|------|---|----------|
| SPIN_PAGE | 0 | free_stack, free_top (page allocator) |
| SPIN_PROC | 1 | proc_table, next_pid, running_on_core |
| SPIN_VFS  | 2 | mount table, vnode pool |
| SPIN_FS   | 3 | sector_buf (vfat.c), ufs_buf (ufs.c) |

Pattern: disable local IRQs before acquire, re-enable after release — prevents
deadlock if an ISR tries to acquire the same lock.

**Scheduler:** `sched_next()` acquires SPIN_PROC, scans for RUNNABLE processes,
skips any whose `running_on_core` matches the *other* core.  New PCB field:
`int8_t running_on_core` (-1 = not running, 0 = Core 0, 1 = Core 1).

**FS serialization:** With both cores doing syscalls, SPIN_FS serializes all
filesystem I/O.  Acceptable — SD card SPI is the throughput bottleneck.

**QEMU compatibility:** Core 1 launch is already skipped on QEMU (CPUID !=
Cortex-M0+).  `core_id()` returns 0 on QEMU, so `current_core[0]` preserves
single-core behavior.  Spinlock acquire/release still executes (IRQ
disable/enable), but with no contention the spin never loops.

## Exit Criteria

1. `scripts/test_all_targets.sh` builds all three targets
2. `scripts/qemu-test.sh` passes all existing tests (single-core regression)
3. On pico1/pico1calc: both cores execute user processes (visible via
   `SYS_GETCPU` returning 0 and 1 for different processes)
4. Shell pipeline (`cat file | grep pat | sort`) runs correctly with processes
   distributed across cores
5. No deadlocks or HardFaults during interactive use

---

## Week 1 — Hardware Spinlock API + Shared State Protection

### ✓ Step 1 — Hardware spinlock API

**File:** NEW `src/kernel/spinlock.h`

QEMU fix: `spin_have_hw()` checks SCB.CPUID (Cortex-M0+ PARTNO = 0xC60) to
detect RP2040 vs QEMU (mps2-an500/Cortex-M3).  Hardware lock acquire/release
is skipped on non-M0+ targets — IRQ disable alone is sufficient for
single-core.

Boot fix: `spin_locks_reset()` writes all 32 SIO spinlock registers at early
boot (called from `kmain()` before any init).  On RP2040, the SIO block is NOT
reset by a Core 0 reset (e.g. GDB reload + `monitor reset halt`), so stale
locks from a previous session would cause the first acquire to hang forever.
The pico-sdk does the same in `runtime_init → spin_locks_reset()`.

### ✓ Step 2 — Protect page allocator with SPIN_PAGE

**File:** `src/kernel/mm/page.c`

Wrapped `page_alloc()`, `page_alloc_at()`, `page_free()`, `page_free_count()`
with `spin_lock_irqsave(SPIN_PAGE)` / `spin_unlock_irqrestore(SPIN_PAGE, saved)`.

### ✓ Step 3 — Protect proc_table with SPIN_PROC

**Files:** `src/kernel/proc/proc.c`, `src/kernel/proc/sched.c`

- `proc_alloc()`: SPIN_PROC around proc_table scan + `next_pid++`
- `proc_free()`: SPIN_PROC around state = PROC_FREE
- `sched_next()`: SPIN_PROC around runnable scan
- `sched_wakeup()`: SPIN_PROC around state transitions; PENDSVSET moved outside lock

### ✓ Step 4 — Protect VFS with SPIN_VFS

**File:** `src/kernel/vfs/vfs.c`

- `vnode_alloc()`, `vnode_ref()`, `vnode_put()`: SPIN_VFS around pool/refcnt ops
- `vfs_mount()`: SPIN_VFS around mount table; **releases lock before calling
  ops->mount()** to avoid re-entrant deadlock (RP2040 hardware spinlocks are
  NOT re-entrant — same-core re-acquire returns 0 → infinite spin)
- `vfs_umount()`: SPIN_VFS with inlined vnode_put logic (direct kmem_free)
  to avoid recursive SPIN_VFS acquire

### ✓ Step 5 — Protect FS I/O with SPIN_FS

**Files:** `src/kernel/fs/vfat.c`, `src/kernel/fs/ufs.c`

Used `_locked()` wrapper pattern: each VFS operation gets a thin wrapper that
acquires SPIN_FS, calls the original function, then releases.  The wrappers
are registered in the `vfs_ops_t` table.  `vfat_stat` left unwrapped (no
sector_buf access).

---

## Week 2 — Per-Core State and Core-Aware Handlers

### ☐ Step 6 — Per-core `current` pointer

**Files:**
- `src/kernel/proc/proc.h` — declare `current_core[2]`, `#define current`
- `src/kernel/proc/proc.c` — define `current_core[2]`

Add `int8_t running_on_core` to `pcb_t` (init to -1).

### ☐ Step 7 — Per-core SVC state

**File:** `src/kernel/syscall/syscall.c`

```c
volatile int      exec_pending[2] = {0, 0};
volatile int      svc_restart[2]  = {0, 0};
volatile uint32_t svc_saved_a0[2] = {0, 0};
```

All C code that references these indexes by `core_id()`.

### ☐ Step 8 — Core-aware PendSV_Handler

**File:** `src/kernel/proc/switch.S`

Replace `ldr r2, =current` with core-aware indexing:
```asm
ldr  r1, =0xD0000000     @ SIO_CPUID
ldr  r1, [r1]            @ r1 = core_id (0 or 1)
lsls r1, r1, #2          @ pointer offset
ldr  r2, =current_core
adds r2, r2, r1          @ r2 = &current_core[core_id]
```

After `sched_next()` returns, set `next->running_on_core = core_id` and
clear `outgoing->running_on_core = -1`.

### ☐ Step 9 — Core-aware SVC_Handler

**File:** `src/kernel/syscall/svc.S`

Replace each global reference (`current`, `svc_saved_a0`, `exec_pending`,
`svc_restart`) with core-indexed access via SIO_CPUID.

### ☐ Step 10 — Core-aware scheduler

**File:** `src/kernel/proc/sched.c`

`sched_next()`: acquire SPIN_PROC, scan for RUNNABLE with
`running_on_core < 0`, set `running_on_core = core_id`, clear outgoing.

`sched_tick()`: only Core 0 increments `tick_count`.  Both cores decrement
their own `current->ticks_remaining`.

---

## Week 3 — Core 1 Execution and Integration

### ☐ Step 11 — Core 1 entry point

**File:** `src/kernel/smp.c`

New `core1_sched_entry()` replacing `core1_io_worker()`:
1. Call `mpu_init()` (programs Core 1's own MPU: regions 0,1,3)
2. Set PendSV priority to lowest, SVC priority to 0x80
3. Configure Core 1's SysTick (same SYSTICK_RELOAD)
4. Switch to PSP (from MSP allocated by `core1_launch`)
5. Enable interrupts
6. Enter WFI idle loop (PendSV switches to runnable process)

Update `core1_launch()` to pass `core1_sched_entry` as entry.

### ☐ Step 12 — Update SYS_GETCPU

**File:** `src/kernel/syscall/syscall.c`

```c
case SYS_GETCPU:
    ret = (long)core_id();
    break;
```

### ☐ Step 13 — Per-core CPU stats in /proc/stat

**File:** `src/kernel/proc/sched.c`, `src/kernel/fs/procfs.c`

Per-core `cpu_user_ticks`, `cpu_system_ticks`, `cpu_idle_ticks`.
`/proc/stat` shows `cpu0` and `cpu1` lines.

### ☐ Step 14 — Dual-core test suite

**File:** `tests/kernel/ktest.c`

New test: verify `SYS_GETCPU` returns both 0 and 1 across different
processes.  Verify no deadlocks during concurrent fork/exec.

---

## Files Modified (Complete)

| File | Change |
|------|--------|
| NEW `src/kernel/spinlock.h` | Hardware spinlock API (QEMU detect + boot reset) |
| `src/kernel/main.c` | Call `spin_locks_reset()` at start of `kmain()` |
| `src/kernel/proc/proc.h` | `current_core[2]`, `#define current`, `running_on_core` |
| `src/kernel/proc/proc.c` | Define `current_core[2]`, init running_on_core |
| `src/kernel/proc/sched.c` | SPIN_PROC in sched_next, skip other-core, per-core tick |
| `src/kernel/proc/switch.S` | Read SIO_CPUID, index current_core[] |
| `src/kernel/syscall/svc.S` | Per-core svc_saved_a0[], exec_pending[], current_core[] |
| `src/kernel/syscall/syscall.c` | Per-core arrays; SYS_GETCPU returns core_id() |
| `src/kernel/smp.c` | core1_sched_entry() replaces core1_io_worker() |
| `src/kernel/mm/page.c` | SPIN_PAGE around free_stack access |
| `src/kernel/mm/mpu.c` | mpu_init() called from both cores |
| `src/kernel/vfs/vfs.c` | SPIN_VFS around mount/vnode operations |
| `src/kernel/fs/vfat.c` | SPIN_FS around sector_buf |
| `src/kernel/fs/ufs.c` | SPIN_FS around ufs_buf |
| `src/kernel/fs/procfs.c` | Per-core CPU stats in /proc/stat |
| `tests/kernel/ktest.c` | Dual-core scheduling test |

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Spinlock deadlock between cores | Medium | Fixed acquisition order; IRQs disabled while held |
| Both cores schedule same process | Medium | `running_on_core` + SPIN_PROC prevents double-schedule |
| Core 1 SysTick misconfigured | Low | Same SYSTICK_RELOAD; verified in ktest |
| QEMU regression from per-core changes | Low | core_id()=0 on QEMU; current_core[0] = old behavior |
| FS throughput degraded by SPIN_FS | Low | SD card SPI is the bottleneck |
| Stale spinlocks after GDB reload | **Fixed** | `spin_locks_reset()` at boot; SIO block not reset by Core 0 reset |
| Re-entrant SPIN_VFS in vfs_mount | **Fixed** | Release lock before `ops->mount()` callback |
