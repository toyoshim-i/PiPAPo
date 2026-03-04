# Phase 10: Stabilization — Detailed Plan

**PicoPiAndPortable Development Roadmap**
Estimated Duration: 3 weeks
Prerequisites: Phase 9 (Dual-Core Scheduling) complete
**Status: Not started**

---

## Context

With dual-core scheduling in place (Phase 9), this phase hardens the kernel
for reliable interactive use.  Focus areas: process lifecycle bugs, OOM
visibility, signal/buffer safety, filesystem correctness, and input validation.

## Design Decisions

**OOM handling:** No OOM killer.  On a device with 51 pages and max 8 trusted
processes, the operator controls what runs.  When `page_alloc()` fails, print
UART warning and return NULL.  `/proc/meminfo` reports `OomCount`.

**Spinlock-aware fixes:** All fixes in this phase must respect the spinlock
discipline established in Phase 9.  Proc_table modifications acquire SPIN_PROC,
FS operations are already serialized by SPIN_FS.

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

### ☐ Step 1 — Orphan reparenting to init

**Problem:** When a parent exits, children become unreachable zombies.  Stack
pages leak permanently.

**File:** `src/kernel/syscall/sys_proc.c` — modify `sys_exit()`

After "wake parent" block, before `current->state = PROC_ZOMBIE`, scan
proc_table (under SPIN_PROC) and reparent children whose `ppid == current->pid`
to PID 1.  If reparented child is already zombie, wake init.

### ☐ Step 2 — Kernel thread stack NULL check

**File:** `src/kernel/main.c`

Check `page_alloc()` return for Thread 0 stack.  Panic + `wfi` on failure.

### ☐ Step 3 — Init load failure halts scheduler

**File:** `src/kernel/main.c`

If both init and /bin/sh fail to exec, `proc_free(init)` then halt with `wfi`.

### ☐ Step 4 — Fix page_free() silent drop

**File:** `src/kernel/mm/page.c`

Add double-free detection (scan free_stack under SPIN_PAGE) and page-alignment
check.  O(51) at 133 MHz ≈ 1 µs.

### ☐ Step 5 — OOM warning on page_alloc failure

**File:** `src/kernel/mm/page.c`

When `free_top == 0`, increment `oom_count` and print UART warning.

### ☐ Step 6 — Document stack page lifecycle

**File:** `src/kernel/syscall/sys_proc.c`

Comment: user_pages freed in sys_exit(), stack_page freed in sys_waitpid().
With Step 1's reparenting, init always reaps orphans.

---

## Week 2 — Defensive Checks and FS Correctness

### ☐ Step 7 — Signal handler stack overflow check

**File:** `src/kernel/signal/signal.c`

Bounds-check `new_psp >= stack_page` before writing signal frame.

### ☐ Step 8 — Fix execve fd_close_all ordering

**Files:** `src/kernel/syscall/sys_proc.c`, `src/kernel/exec/exec.c`

Move `fd_close_all()` to after successful `do_execve()`.  On failure, fds
untouched (POSIX-correct).

### ☐ Step 9 — TTY line buffer overflow prevention

**File:** `src/kernel/fd/tty.c`

When `line_pos == LINE_BUF_SIZE`, auto-complete line and ring bell.

### ☐ Step 10 — romfs sibling chain cycle detection

**File:** `src/kernel/fs/romfs.c`

Add iteration limit (1024) to `romfs_lookup()` and `romfs_readdir()`.

### ☐ Step 11 — ELF metadata bounds validation

**Files:** `src/kernel/exec/elf.c`, `src/kernel/exec/exec.c`

Validate `e_phentsize`, `e_phnum`, segment offsets within file size.

### ☐ Step 12 — VFAT BPB validation

**File:** `src/kernel/fs/vfat.c`

Validate `num_fats`, `reserved_sectors`, `fat_size_32`, `data_start_sector`.

### ☐ Step 13 — VFAT directory entry cluster field update

**File:** `src/kernel/fs/vfat.c`

Fix TODO at line 535: write `first_cluster_hi/lo` to directory entry after
allocating a cluster for an empty file.

### ☐ Step 14 — UFS symlink buffer overflow fix

**File:** `src/kernel/fs/ufs.c`

Cap `len` to `UFS_FAST_SYMLINK_MAX` in fast symlink path.

### ☐ Step 15 — O_TRUNC failure check

**File:** `src/kernel/syscall/sys_fs.c`

Check `truncate()` return value; propagate error.

---

## Week 3 — Performance, Tests, and Polish

### ☐ Step 16 — Path resolution stack reduction

**File:** `src/kernel/vfs/namei.c`

Reduce symlink recursion limit from 8 to 4.  Eliminate one 128-byte buffer.

### ☐ Step 17 — procfs OOM count in /proc/meminfo

**File:** `src/kernel/fs/procfs.c`

Add `OomCount` line using `oom_count` from page.c.

### ☐ Step 18 — New test suites

**File:** `tests/kernel/ktest.c`

Orphan reparenting test, OOM ENOMEM test, signal stack overflow test.

### ☐ Step 19 — Documentation and version bump

**Files:**
- `src/kernel/fs/procfs.c` — bump version to "0.10.0"
- `src/kernel/syscall/sys_proc.c` — document orphan/zombie lifecycle

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
| `src/kernel/exec/exec.c` | Segment bounds validation; remove fd_stdio_init |
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
