# Process Lifecycle: Zombies, Orphans, and Reaping

How PPAP manages process termination, zombie state, orphan reparenting,
and the interaction with vfork/exec.

Related issue: [#21 — Process lifecycle and zombie handling](https://github.com/toyoshim-i/PiPAPo/issues/21)

---

## 1. Process States

```
PROC_FREE → PROC_RUNNABLE → PROC_BLOCKED → PROC_ZOMBIE → PROC_FREE
              ↑       ↓         ↑      ↓
              └───────┘         └──────┘
                yield         waitpid/vfork
```

| State | Value | Meaning |
|-------|-------|---------|
| `PROC_FREE` | 0 | Slot available for `proc_alloc()` |
| `PROC_RUNNABLE` | 1 | Ready to run or currently executing |
| `PROC_SLEEPING` | 2 | Blocked until `sleep_until` tick count |
| `PROC_BLOCKED` | 3 | Blocked on vfork, waitpid, or I/O |
| `PROC_ZOMBIE` | 4 | Exited; waiting for parent to call `waitpid()` |

## 2. Normal Exit Flow

When a process calls `_exit(status)` (or musl's `exit()` → `_Exit()`):

```
Process calls _exit(status)
    │
    ▼
sys_exit(status)
    ├── Subsystem cleanup (on_exit hook)
    ├── Close all file descriptors (fd_close_all)
    ├── Free user_pages[] (text+data SRAM pages)
    ├── Unblock vfork parent (if applicable)
    ├── Wake parent (if blocked in waitpid)
    ├── Reparent children to init (PID 1)
    ├── Set state = PROC_ZOMBIE
    └── sched_yield() → process never runs again
```

**Key point**: The stack_page is NOT freed in `sys_exit`. It remains
allocated until the parent calls `waitpid()` to reap the zombie. This
is because the stack might still be referenced by the trap frame during
the context switch away from the zombie.

## 3. Reaping (waitpid)

When a parent calls `waitpid(pid, &status, options)`:

```
Parent calls waitpid(-1, &status, 0)
    │
    ▼
sys_waitpid scans proc_table for matching child
    │
    ├── Found ZOMBIE child:
    │   ├── Copy exit_status to *status
    │   ├── Free zombie's stack_page
    │   ├── proc_free(zombie) → state = PROC_FREE
    │   └── Return child PID
    │
    ├── Found running/sleeping child (no zombie yet):
    │   ├── Set parent state = PROC_BLOCKED
    │   ├── sched_yield() → parent sleeps
    │   └── (woken when child exits → re-scan)
    │
    ├── WNOHANG set and no zombie:
    │   └── Return 0 immediately
    │
    └── No matching children:
        └── Return -ECHILD
```

## 4. Orphan Reparenting

When a parent exits while children are still alive:

```
Parent calls sys_exit()
    │
    ▼
For each child where child->ppid == current->pid:
    ├── child->ppid = 1  (reparent to init)
    │
    └── If child is already ZOMBIE:
        └── Wake init (PID 1) if it's blocked in waitpid
```

**Init's role**: PID 1 (init) runs a `waitpid(-1, &status, 0)` loop
that reaps all zombie children. When orphaned zombies are reparented
to init, init wakes up and reaps them.

If init itself exits, the kernel enters the idle loop with no process
to reap orphans. Orphaned zombies become permanent leaks (slots never
freed). This is a design limitation — init must not exit.

## 5. Vfork + Exec Interaction

Vfork creates a child that shares the parent's address space until
the child calls `execve()` or `_exit()`.

```
Shell calls vfork()
    │
    ├── sys_vfork:
    │   ├── Allocate child PCB + stack page
    │   ├── Copy parent's stack page to child
    │   ├── Copy parent's user_pages to child (shared, not duplicated)
    │   ├── Set child->vfork_parent = parent
    │   ├── Parent state = PROC_BLOCKED
    │   └── Child state = PROC_RUNNABLE
    │
    ├── Child runs:
    │   ├── execve("/bin/cat", ...)
    │   │   ├── execve: allocate NEW stack + user_pages for cat
    │   │   ├── Free old_stack (vfork-allocated copy)
    │   │   ├── If vfork child: don't free old_user_pages (parent owns them)
    │   │   └── exec_pending = 1 → trap return reloads SP
    │   │
    │   └── OR _exit(127) if exec fails
    │       └── Unblocks vfork parent
    │
    └── Parent resumes after child execs or exits
        └── waitpid() loop to reap children
```

**Page ownership during vfork**:
- `vfork_parent != NULL` → child does NOT own user_pages
- On `_exit()`: only free pages the child allocated (not shared parent pages)
- On `execve()`: `owns_pages = (vfork_parent == NULL)` determines cleanup

## 6. Memory Freed at Each Stage

| Stage | What is freed | By whom |
|-------|--------------|---------|
| `sys_exit()` | user_pages[], mmap regions, fds | The exiting process |
| `sys_exit()` | user_stack_page (m68k only) | The exiting process |
| `sys_waitpid()` | stack_page | The parent (reaper) |
| `sys_waitpid()` | PCB slot (proc_free) | The parent |
| `sys_execve()` | old stack_page, old user_pages (if owned) | The exec'd process |

## 7. Musl Double-Exit Issue (RISC-V)

Musl's `_Exit()` calls `SYS_exit_group` first, then `SYS_exit` in an
infinite loop:

```c
void _Exit(int ec) {
    __syscall(SYS_exit_group, ec);
    for (;;) __syscall(SYS_exit, ec);
}
```

On ARM, the first `sys_exit` triggers PendSV which context-switches
away from the zombie before the loop continues. On RISC-V, the ecall
return path may re-execute the loop before the context switch happens,
calling `sys_exit` a second time on the zombie.

**Mitigation**: `sys_exit` checks `current->state == PROC_ZOMBIE` at
entry and yields immediately without re-freeing pages.

## 8. How Other OSes Handle This

### Linux

Linux has full `fork()` with copy-on-write (COW) page tables. When a
process forks, the child gets a complete virtual address space that
initially shares physical pages with the parent. Pages are only copied
when either process writes to them.

Zombie handling is similar to PPAP: exited processes become zombies
until `waitpid()`. Orphans are reparented to PID 1 (init or systemd's
subreaper). Linux also sends `SIGCHLD` to the parent on child exit,
and supports `SA_NOCLDWAIT` to auto-reap children without zombies.

Linux threads share the same address space and are managed by the
kernel's task scheduler. `exit_group()` terminates all threads in a
thread group simultaneously. PPAP has no threads — each process is
single-threaded.

### BSD / macOS

Similar to Linux but with different internals. BSD uses `wait4()` as
the primary wait syscall and has more sophisticated process group and
session management. macOS adds `launchd` as PID 1, which handles
orphan reaping with additional service management logic.

### Windows

Windows has no zombie concept. When a process exits, its handle remains
valid until all references are closed (reference counting, not parent
reaping). There is no reparenting — any handle holder can query exit
status. The "zombie" equivalent is a process object with zero threads
but an open handle.

### Embedded RTOSes (FreeRTOS, Zephyr)

Most RTOSes don't have a UNIX process model. Tasks are created and
deleted explicitly. There is no fork/exec, no parent-child hierarchy,
no zombie state, and no waitpid. Task cleanup is immediate on deletion.

PPAP is unusual among bare-metal systems in implementing full UNIX
process semantics (vfork, execve, waitpid, zombie/orphan handling)
on microcontrollers with no MMU.

### Key PPAP Differences from Linux

| Aspect | Linux | PPAP |
|--------|-------|------|
| `fork()` | COW page tables | Not available — use `vfork()` only |
| Address space | Virtual (MMU) | Physical (no MMU, no page tables) |
| Zombie cleanup | `SIGCHLD` + `waitpid` | Direct parent wake + `waitpid` |
| `SA_NOCLDWAIT` | Auto-reap children | Not supported |
| Thread groups | `clone()` + `exit_group()` | Single-threaded only |
| PID namespace | Hierarchical namespaces | Flat 8-slot proc_table |
| Memory isolation | Per-process page tables | PMP regions (ARM MPU) or none (RISC-V) |
| OOM killer | Selects victim process | `page_alloc` returns NULL |

## 9. PPAP vs POSIX Differences

| Aspect | POSIX | PPAP |
|--------|-------|------|
| `fork()` | Full address space copy | Not implemented (use `vfork`) |
| `vfork()` | Parent suspended until child execs/exits | Same |
| Zombie reaping | Parent must call `wait*()` | Same |
| Orphan reparenting | Reparent to PID 1 (init) | Same |
| `SIGCHLD` | Sent to parent on child exit | Not sent (parent woken directly) |
| Process groups | Full job control | Basic `pgid`/`sid` tracking |
| Thread support | `pthread_create` etc. | Not implemented (single-threaded) |

## 9. Debugging Tips

- **Zombie leak**: if `ps` shows processes stuck in state 4, the parent
  isn't calling `waitpid()`. Check if the parent is alive and in its
  wait loop.

- **Double-free on exit**: check if musl-linked binaries trigger the
  double-exit path. The zombie guard should prevent it, but verify with
  `trace --ppap /bin/command`.

- **Orphan accumulation**: if init isn't running or isn't calling
  `waitpid(-1, ...)`, orphaned zombies accumulate. Check init's state
  with OpenOCD or the `ps` command.
