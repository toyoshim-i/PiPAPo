# Phase 10: Stabilization — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 3 weeks
Prerequisites: Phase 9 (Dual-Core Scheduling) complete
**Status: Not started**

---

## Context

Phase 9 (Dual-Core Scheduling) is complete.  The kernel now runs both RP2040
cores with hardware spinlock protection:
- **SPIN_PAGE** guards `page_alloc`/`page_free`/`page_free_count`
- **SPIN_PROC** guards `proc_table` and scheduler state
- **SPIN_FS** serializes VFS + VFAT/UFS sector buffers (per-entry-point wrappers)
- **SPIN_VFS** guards vnode pool and mount table
- `core1_launch()` is deferred to after init gets PID 1 (commit b2a28cd)
- Per-core arrays: `svc_restart[core_id()]`, `exec_pending[core_id()]`

This phase hardens the kernel for reliable interactive use.  Focus areas:
process lifecycle bugs, OOM visibility, signal/buffer safety, filesystem
correctness, and input validation.

## Design Decisions

**OOM handling:** No OOM killer.  On a device with 51 pages and max 8 trusted
processes, the operator controls what runs.  When `page_alloc()` fails, print
UART warning and return NULL.  `/proc/meminfo` reports `OomCount`.

**Spinlock-aware fixes:** All fixes must respect the spinlock discipline now
operational.  Proc_table modifications acquire SPIN_PROC, page allocator
operations acquire SPIN_PAGE, FS operations are serialized by SPIN_FS wrappers.

## Exit Criteria

1. `scripts/qemu-test.sh` passes all existing + new tests
2. Free page count after 50 sequential shell commands returns to within
   1 page of baseline
3. OOM returns ENOMEM cleanly with UART warning
4. No UART "PANIC" during normal interactive use
5. `scripts/test_all_targets.sh` builds all three targets
6. VFAT file create-write-close-reopen cycle works on pico1calc SD card

---

## Week 1 — Process Lifecycle and Memory Safety

### ✓ Step 0 — Fix /proc/stat tick accounting

**Problem:** `/proc/stat` shows user=0 and idle=0; all ticks counted as system.

Two root causes:
1. **RETTOBASE (ICSR bit 11) is RAZ on Cortex-M0+** — the ARMv6-M architecture
   does not implement this field.  It always reads 0, so the user-tick branch
   was never taken.
2. **Idle threads are PROC_RUNNABLE** — proc_table[0] ("kernel") and Core 1's
   idle process ("idle1") have `state == PROC_RUNNABLE`, so they enter the
   running-process branch instead of the idle branch.

**Fix:**
- Replace RETTOBASE check with EXC_RETURN (LR) bit 3 in `SysTick_Handler`.
  Naked assembly wrapper captures EXC_RETURN before the C prologue clobbers it.
  Bit 3 = 1 → Thread mode (user), bit 3 = 0 → Handler mode (system).
- Add `pcb_t.is_idle` flag; set for proc_table[0] and Core 1 idle process.
  Ticks for idle threads go to `cpu_idle_ticks[]`.

**Files:** `src/kernel/proc/sched.c`, `src/kernel/proc/proc.h`,
`src/kernel/proc/proc.c`, `src/kernel/smp.c`

### ✓ Step 1 — Orphan reparenting to init

**Problem:** When a parent exits, children become unreachable zombies.  Stack
pages leak permanently.

**File:** `src/kernel/syscall/sys_proc.c` — modify `sys_exit()`

After "wake parent" block, before `current->state = PROC_ZOMBIE`, scan
proc_table (under SPIN_PROC) and reparent children whose `ppid == current->pid`
to PID 1.  If reparented child is already zombie, wake init.

### ✓ Step 2 — Kernel thread stack NULL check

**File:** `src/kernel/main.c`

Check `page_alloc()` return for Thread 0 stack.  Panic + `wfi` on failure.

### ✓ Step 3 — Init load failure halts scheduler

**File:** `src/kernel/main.c`

Currently prints "PANIC: no init or shell" but falls through to `core1_launch()`
and `sched_start()`.  Fix: if both init and /bin/sh fail to exec,
`proc_free(init)` then halt with `wfi` — must not reach `core1_launch()` or
`sched_start()`.

### ✓ Step 4 — Fix page_free() double-free detection

**File:** `src/kernel/mm/page.c`

Phase 9 added SPIN_PAGE protection and a range guard (addr < PAGE_POOL_BASE
check) to `page_free()`.  Remaining: add a duplicate scan of `free_stack` inside
the existing SPIN_PAGE critical section to detect double-free.
O(51) at 133 MHz ≈ 1 µs.

### ✓ Step 5 — OOM warning on page_alloc failure

**File:** `src/kernel/mm/page.c`

`page_alloc()` already returns NULL under SPIN_PAGE when `free_top == 0`.
Add: increment `oom_count` and print UART warning inside the existing critical
section.

### ✓ Step 6 — Document stack page lifecycle

**File:** `src/kernel/syscall/sys_proc.c`

Comment: user_pages freed in sys_exit(), stack_page freed in sys_waitpid().
With Step 1's reparenting, init always reaps orphans.

---

## Week 2 — Defensive Checks and FS Correctness

### ✓ Step 7 — Signal handler stack overflow check

**File:** `src/kernel/signal/signal.c`

