# Phase 6 Step 5 — Syscall Gap Analysis

Analysis of syscalls required by busybox 1.36.1 + musl libc vs what PPAP
currently implements.  Guides Steps 7-9 (syscall implementation).

**Binary analysed:** `build/busybox/busybox` (ARMv6S-M Thumb, static, musl,
hush shell + 21 applets, 794 KB ELF)

**Method:** `arm-none-eabi-objdump -d` SVC extraction (93 SVC sites, 57
unique syscall numbers) + musl source code audit of internal paths.

---

## Critical: musl Syscall Number Remapping

musl's `src/internal/syscall.h` silently remaps legacy ARM syscall numbers
to `*64` / `*32` variants.  Five of PPAP's existing 24 syscalls will
**never be reached** by busybox+musl:

| musl calls | # | Instead of | # | Impact |
|---|---|---|---|---|
| `stat64` | 195 | `stat` | 106 | **SYS_STAT(106) dead** |
| `fstat64` | 197 | `fstat` | 108 | **SYS_FSTAT(108) dead** |
| `getdents64` | 217 | `getdents` | 141 | **SYS_GETDENTS(141) dead** |
| `wait4` | 114 | `waitpid` | 7 | **SYS_WAITPID(7) dead** |
| `rt_sigaction` | 174 | `sigaction` | 67 | **SYS_SIGACTION(67) dead** |
| `_llseek` | 140 | `lseek` | 19 | **SYS_LSEEK(19) dead** |
| `fork` | 2 | `vfork` | 190 | **SYS_VFORK(190) dead** (musl's vfork calls SYS_fork) |
| `rt_sigreturn` | 173 | `sigreturn` | 119 | **SYS_SIGRETURN(119) dead** |

Additionally, `struct stat` layout is incompatible: PPAP's is 16 bytes
(4 uint32_t fields), musl expects 88 bytes (Linux ARM `struct stat`).
`struct dirent` is also incompatible (fixed-size vs variable-length `dirent64`).

---

## Complete Syscall Matrix

### Already Implemented — Called by busybox+musl as-is

| # | Name | PPAP module | Notes |
|---|------|-------------|-------|
| 1 | `exit` | sys_proc.c | musl's `_Exit` fallback after `exit_group` |
| 3 | `read` | sys_io.c | Via `__syscall_cp_c` (cancelation point) |
| 4 | `write` | sys_io.c | Via `__syscall_cp_c` |
| 5 | `open` | sys_fs.c | Direct |
| 6 | `close` | sys_fs.c | Direct |
| 10 | `unlink` | sys_fs.c | `rm` applet |
| 11 | `execve` | sys_proc.c | hush command execution |
| 12 | `chdir` | sys_fs.c | hush `cd` builtin |
| 20 | `getpid` | sys_proc.c | Process management |
| 37 | `kill` | signal.c | `kill` applet, hush signal delivery |
| 39 | `mkdir` | sys_fs.c | `mkdir` applet |
| 41 | `dup` | sys_fs.c | hush I/O setup |
| 42 | `pipe` | pipe.c | hush pipelines |
| 45 | `brk` | sys_mem.c | musl malloc primary path |
| 63 | `dup2` | sys_fs.c | hush I/O redirection |
| 183 | `getcwd` | sys_fs.c | hush prompt, pwd |

**16 syscalls reachable.  8 existing syscalls dead (see remapping above).**

### Already Implemented — Dead (musl remaps to different numbers)

These still serve PPAP's own test programs but won't be called by busybox:

| Old # | Name | Replacement # | Replacement Name |
|---|---|---|---|
| 7 | `waitpid` | 114 | `wait4` |
| 19 | `lseek` | 140 | `_llseek` |
| 67 | `sigaction` | 174 | `rt_sigaction` |
| 106 | `stat` | 195 | `stat64` |
| 108 | `fstat` | 197 | `fstat64` |
| 119 | `sigreturn` | 173 | `rt_sigreturn` |
| 141 | `getdents` | 217 | `getdents64` |
| 162 | `nanosleep` | 265 | `clock_nanosleep_time32` |
| 190 | `vfork` | 2 | `fork` |

---

## P0 — Must Implement for Boot

These are called during musl libc startup, first `printf`, and basic
process lifecycle.  Without them busybox cannot even print "hello".

| # | Name | Source | Implementation |
|---|------|--------|----------------|
| 248 | `exit_group` | musl `_Exit()` | Alias → `sys_exit()` (single-threaded, identical) |
| 256 | `set_tid_address` | musl `__init_tp()` | Store ptr in PCB, return `getpid()` (3 lines) |
| 192 | `mmap2` | musl malloc, TLS init | Anonymous-only: allocate pages from page pool |
| 91 | `munmap` | musl malloc, cleanup | Free mmap'd pages back to pool |
| 146 | `writev` | musl `__stdio_write()` | Loop iovec[], call `sys_write` per segment |
| 145 | `readv` | musl `__stdio_read()` | Loop iovec[], call `sys_read` per segment |
| 175 | `rt_sigprocmask` | musl fork, sigaction | Manipulate `pcb->sig_blocked` bitmask |
| 174 | `rt_sigaction` | musl `sigaction()` | Parse `struct k_sigaction`; install handler |
| 173 | `rt_sigreturn` | musl signal trampoline | Like `sys_sigreturn(119)` but for rt_sigaction |
| 54 | `ioctl` | musl `__stdout_write` | Dispatch to `file_ops->ioctl`; tty: TCGETS, TIOCGWINSZ |
| 221 | `fcntl64` | musl `fdopen`, hush pipes | F_GETFD, F_SETFD, F_GETFL, F_SETFL, F_DUPFD |
| 195 | `stat64` | musl `stat()` | Same logic as `sys_stat` but fill Linux 88-byte struct |
| 197 | `fstat64` | musl `fstat()` | Same logic as `sys_fstat` but fill Linux 88-byte struct |
| 217 | `getdents64` | musl `readdir()` | Produce variable-length `struct dirent64` records |
| 114 | `wait4` | musl `waitpid()` | Extend `sys_waitpid`: 4th arg `rusage` (ignore if NULL) |
| 140 | `_llseek` | musl `lseek()` | 5-arg: `(fd, off_hi, off_lo, result_ptr, whence)` |
| 2 | `fork` | musl `vfork()`, `_Fork()` | Map → `sys_vfork()` (NOMMU: fork=vfork) |
| 122 | `uname` | musl, busybox `uname` | Fill `struct utsname` with PPAP system info |
| 199 | `getuid32` | musl startup | Return 0 (root) |
| 200 | `getgid32` | musl startup | Return 0 |
| 201 | `geteuid32` | musl startup | Return 0 |
| 202 | `getegid32` | musl startup | Return 0 |
| 263 | `clock_gettime32` | musl `clock_gettime()` | Return SysTick-based monotonic time |
| 403 | `clock_gettime64` | musl time64 first-try | Same impl, 64-bit timespec output |

**24 new syscalls (P0).**

---

## P1 — Must Implement for Hush Interactive Shell

Needed after boot for interactive shell use (command lookup, job control,
prompt display).

| # | Name | Source | Implementation |
|---|------|--------|----------------|
| 33 | `access` | hush command lookup, `test -e` | `vfs_lookup` + check existence (root=always OK) |
| 85 | `readlink` | `/proc/self/exe`, symlink resolution | VFS `readlink` op |
| 196 | `lstat64` | `ls -l` (symlink detection) | `vfs_lookup(NOFOLLOW)` + stat64 fill |
| 64 | `getppid` | hush process management | Return `pcb->ppid` |
| 57 | `setpgid` | hush job control | Set `pcb->pgid` |
| 66 | `setsid` | init session creation | Set `pcb->sid = pcb->pid` |
| 60 | `umask` | file creation mode | Store/return `pcb->umask` |
| 40 | `rmdir` | `rmdir` applet | VFS unlink for directories |
| 120 | `clone` | musl `_Fork()` fallback | Map → `sys_vfork()` when `flags=SIGCHLD, stack=0` |
| 240 | `futex` | musl thread locks | Return 0 (single-threaded, locks are NOPs) |
| 78 | `gettimeofday` | musl time functions | Convert SysTick to `struct timeval` |
| 163 | `mremap` | musl malloc (region resize) | Return -ENOMEM (force malloc to mmap new region) |
| 327 | `fstatat64` | musl `stat`/`fstat` variants | Resolve `dirfd` + path → `sys_stat64` |
| 265 | `clock_nanosleep32` | musl `nanosleep()` | Sleep with clock spec |
| 407 | `clock_nanosleep64` | musl time64 first-try | Same impl, 64-bit timespec |

**15 new syscalls (P1).**

---

## P2 — Nice to Have (Applet Completeness)

Individual applet features.  Can be stubs initially.

| # | Name | Source | Implementation |
|---|------|--------|----------------|
| 9 | `link` | `ln` applet (hard links) | VFS `link` op |
| 83 | `symlink` | `ln -s` applet | VFS `symlink` op |
| 38 | `rename` | `mv` applet | VFS same-FS rename |
| 15 | `chmod` | `chmod` applet | VFS `chmod` op (store mode) |
| 94 | `fchmod` | `chmod` via fd | Same as chmod on vnode |
| 14 | `mknod` | devfs (stub) | Return -EPERM |
| 198 | `lchown32` | Ownership (stub) | Return 0 (single-user) |
| 207 | `fchown32` | Ownership (stub) | Return 0 |
| 212 | `chown32` | Ownership (stub) | Return 0 |
| 206 | `setgroups32` | Group mgmt (stub) | Return 0 |
| 125 | `mprotect` | musl rare path | Return 0 (no MMU) |
| 128 | `init_module` | Never called (dead code) | Return -ENOSYS |
| 167 | (undefined) | Dead code or false positive | Return -ENOSYS |
| 269 | `utimes` | Timestamp update | Return 0 (stub, no RTC) |
| 281 | `socket` | Never called (no networking) | Return -ENOSYS |
| 345 | `getcpu` | musl rare path | Return 0 (single-core) |
| 397 | `statx` | musl stat fallback | Return -ENOSYS (stat64 suffices) |

**17 new syscalls (P2).**

---

## struct stat Migration Plan

PPAP's current layout (`src/kernel/vfs/vfs.h:57-62`):
```c
struct stat {          /* 16 bytes */
    uint32_t st_ino, st_mode, st_nlink, st_size;
};
```

musl's expected layout (`musl/arch/arm/bits/stat.h`):
```c
struct stat {          /* ~88 bytes */
    dev_t st_dev;               /* +0  */
    int __st_dev_padding;       /* +8  */
    long __st_ino_truncated;    /* +12 */
    mode_t st_mode;             /* +16 */
    nlink_t st_nlink;           /* +20 */
    uid_t st_uid;               /* +24 */
    gid_t st_gid;               /* +28 */
    dev_t st_rdev;              /* +32 */
    int __st_rdev_padding;      /* +40 */
    off_t st_size;              /* +44: 64-bit */
    blksize_t st_blksize;       /* +52 */
    blkcnt_t st_blocks;         /* +56: 64-bit */
    struct { long tv_sec, tv_nsec; } __st_atim32, __st_mtim32, __st_ctim32;
    ino_t st_ino;               /* 64-bit */
    struct timespec st_atim, st_mtim, st_ctim;
};
```

**Strategy:**
- Keep internal VFS `struct stat` small (rename to `vfs_stat_t` or similar)
- `sys_stat64()` / `sys_fstat64()` call VFS stat, then translate to the
  Linux-compatible 88-byte layout for userspace
- Zero-fill fields PPAP doesn't track (uid, gid, rdev, times)
- Use `_Static_assert(sizeof(...))` to verify layout matches

## struct dirent64 Migration Plan

PPAP's current layout:
```c
struct dirent {        /* fixed-size */
    uint32_t d_ino;
    uint8_t  d_type;
    char     d_name[VFS_NAME_MAX + 1];
};
```

Linux `dirent64` (variable-length):
```c
struct dirent64 {
    uint64_t d_ino;
    int64_t  d_off;     /* next cookie */
    uint16_t d_reclen;  /* total record length */
    uint8_t  d_type;
    char     d_name[];  /* NUL-terminated, padded to align */
};
```

**Strategy:** `sys_getdents64()` calls VFS readdir internally, then packs
entries into the variable-length Linux format in the user buffer.

---

## _llseek (140) Interface

musl's `lseek()` on 32-bit ARM uses `_llseek` with 5 arguments:
```c
_llseek(fd, offset_hi, offset_lo, &result, whence)
```
where `result` is a `loff_t *` (pointer to 64-bit offset output).
Implementation: combine `offset_hi:offset_lo` (PPAP files are small, hi
is always 0), call the existing seek logic, write result to `*result`.

---

## rt_sigaction (174) Interface

musl's `sigaction()` calls `rt_sigaction(sig, act, oact, sigsetsize)`:
```c
struct k_sigaction {
    void (*handler)(int);    /* or sa_sigaction */
    unsigned long sa_flags;  /* SA_RESTART, SA_SIGINFO, SA_RESTORER, ... */
    void (*sa_restorer)(void);  /* musl provides __restore_rt */
    unsigned long sa_mask[2];   /* blocked signals during handler */
};
```
4th argument `sigsetsize` = `_NSIG/8` (= 8 on ARM).

Current `sys_sigaction(67)` takes `(sig, handler, old_ptr)` — much simpler.
New `sys_rt_sigaction(174)` must parse `k_sigaction` struct properly.

---

## Summary

| Category | Existing (reachable) | New P0 | New P1 | New P2 | Total |
|---|---|---|---|---|---|
| Syscalls | 16 | 24 | 15 | 17 | 72 |

Steps 7-9 implement P0 first, then P1, then P2 as needed.
