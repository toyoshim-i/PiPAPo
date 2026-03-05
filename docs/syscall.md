# PPAP System Call Reference

This document describes every system call available in the PicoPiAndPortable
kernel, with usage details and notes on how each differs from POSIX / Linux.

---

## Calling Convention (ARM EABI)

PPAP uses the Linux ARM EABI syscall convention:

| Register | Purpose |
|----------|---------|
| `r7` | Syscall number |
| `r0`–`r3` | Arguments 1–4 |
| `r4` | Argument 5 |
| `r5` | Argument 6 |
| `r0` | Return value |

Invoke with `svc 0`.  On success the return value is zero or positive.
On error the return value is a negative errno (e.g. `-ENOENT`).

---

## Syscall Table

| # | Name | Signature |
|---|------|-----------|
| 1 | exit | `void exit(int status)` |
| 2 | fork | `pid_t fork(void)` |
| 3 | read | `ssize_t read(int fd, void *buf, size_t n)` |
| 4 | write | `ssize_t write(int fd, const void *buf, size_t n)` |
| 5 | open | `int open(const char *path, int flags, mode_t mode)` |
| 6 | close | `int close(int fd)` |
| 7 | waitpid | `pid_t waitpid(pid_t pid, int *status, int options)` |
| 10 | unlink | `int unlink(const char *path)` |
| 11 | execve | `int execve(const char *path, char *const argv[], char *const envp[])` |
| 12 | chdir | `int chdir(const char *path)` |
| 14 | mknod | `int mknod(const char *path, mode_t mode, dev_t dev)` |
| 15 | chmod | `int chmod(const char *path, mode_t mode)` |
| 19 | lseek | `off_t lseek(int fd, off_t off, int whence)` |
| 20 | getpid | `pid_t getpid(void)` |
| 21 | mount | `int mount(const char *src, const char *tgt, const char *fs, unsigned long flags, const void *data)` |
| 33 | access | `int access(const char *path, int mode)` |
| 37 | kill | `int kill(pid_t pid, int sig)` |
| 38 | rename | `int rename(const char *old, const char *new)` |
| 39 | mkdir | `int mkdir(const char *path, mode_t mode)` |
| 40 | rmdir | `int rmdir(const char *path)` |
| 41 | dup | `int dup(int oldfd)` |
| 42 | pipe | `int pipe(int fds[2])` |
| 45 | brk | `void *brk(void *addr)` |
| 52 | umount2 | `int umount2(const char *target, int flags)` |
| 54 | ioctl | `int ioctl(int fd, unsigned long cmd, ...)` |
| 57 | setpgid | `int setpgid(pid_t pid, pid_t pgid)` |
| 60 | umask | `mode_t umask(mode_t mask)` |
| 63 | dup2 | `int dup2(int oldfd, int newfd)` |
| 64 | getppid | `pid_t getppid(void)` |
| 66 | setsid | `pid_t setsid(void)` |
| 67 | sigaction | `int sigaction(int sig, uintptr_t handler, uintptr_t *old)` |
| 78 | gettimeofday | `int gettimeofday(struct timeval *tv, void *tz)` |
| 85 | readlink | `ssize_t readlink(const char *path, char *buf, size_t bufsiz)` |
| 91 | munmap | `int munmap(void *addr, size_t len)` |
| 106 | stat | `int stat(const char *path, struct stat *buf)` |
| 108 | fstat | `int fstat(int fd, struct stat *buf)` |
| 114 | wait4 | `pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru)` |
| 119 | sigreturn | `void sigreturn(void)` |
| 120 | clone | `pid_t clone(unsigned long flags, void *stack, ...)` |
| 122 | uname | `int uname(struct utsname *buf)` |
| 125 | mprotect | `int mprotect(void *addr, size_t len, int prot)` |
| 132 | getpgid | `pid_t getpgid(pid_t pid)` |
| 140 | _llseek | `int _llseek(int fd, long off_hi, long off_lo, loff_t *result, int whence)` |
| 141 | getdents | `int getdents(int fd, struct dirent *buf, size_t count)` |
| 145 | readv | `ssize_t readv(int fd, const struct iovec *iov, int iovcnt)` |
| 146 | writev | `ssize_t writev(int fd, const struct iovec *iov, int iovcnt)` |
| 162 | nanosleep | `int nanosleep(const struct timespec *req, struct timespec *rem)` |
| 163 | mremap | `void *mremap(void *addr, size_t old, size_t new, int flags)` |
| 173 | rt_sigreturn | `void rt_sigreturn(void)` |
| 174 | rt_sigaction | `int rt_sigaction(int sig, const struct sigaction *act, struct sigaction *oact, size_t sigsetsize)` |
| 175 | rt_sigprocmask | `int rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, size_t sigsetsize)` |
| 183 | getcwd | `char *getcwd(char *buf, size_t size)` |
| 190 | vfork | `pid_t vfork(void)` |
| 192 | mmap2 | `void *mmap2(void *addr, size_t len, int prot, int flags, int fd, off_t pgoff)` |
| 195 | stat64 | `int stat64(const char *path, struct stat64 *buf)` |
| 196 | lstat64 | `int lstat64(const char *path, struct stat64 *buf)` |
| 197 | fstat64 | `int fstat64(int fd, struct stat64 *buf)` |
| 198 | lchown32 | `int lchown(const char *path, uid_t uid, gid_t gid)` |
| 199 | getuid32 | `uid_t getuid(void)` |
| 200 | getgid32 | `gid_t getgid(void)` |
| 201 | geteuid32 | `uid_t geteuid(void)` |
| 202 | getegid32 | `gid_t getegid(void)` |
| 206 | setgroups32 | `int setgroups(int size, const gid_t *list)` |
| 207 | fchown32 | `int fchown(int fd, uid_t uid, gid_t gid)` |
| 212 | chown32 | `int chown(const char *path, uid_t uid, gid_t gid)` |
| 217 | getdents64 | `int getdents64(int fd, struct dirent64 *buf, size_t count)` |
| 221 | fcntl64 | `int fcntl(int fd, int cmd, ...)` |
| 240 | futex | `int futex(int *uaddr, int op, int val, ...)` |
| 248 | exit_group | `void exit_group(int status)` |
| 256 | set_tid_address | `pid_t set_tid_address(int *tidptr)` |
| 263 | clock_gettime | `int clock_gettime(clockid_t clk, struct timespec *tp)` |
| 265 | clock_nanosleep | `int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)` |
| 266 | statfs64 | `int statfs64(const char *path, size_t sz, struct statfs64 *buf)` |
| 267 | fstatfs64 | `int fstatfs64(int fd, size_t sz, struct statfs64 *buf)` |
| 269 | utimes | `int utimes(const char *path, const struct timeval tv[2])` |
| 322 | openat | `int openat(int dirfd, const char *path, int flags, mode_t mode)` |
| 327 | fstatat64 | `int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags)` |
| 336 | ppoll | `int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *timeout, const sigset_t *sigmask)` |
| 345 | getcpu | `int getcpu(unsigned *cpu, unsigned *node, void *unused)` |
| 397 | statx | `int statx(int dirfd, const char *path, int flags, unsigned mask, struct statx *buf)` |
| 403 | clock_gettime64 | `int clock_gettime64(clockid_t clk, struct timespec64 *tp)` |
| 407 | clock_nanosleep64 | `int clock_nanosleep64(clockid_t clk, int flags, const struct timespec64 *req, struct timespec64 *rem)` |
| 414 | ppoll_time64 | `int ppoll_time64(struct pollfd *fds, nfds_t n, const struct timespec64 *timeout, const sigset_t *sigmask)` |

