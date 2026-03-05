# PPAP procfs Reference

This document describes the virtual files provided by the PPAP `/proc` filesystem.
procfs is mounted read-only at `/proc` during kernel boot.

Implementation: `src/kernel/fs/procfs.c`

---

## System-wide Files

### `/proc/meminfo`

Reports physical memory statistics.

```
MemTotal:     264 kB
MemFree:      132 kB
PageSize:    4096 B
```

| Field      | Description                                      |
|------------|--------------------------------------------------|
| MemTotal   | Total allocatable pages x PAGE_SIZE, in kB       |
| MemFree    | Free pages x PAGE_SIZE, in kB                    |
| PageSize   | Page allocator granularity in bytes (always 4096) |

Source: `page_free_count()` and `PAGE_COUNT` from `mm/page.c`.

---

### `/proc/version`

Single-line kernel version string.

```
PicoPiAndPortable v0.3 (armv6m)
```

Hardcoded; no dynamic content.

---

### `/proc/stat`

Aggregate and per-core CPU tick counters in Linux `/proc/stat` format.

```
cpu  120 0 45 800 0 0 0 0 0 0
cpu0 60 0 22 400 0 0 0 0 0 0
cpu1 60 0 23 400 0 0 0 0 0 0
```

Fields (space-separated after the label):

| # | Name      | Description                             |
|---|-----------|-----------------------------------------|
| 1 | user      | Ticks spent in user mode                |
| 2 | nice      | Always 0 (not tracked)                  |
| 3 | system    | Ticks spent in kernel mode              |
| 4 | idle      | Ticks with no runnable process          |
| 5–10 | —      | Always 0 (iowait, irq, softirq, etc.)  |

The `cpu` line is the sum of all cores.
`cpu0` and `cpu1` lines give per-core breakdowns (added in Phase 9).

Tick rate: `PPAP_TICK_HZ` (100 Hz, i.e., 10 ms per tick).

User vs. system distinction: `SysTick_Handler` checks EXC_RETURN bit 3 (captured
from LR via a naked assembly wrapper) to determine whether the interrupted
context was Thread mode (user, bit 3 = 1) or Handler mode (system, bit 3 = 0).
Note: `ICSR.RETTOBASE` (bit 11) is RAZ on ARMv6-M / Cortex-M0+, so it cannot
be used for this purpose.

Idle detection: processes with `pcb_t.is_idle == 1` (Thread 0 "kernel" on Core 0,
"idle1" on Core 1) have their ticks counted as idle regardless of EXC_RETURN.

Source: `cpu_user_ticks[]`, `cpu_system_ticks[]`, `cpu_idle_ticks[]` in `proc/sched.c`.

---

### `/proc/uptime`

System uptime and cumulative idle time in seconds with centisecond precision.

```
12.34 56.78
```

| Field | Description                                          |
|-------|------------------------------------------------------|
| 1     | Seconds since boot (total ticks / PPAP_TICK_HZ)     |
| 2     | Aggregate idle seconds across all cores              |

Source: `sched_get_ticks()` for uptime; sum of `cpu_idle_ticks[0..1]` for idle.

---

### `/proc/mounts`

Lists mounted filesystems, one per line.

```
romfs / romfs ro 0 0
devfs /dev devfs rw 0 0
procfs /proc procfs ro 0 0
tmpfs /tmp tmpfs rw 0 0
```

Fields: `<fstype> <mountpoint> <fstype> <ro|rw> 0 0`

Source: iterates `vfs_mount_table[]`.

---

## Per-process Files

Each running process gets a `/proc/<pid>/` directory containing the files below.
Only processes that are not in `PROC_FREE` state are listed.

### `/proc/<pid>/stat`

Process status in Linux 52-field format (one line). This is the format expected
by busybox `ps` and `top`.

```
1 (init) S 0 1 1 0 -1 4194304 0 0 0 0 45 67 0 0 20 0 1 0 100 8192 2 0 0 ...
```

Key fields used by PPAP:

| # | Name       | Description                                       |
|---|------------|---------------------------------------------------|
| 1 | pid        | Process ID                                        |
| 2 | comm       | Command name in parentheses, from `pcb->comm`     |
| 3 | state      | R = runnable, S = sleeping/blocked, Z = zombie    |
| 4 | ppid       | Parent PID                                        |
| 5 | pgrp       | Process group ID (`pcb->pgid`)                    |
| 6 | session    | Session ID (`pcb->sid`)                           |
| 7 | tty_nr     | Always 0 (no TTY tracking)                        |
| 14 | utime     | User-mode ticks (`pcb->utime`)                    |
| 15 | stime     | System-mode ticks (`pcb->stime`)                  |
| 22 | starttime | Boot tick when process was created (`pcb->start_time`) |
| 23 | vsize     | Virtual memory size in bytes (see below)           |
| 24 | rss       | Resident pages (= vsize / PAGE_SIZE, no swap)     |

All other fields are 0.

**VSZ calculation** (`proc_vsz()`):
Counts pages from three sources:
- `pcb->stack_page` (1 page if allocated)
- `pcb->user_pages[0..USER_PAGES_MAX-1]` (data/text segment pages)
- `pcb->mmap_regions[0..MMAP_REGIONS_MAX-1]` (mmap'd regions by page count)

Result = total pages x PAGE_SIZE.

---

### `/proc/<pid>/cmdline`

NUL-terminated command name string.

```
init\0
```

Source: `pcb->comm[16]` (basename of the executable).

Note: this is a simplified form; full Linux cmdline would contain the entire
`argv[]` array with NUL separators.

---

## Internals

### Inode numbering

| Range | Assignment |
|-------|------------|
| 1 .. N | Static nodes (meminfo, version, stat, uptime, mounts) |
| 0x1000 + pid*16 | PID directory |
| 0x1000 + pid*16 + 1 | PID/stat |
| 0x1000 + pid*16 + 2 | PID/cmdline |

### VNode encoding

Per-PID file vnodes encode the process slot and sub-file index in `fs_priv`:
`(void*)(0x80000000 | (slot << 8) | sub_index)` where sub_index 1 = stat, 2 = cmdline.

---

## Future Additions

Planned but not yet implemented:

- **`/proc/self`** — symlink to `/proc/<current_pid>/` (requires `readlink` syscall)
- **`/proc/<pid>/status`** — human-readable process status
- **`/proc/<pid>/maps`** — process memory map