Bounds-check `new_psp >= stack_page` before writing signal frame.

### ✓ Step 8 — Fix execve fd_close_all ordering

**Files:** `src/kernel/syscall/sys_proc.c`, `src/kernel/exec/exec.c`

Currently `sys_execve()` calls `fd_close_all()` before `do_execve()` and
restores via `fd_stdio_init()` on failure.  Also, `do_execve()` unconditionally
calls `fd_stdio_init(p)` at line 346.  Fix: move `fd_close_all()` to after
successful `do_execve()`, remove `fd_stdio_init()` from `exec.c`.  On failure,
fds untouched (POSIX-correct).

### ✓ Step 9 — TTY line buffer overflow prevention

**File:** `src/kernel/fd/tty.c`

When `line_pos == LINE_BUF_SIZE`, auto-complete line and ring bell.

### ✓ Step 10 — romfs sibling chain cycle detection

**File:** `src/kernel/fs/romfs.c`

Add iteration limit (1024) to `romfs_lookup()` and `romfs_readdir()`.

### ✓ Step 11 — ELF metadata bounds validation

**Files:** `src/kernel/exec/elf.c`, `src/kernel/exec/exec.c`

Validate `e_phentsize`, `e_phnum`, segment offsets within file size.

### ✓ Step 12 — VFAT BPB validation

**File:** `src/kernel/fs/vfat.c`

`vfat_mount()` already validates `bytes_per_sector == 512` and
`sectors_per_cluster != 0`.  Phase 9 added SPIN_FS wrappers around all entry
points.  Remaining: validate `num_fats`, `reserved_sectors`, `fat_size_32`,
`data_start_sector` for zero / overflow.

### ✓ Step 13 — VFAT directory entry cluster field update

**File:** `src/kernel/fs/vfat.c`

Fix TODO at line 536: write `first_cluster_hi/lo` to directory entry after
allocating a cluster for an empty file.

### ✓ Step 14 — UFS symlink buffer overflow fix

**File:** `src/kernel/fs/ufs.c`

Phase 9 added SPIN_FS wrappers around `ufs_readlink()`.  The underlying
`ufs_readlink()` still needs a fix: cap `len` to `UFS_FAST_SYMLINK_MAX` (40)
in the fast symlink path before `memcpy(buf, inode.i_direct, len)` to prevent
out-of-bounds read from a corrupted inode.

### ✓ Step 15 — O_TRUNC failure check

**File:** `src/kernel/syscall/sys_fs.c`

Check `truncate()` return value; propagate error.

---

## Week 3 — Performance, Tests, and Polish

### ✓ Step 16 — Path resolution stack reduction

**File:** `src/kernel/vfs/namei.c`

Reduce symlink recursion limit from 8 to 4.  Eliminate one 128-byte buffer.

### ✓ Step 17 — procfs OOM count in /proc/meminfo

**File:** `src/kernel/fs/procfs.c`

Add `OomCount` line using `oom_count` from page.c.

### ✓ Step 18 — New test suites

**File:** `tests/kernel/ktest.c`

Orphan reparenting test, OOM ENOMEM test, signal stack overflow test.

### ☐ Step 19 — Documentation and version bump

**Files:**
- `src/kernel/fs/procfs.c` — bump version to "0.10.0"
- `src/kernel/syscall/sys_proc.c` — document orphan/zombie lifecycle
- `docs/procfs.md` — update OomCount entry after Step 17

---

## Files Modified (Complete)

| File | Change |
|------|--------|
| `src/kernel/syscall/sys_proc.c` | Orphan reparenting; execve fd ordering fix |
| `src/kernel/main.c` | NULL check for thread 0 stack; halt on init failure |
| `src/kernel/mm/page.c` | Double-free detection; OOM warning; oom_count |
| `src/kernel/mm/page.h` | Declare oom_count |
| `src/kernel/signal/signal.c` | Stack overflow bounds check |
| `src/kernel/fd/tty.c` | Line buffer overflow fix |
| `src/kernel/fs/romfs.c` | Cycle detection limit |
| `src/kernel/exec/elf.c` | ELF header validation |
| `src/kernel/exec/exec.c` | Segment bounds validation; remove fd_stdio_init (line 346) |
| `src/kernel/fs/vfat.c` | BPB validation; cluster field TODO fix |
| `src/kernel/fs/ufs.c` | Fast symlink buffer cap |
| `src/kernel/syscall/sys_fs.c` | O_TRUNC check |
| `src/kernel/fs/procfs.c` | OOM count; version bump |
| `src/kernel/vfs/namei.c` | Reduce symlink depth; stack reduction |
| `tests/kernel/ktest.c` | New: orphan, OOM, signal tests |

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| Double-free scan slows page_free | Very low | O(51) at 133 MHz ≈ 1 µs |
| exec fd reorder breaks busybox | Low | POSIX-correct; busybox handles this |
| romfs iteration limit too low | Very low | 1024 >> any realistic directory |
| Stack reduction causes symlink failure | Low | 4 levels covers /bin/ls → busybox |

## Deferred to Phase 11+

| Issue | Reason |
|-------|--------|
| VFAT FAT dual-write atomicity | Power failure resilience; SD is removable |
| SA_RESTART for signal interrupts | Busybox handles EINTR correctly |
| mmap O(n²) scan | Only multi-page anonymous mmap; musl does single-page |