---

## Detailed Descriptions

### Process Management

#### exit (1) / exit_group (248)

```c
void exit(int status);
void exit_group(int status);
```

Terminate the calling process.  Both numbers route to the same handler.

- Closes all open file descriptors.
- Frees user pages (unless the process is a vfork child sharing the parent's
  address space).
- Frees mmap regions.
- Unblocks the vfork parent if applicable.
- Wakes a parent blocked in `waitpid`.
- Reparents orphan children to init (PID 1).
- Marks the process `PROC_ZOMBIE` and yields.

**vs POSIX/Linux:**  Identical semantics.  `exit_group` is the same as `exit`
because PPAP is single-threaded (no thread groups).

---

#### fork (2) / clone (120) / vfork (190)

```c
pid_t fork(void);
pid_t vfork(void);
pid_t clone(unsigned long flags, void *stack, ...);
```

All three are routed to `sys_vfork`.  PPAP implements **vfork semantics only**:
the parent blocks until the child calls `execve` or `exit`.  The child shares
the parent's user pages.

`clone` accepts the fast-path `clone(SIGCHLD, 0)` that musl uses for `fork()`
and routes it to `sys_vfork`.

Returns the child PID in the parent and 0 in the child.

**vs POSIX/Linux:**
- Linux `fork` creates a fully independent copy (COW).  PPAP always uses
  vfork — the child **must not** modify shared memory before exec/exit.
- Linux `clone` supports thread creation with flags like `CLONE_VM`,
  `CLONE_THREAD`, etc.  PPAP only handles the `fork()`-equivalent fast path.

---

#### waitpid (7) / wait4 (114)

```c
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru);
```

Wait for a child process to exit.

- `pid > 0`: wait for that specific child.
- `pid == -1`: wait for any child.
- `options & WNOHANG`: return 0 immediately if no child has exited.

`wait4` ignores the `rusage` parameter and delegates to `waitpid`.

If children exist but none has exited, the caller blocks.  The blocking uses
the **svc_restart** mechanism: when the process is woken, SVC re-executes
the syscall with the original arguments.

**vs POSIX/Linux:**
- `pid == 0` (wait for same-PGID) and `pid < -1` (wait for specific PGID) are
  not implemented — only `pid > 0` and `pid == -1`.
- `rusage` is not filled in.
- The status word uses the standard `W_EXITCODE(status, 0)` encoding.

---

#### execve (11)

```c
int execve(const char *path, char *const argv[], char *const envp[]);
```

Replace the current process image with a new ELF binary.

- Loads the ELF from the VFS.
- Closes all open file descriptors and re-initializes stdio (fd 0/1/2).
- Frees old user pages and stack.
- Unblocks the vfork parent if applicable.
- On success, never returns — the new program begins execution.

`envp` is accepted but ignored (PPAP has no environment variable support).

**vs POSIX/Linux:**
- Only static PIE ELF binaries are supported (no dynamic linker, no `#!`
  scripts).
- Environment variables are not passed to the new process.
- `argv` is copied to user stack but the total size is limited by the 4 KB
  stack page.

---

#### getpid (20) / getppid (64)

```c
pid_t getpid(void);
pid_t getppid(void);
```

Return the process ID / parent process ID.  Identical to POSIX.

---

#### setpgid (57) / getpgid (132)

```c
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
```

Set or get the process group ID.  `pid == 0` means the calling process.

**vs POSIX/Linux:**  Simplified — no permission checks, no session-leader
restrictions.

---

#### setsid (66)

```c
pid_t setsid(void);
```

Create a new session.  Sets both `sid` and `pgid` to the caller's PID.
Returns the new session ID.

**vs POSIX/Linux:**  Always succeeds — no check for existing session leader.

---

#### set_tid_address (256)

```c
pid_t set_tid_address(int *tidptr);
```

Store `tidptr` in the PCB for thread library use.  Returns the caller's PID.

**vs POSIX/Linux:**  The kernel stores the pointer but never writes to it
(no thread support).  Exists to satisfy musl's startup sequence.

---

#### uname (122)

```c
int uname(struct utsname *buf);
```

Fill `buf` (390 bytes = 6 x 65-byte fields) with system identification:

| Field | Value |
|-------|-------|
| sysname | `PicoPiAndPortable` |
| nodename | `ppap` |
| release | `0.6.0` |
| version | `#1 PPAP` |
| machine | `armv6m` |
| domainname | (empty) |

**vs POSIX/Linux:**  Identical interface.  Values are hardcoded.

---

#### getcpu (345)

```c
int getcpu(unsigned *cpu, unsigned *node, void *unused);
```

Write the current CPU core number (0 or 1) to `*cpu`.  `node` is set to 0.

**vs Linux:**  Same interface.  Always returns core 0 or 1 (RP2040 dual-core).

---

### File I/O

#### read (3) / write (4)

```c
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
```

Read from or write to a file descriptor.  Dispatches through the file
operations vtable (`f->ops->read` / `f->ops->write`).

**vs POSIX/Linux:**  Identical interface.  Behaviour depends on the backing
driver (tty, VFS file, pipe, device file).

---

#### readv (145) / writev (146)

```c
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
```

Scatter/gather I/O.  Loops through the iovec array calling `read`/`write` for
each element.  `iovcnt` must be 1–1024.

**vs POSIX/Linux:**  Semantically identical.  Internally implemented as a loop
of single read/write calls (not truly atomic across iovecs).

---

#### open (5) / openat (322)

```c
int open(const char *path, int flags, mode_t mode);
int openat(int dirfd, const char *path, int flags, mode_t mode);
```

Open a file and return a file descriptor.

Supported flags:
- `O_CREAT` (0x0040) — create the file if it does not exist.
- `O_TRUNC` (0x0200) — truncate to zero length.
- `O_APPEND` (0x0400) — writes append to end of file.

TTY device paths (`/dev/ttyS0`, `/dev/console`, `/dev/tty`) are detected and
wired to the kernel tty driver directly.

`openat` accepts `AT_FDCWD` (-100) as `dirfd` and treats the path as
cwd-relative.  Other `dirfd` values are not supported.

**vs POSIX/Linux:**
- `O_RDONLY`/`O_WRONLY`/`O_RDWR` are accepted but not enforced at the VFS
  layer — the underlying FS driver decides read/write capability.
- `O_EXCL`, `O_NOCTTY`, `O_DIRECTORY`, `O_CLOEXEC` are not implemented.
- `openat` only supports `dirfd == AT_FDCWD` — arbitrary directory fds are
  not supported.
- Maximum 16 open fds per process (`FD_MAX`), 32 open files kernel-wide
  (`FILE_MAX`).

---

#### close (6)

```c
int close(int fd);
```

Close a file descriptor.  Decrements the reference count; the underlying file
object is freed when the count reaches zero.  Identical to POSIX.

---

#### lseek (19) / _llseek (140)

```c
off_t lseek(int fd, off_t off, int whence);
int _llseek(int fd, long off_hi, long off_lo, loff_t *result, int whence);
```

Reposition the file offset.  `whence` is `SEEK_SET` (0), `SEEK_CUR` (1), or
`SEEK_END` (2).

`_llseek` ignores the high word (PPAP files are small) and writes the 64-bit
result to `*result`.

Returns `-ESPIPE` for ttys and pipes.

**vs POSIX/Linux:**  Identical for regular files.  `_llseek` is the 32-bit
Linux compat syscall; PPAP ignores `off_hi`.

---

#### dup (41) / dup2 (63)

```c
int dup(int oldfd);
int dup2(int oldfd, int newfd);
```

Duplicate a file descriptor.  `dup` returns the lowest available fd.  `dup2`
atomically closes `newfd` (if open) and makes it a copy of `oldfd`.

**vs POSIX/Linux:**  Identical.  No `O_CLOEXEC` support (no `dup3`).

---

#### fcntl64 (221)

```c
int fcntl(int fd, int cmd, ...);
```

File control operations:

| Command | Behaviour |
|---------|-----------|
| `F_DUPFD` | Duplicate fd to lowest available >= arg |
| `F_DUPFD_CLOEXEC` | Same as `F_DUPFD` (CLOEXEC ignored) |
| `F_GETFD` | Returns 0 (no CLOEXEC tracking) |
| `F_SETFD` | No-op |
| `F_GETFL` | Returns file status flags |
| `F_SETFL` | Sets `O_APPEND` and `O_NONBLOCK` only |

**vs POSIX/Linux:**  `FD_CLOEXEC` is not implemented.  Advisory locking
(`F_GETLK`, `F_SETLK`, `F_SETLKW`) is not supported.

---

#### pipe (42)

```c
int pipe(int fds[2]);
```

Create a unidirectional byte pipe.  `fds[0]` is the read end, `fds[1]` is the
write end.

- Ring buffer size: 512 bytes (`PIPE_BUF_SIZE`).
- Readers block when the pipe is empty; writers block when full.
- Read returns 0 (EOF) when all write ends are closed.
- Write returns `-EPIPE` when all read ends are closed.
- Maximum 4 concurrent pipes (`PIPE_MAX`).

Blocking uses the svc_restart mechanism (syscall re-executes on wake).

**vs POSIX/Linux:**
- Buffer is 512 bytes (Linux default is 64 KB).
- `pipe2` (with `O_CLOEXEC`/`O_NONBLOCK` flags) is not available.
- `PIPE_BUF` atomicity guarantee: writes <= 512 bytes are atomic.

---

#### ioctl (54)

```c
int ioctl(int fd, unsigned long cmd, ...);
```

Device-specific control.  Dispatches to `f->ops->ioctl`.  Returns `-ENOTTY`
if the file does not support ioctl.

Supported commands depend on the driver (e.g. tty `TIOCGWINSZ`, `TCGETS`,
`TCSETS`, `TIOCGPGRP`, `TIOCSPGRP`).

**vs POSIX/Linux:**  Same interface.  Only a subset of tty ioctls is
implemented.

---

### File Metadata

#### stat (106) / fstat (108)

```c
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
```

Get file status using the kernel's internal `struct stat` format.  Dispatches
to the filesystem's `stat` operation.

---

#### stat64 (195) / lstat64 (196) / fstat64 (197) / fstatat64 (327)

```c
int stat64(const char *path, struct stat64 *buf);
int lstat64(const char *path, struct stat64 *buf);
int fstat64(int fd, struct stat64 *buf);
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags);
```

Get file status in Linux `struct stat64` format (96 bytes).  These are the
primary stat calls used by musl.  `lstat64` does not follow symlinks.
`fstatat64` only supports `dirfd == AT_FDCWD`.

Layout of `struct stat64` (ARM):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | st_dev |
| 12 | 4 | st_ino (truncated) |
| 16 | 4 | st_mode |
| 20 | 4 | st_nlink |
| 24 | 4 | st_uid |
| 28 | 4 | st_gid |
| 32 | 8 | st_rdev |
| 44 | 8 | st_size |
| 52 | 4 | st_blksize (always 4096) |
| 56 | 8 | st_blocks (ceil(size/512)) |
| 64 | 8 | st_atime + nsec |
| 72 | 8 | st_mtime + nsec |
| 80 | 8 | st_ctime + nsec |
| 88 | 8 | st_ino (full) |

**vs POSIX/Linux:**  Wire-compatible with the Linux ARM stat64 structure.
Timestamps are zero (no RTC).  `st_uid`/`st_gid` are always 0.

---

#### statx (397)

```c
int statx(int dirfd, const char *path, int flags, unsigned mask,
          struct statx *buf);
```

Always returns `-ENOSYS`.  The `stat64` family suffices for musl.

---

### Directory Operations

#### getdents (141) / getdents64 (217)

```c
int getdents(int fd, struct dirent *buf, size_t count);
int getdents64(int fd, struct dirent64 *buf, size_t count);
```

Read directory entries from an open directory fd.  Returns the number of
entries written, or 0 at end-of-directory.

`getdents64` outputs Linux `struct dirent64`:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | d_ino |
| 8 | 8 | d_off (next cookie) |
| 16 | 2 | d_reclen |
| 18 | 1 | d_type |
| 19 | var | d_name (NUL-terminated, 8-byte aligned) |

The file offset serves as the readdir cookie.  An internal `GETDENTS_EOF`
sentinel (0xFFFFFFFF) prevents restarting after reaching the end.

**vs POSIX/Linux:**  Same wire format as Linux.  `readdir(3)` in musl works
on top of `getdents64`.

---

#### mkdir (39)

```c
int mkdir(const char *path, mode_t mode);
```

Create a directory.  The `mode` is passed to the filesystem but not enforced
(PPAP has no permission model).  Returns `-EROFS` on read-only mounts.

**vs POSIX/Linux:**  Same interface.  `mode` is stored but not checked.

---

#### unlink (10) / rmdir (40)

```c
int unlink(const char *path);
int rmdir(const char *path);
```

Remove a file or directory.  `rmdir` delegates to `unlink` — the VFS unlink
handler checks whether the target is a non-empty directory.

**vs POSIX/Linux:**  `unlink` can remove both files and empty directories
(Linux `unlink` only removes files; `rmdir` only removes directories).

---

#### chdir (12) / getcwd (183)

```c
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
```

Change or query the working directory.

`getcwd` returns the length including the NUL terminator (not a pointer)
on success.

**vs POSIX/Linux:**  `getcwd` returns a length rather than a pointer in `r0`
(the musl wrapper handles the difference).  Maximum path length is 128 bytes
(`VFS_PATH_MAX`).

---

### Memory Management

#### brk (45)

```c
void *brk(void *addr);
```

Adjust the program break (heap boundary).

- `addr == 0`: query current break.
- `addr > current break`: expand by allocating 4 KB pages.
- `addr < current break`: shrink by freeing pages.

Always returns the current break value (never a negative errno), matching
Linux semantics so musl can fall back to `mmap` on failure.

**vs POSIX/Linux:**  Same semantics.  Maximum heap is limited by
`USER_PAGES_MAX` (32 pages = 128 KB per process).

---

#### mmap2 (192)

```c
void *mmap2(void *addr, size_t len, int prot, int flags, int fd, off_t pgoff);
```

Map anonymous memory.  Only `MAP_ANONYMOUS | MAP_PRIVATE` is supported.
`fd` must be -1.  `pgoff` is ignored.

- `MAP_FIXED`: try to allocate at the specified address.
- Otherwise: kernel chooses the address from the page pool.
- Maximum 8 mmap regions per process (`MMAP_REGIONS_MAX`).

**vs POSIX/Linux:**
- **No file-backed mappings** — only anonymous memory.
- **No shared mappings** — `MAP_SHARED` is not supported.
- **No protection enforcement** — `prot` is accepted but ignored (no MMU).
- Pages are always zero-filled on allocation.

---

#### munmap (91)

```c
int munmap(void *addr, size_t len);
```

Unmap a previously mapped region.  Frees the underlying pages.

**vs POSIX/Linux:**  Same interface.  Partial unmaps within a region are not
supported — the entire region is freed.

---

#### mremap (163)

```c
void *mremap(void *addr, size_t old, size_t new, int flags);
```

Always returns `-ENOMEM`.  This forces musl's `realloc` to fall back to
`mmap` + copy + `munmap` instead of trying to grow a mapping in-place.

---

#### mprotect (125)

```c
int mprotect(void *addr, size_t len, int prot);
```

Always returns 0 (no-op).  PPAP has no MMU-based page protection.

**vs POSIX/Linux:**  Linux enforces per-page R/W/X permissions.  PPAP
accepts the call silently to satisfy musl startup.

---

### Signal Handling

#### kill (37)

```c
int kill(pid_t pid, int sig);
```

Send a signal to a process.

- `sig == 0`: permission check only (always succeeds).
- Sets `sig_pending |= (1 << sig)` on the target process.
- Wakes the target if it is `PROC_BLOCKED` or `PROC_SLEEPING`.

**vs POSIX/Linux:**
- `pid == 0` (all in process group) and `pid == -1` (all processes) are not
  implemented — only specific `pid > 0`.
- No permission checks (single-user system).

---

#### sigaction (67) / rt_sigaction (174)

```c
int sigaction(int sig, uintptr_t handler, uintptr_t *old);
int rt_sigaction(int sig, const struct sigaction *act,
                 struct sigaction *oact, size_t sigsetsize);
```

Install a signal handler.

- `SIGKILL` (9) and `SIGSTOP` (19) cannot be caught.
- `SIG_DFL` (0): default action (terminate).
- `SIG_IGN` (1): ignore the signal.
- Function pointer: user handler invoked on signal delivery.

`rt_sigaction` uses the Linux `struct k_sigaction` (48 bytes) with `sa_flags`
and `sa_mask` fields that are accepted but not fully honoured.

**vs POSIX/Linux:**
- `sa_flags` (`SA_RESTART`, `SA_SIGINFO`, etc.) are stored but ignored.
- `sa_mask` is stored but not applied during handler execution.
- Signal queuing is not supported — signals are a simple bitmask.

---

#### rt_sigprocmask (175)

```c
int rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset,
                   size_t sigsetsize);
```

Manipulate the blocked signal mask.

| `how` | Action |
|-------|--------|
| `SIG_BLOCK` (0) | Add signals in `set` to blocked mask |
| `SIG_UNBLOCK` (1) | Remove signals in `set` from blocked mask |
| `SIG_SETMASK` (2) | Replace blocked mask with `set` |

Only the low 32 bits of the signal set are used.

**vs POSIX/Linux:**  Same interface.  Only 32 signals supported (Linux
supports 64).

---

#### sigreturn (119) / rt_sigreturn (173)

```c
void sigreturn(void);
void rt_sigreturn(void);
```

Restore context after a signal handler returns.  Called from the signal
trampoline.  Advances PSP past the trampoline frame so the hardware exception
return pops the original (pre-signal) context.

Both route to the same handler.  `rt_sigreturn` is the musl-preferred variant.

**vs POSIX/Linux:**  Same mechanism.  The trampoline is injected by the kernel
onto the user stack.

---

### Time

#### nanosleep (162) / clock_nanosleep (265, 407)

```c
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_nanosleep(clockid_t clk, int flags,
                    const struct timespec *req, struct timespec *rem);
```

Sleep for the specified duration.

- Resolution: 10 ms (100 Hz tick).  Sleeps shorter than 10 ms round up to
  one tick.
- The `clk` and `flags` parameters are accepted but ignored — all clocks
  are the same monotonic tick counter.
- `rem` is not filled in on return.
- Returns `-EINTR` if a signal is delivered during sleep.

Uses the svc_restart mechanism: the process is marked `PROC_SLEEPING` with
a deadline; on each reschedule the syscall re-executes and checks whether the
deadline has passed.

Syscall 265 uses 32-bit `struct timespec`; 407 uses 64-bit.

**vs POSIX/Linux:**
- 10 ms granularity (Linux: ~1 ns with hrtimers).
- `TIMER_ABSTIME` flag is not supported.
- `rem` is never filled in.

---

#### gettimeofday (78)

```c
int gettimeofday(struct timeval *tv, void *tz);
```

Get elapsed time since boot as `struct timeval`.

- `tv_sec = ticks / 100`
- `tv_usec = (ticks % 100) * 10000`
- `tz` is ignored.

**vs POSIX/Linux:**  Returns time since boot, not wall-clock time (no RTC).

---

#### clock_gettime (263, 403)

```c
int clock_gettime(clockid_t clk, struct timespec *tp);
int clock_gettime64(clockid_t clk, struct timespec64 *tp);
```

Get elapsed time since boot as `struct timespec`.

- `tv_sec = ticks / 100`
- `tv_nsec = (ticks % 100) * 10000000`
- `clk` is ignored — all clocks return the same value.

Syscall 263 uses 32-bit timespec; 403 uses 64-bit.

**vs POSIX/Linux:**
- `CLOCK_MONOTONIC`, `CLOCK_REALTIME`, etc. are all equivalent.
- Time is since boot, not epoch.
- No RTC — the counter starts at zero.

---

### File Descriptor Polling

#### ppoll (336) / ppoll_time64 (414)

```c
int ppoll(struct pollfd *fds, nfds_t nfds,
          const struct timespec *timeout, const sigset_t *sigmask);
```

Poll file descriptors for readiness.

Each `struct pollfd` contains `fd`, `events` (requested), and `revents`
(returned):

| Mask | Value | Meaning |
|------|-------|---------|
| `POLLIN` | 0x0001 | Data available for reading |
| `POLLOUT` | 0x0004 | Ready for writing |
| `POLLERR` | 0x0008 | Error condition |
| `POLLHUP` | 0x0010 | Hang up |
| `POLLNVAL` | 0x0020 | Invalid fd |

- If any fd is ready, returns immediately with the count.
- If `timeout` is zero (`tv_sec == 0 && tv_nsec == 0`): non-blocking poll.
- If `timeout` is NULL: block indefinitely.
- Returns `-EINTR` if a signal is pending.

Uses svc_restart for blocking.  Woken by `tty_rx_notify()` or timeout.

Syscall 336 uses 32-bit timespec; 414 uses 64-bit.

**vs POSIX/Linux:**
- `sigmask` is accepted but not applied (signal mask is not temporarily
  changed during the poll).
- Maximum number of fds per poll is limited by `FD_MAX` (16).

---

### Filesystem Management

#### mount (21)

```c
int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data);
```

Mount a filesystem at `target`.

Supported filesystem types:

| Type | Source | Description |
|------|--------|-------------|
| `devfs` | ignored | Device filesystem |
| `procfs` / `proc` | ignored | Process information filesystem |
| `tmpfs` | ignored | RAM-backed temporary filesystem |
| `vfat` | block device | FAT32 filesystem |
| `ufs` | block device | UFS filesystem |

For `vfat` and `ufs`, `source` should be the block device path (e.g.
`/dev/mmcblk0p1`); the `/dev/` prefix is stripped and the device is looked up
in the block device registry.

`flags & MS_RDONLY` mounts the filesystem read-only.

**vs POSIX/Linux:**  Same interface.  Only the listed filesystem types are
supported.  `data` (mount options string) is ignored.

---

#### umount2 (52)

```c
int umount2(const char *target, int flags);
```

Unmount the filesystem at `target`.  `flags` (e.g. `MNT_DETACH`) are accepted
but ignored.

**vs POSIX/Linux:**  Same interface.  Lazy unmount is not supported.

---

#### statfs64 (266) / fstatfs64 (267)

```c
int statfs64(const char *path, size_t sz, struct statfs64 *buf);
int fstatfs64(int fd, size_t sz, struct statfs64 *buf);
```

Get filesystem statistics for the mount containing `path` or `fd`.

**vs POSIX/Linux:**  Same interface.  Values come from the filesystem driver's
`statfs` operation.

---

### Ownership and Permissions

PPAP is a single-user system running as root.  Ownership and permission
syscalls are stubs that satisfy musl and standard utilities.

#### getuid32 (199) / geteuid32 (201) / getgid32 (200) / getegid32 (202)

All return 0 (root).

#### chown32 (212) / lchown32 (198) / fchown32 (207)

All return 0 (success, no-op).

#### chmod (15)

Returns 0 (success, no-op).

#### setgroups32 (206)

Returns 0 (success, no-op).

#### umask (60)

```c
mode_t umask(mode_t mask);
```

Set the file creation mask.  Returns the previous mask.  The mask is stored
in the PCB but not enforced by any filesystem.

#### access (33)

```c
int access(const char *path, int mode);
```

Check file accessibility.  Verifies the path exists via VFS lookup but always
grants access (root user).  Returns `-ENOENT` if the path does not exist.

---

### Miscellaneous

#### readlink (85)

```c
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
```

Read the target of a symbolic link.  Does **not** NUL-terminate the output.

Special case: `/proc/self/exe` returns `/bin/busybox`.

**vs POSIX/Linux:**  Same interface.  The `/proc/self/exe` mapping is
hardcoded.

---

#### rename (38)

```c
int rename(const char *oldpath, const char *newpath);
```

Always returns `-ENOSYS`.  Not implemented.

---

#### mknod (14)

```c
int mknod(const char *path, mode_t mode, dev_t dev);
```

Always returns `-EPERM`.  Device nodes are created via devfs, not `mknod`.

---

#### utimes (269)

```c
int utimes(const char *path, const struct timeval tv[2]);
```

Returns 0 (no-op).  No RTC — timestamps are meaningless.

---

#### futex (240)

```c
int futex(int *uaddr, int op, int val, ...);
```

Returns 0 (no-op).  PPAP is single-threaded — futex synchronisation is
unnecessary.  Exists to satisfy musl's lock initialisation.

---

## Resource Limits

| Resource | Limit | Notes |
|----------|-------|-------|
| Processes | 8 (`PROC_MAX`) | Static process table |
| File descriptors per process | 16 (`FD_MAX`) | |
| Open files (kernel-wide) | 32 (`FILE_MAX`) | Shared pool |
| User pages per process | 32 (`USER_PAGES_MAX`) | 128 KB max |
| mmap regions per process | 8 (`MMAP_REGIONS_MAX`) | Anonymous only |
| Mount points | 8 (`VFS_MOUNT_MAX`) | |
| Vnodes | 64 (`VFS_VNODE_MAX`) | In-memory inode cache |
| Path length | 128 (`VFS_PATH_MAX`) | Bytes |
| Filename length | 63 (`VFS_NAME_MAX`) | Bytes |
| Symlink depth | 8 (`VFS_SYMLOOP_MAX`) | |
| Pipes | 4 (`PIPE_MAX`) | |
| Pipe buffer | 512 (`PIPE_BUF_SIZE`) | Bytes |
| Block devices | 4 (`BLKDEV_MAX`) | |
| Page size | 4096 (`PAGE_SIZE`) | Bytes |
| Tick rate | 100 Hz (`PPAP_TICK_HZ`) | 10 ms resolution |

---

## Error Codes

PPAP uses standard POSIX errno values (Linux ARM ABI numbering):

| Errno | Value | Description |
|-------|-------|-------------|
| EPERM | 1 | Operation not permitted |
| ENOENT | 2 | No such file or directory |
| ESRCH | 3 | No such process |
| EINTR | 4 | Interrupted system call |
| EIO | 5 | I/O error |
| ENOEXEC | 8 | Exec format error |
| EBADF | 9 | Bad file descriptor |
| ECHILD | 10 | No child processes |
| ENOMEM | 12 | Out of memory |
| EACCES | 13 | Permission denied |
| EFAULT | 14 | Bad address |
| EBUSY | 16 | Device or resource busy |
| EEXIST | 17 | File exists |
| ENODEV | 19 | No such device |
| ENOTDIR | 20 | Not a directory |
| EISDIR | 21 | Is a directory |
| EINVAL | 22 | Invalid argument |
| EMFILE | 24 | Too many open files |
| ENOTTY | 25 | Inappropriate ioctl |
| ENOSPC | 28 | No space left on device |
| ESPIPE | 29 | Illegal seek |
| EROFS | 30 | Read-only file system |
| EPIPE | 32 | Broken pipe |
| ERANGE | 34 | Result too large |
| ENAMETOOLONG | 36 | File name too long |
| ENOSYS | 38 | Function not implemented |
| ENOTEMPTY | 39 | Directory not empty |
| ELOOP | 40 | Too many symbolic links |
| ETIMEDOUT | 110 | Connection timed out |

---

## Blocking Syscalls and Restart

The following syscalls can block the calling process:

| Syscall | Blocks when |
|---------|-------------|
| waitpid / wait4 | No zombie child available |
| nanosleep / clock_nanosleep | Sleep duration not elapsed |
| read (pipe) | Pipe buffer empty |
| write (pipe) | Pipe buffer full |
| ppoll | No ready fds and timeout not expired |

Blocking uses the **svc_restart** mechanism:

1. The syscall marks the process as `PROC_BLOCKED` or `PROC_SLEEPING`.
2. It sets `svc_restart[core_id] = 1` and saves the original first argument.
3. The process yields to the scheduler.
4. When woken, SVC_Handler restores the saved argument into the exception
   frame and adjusts the stacked PC back by 2 bytes (to the `svc 0`
   instruction).
5. The syscall re-executes with the original arguments and checks whether the
   blocking condition has been resolved.
