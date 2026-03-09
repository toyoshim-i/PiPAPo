# PPAP System Call Reference

This document describes every system call available in the PiPAPo
kernel, with usage details and notes on how each differs from POSIX / Linux.

---

## Syscall Numbering

PPAP uses a **16-bit grouped numbering** scheme: the high byte is the group,
the low byte is the index.  The same numbers are used on both ARM and m68k.

| Group  | Category       | Range          |
|--------|---------------|----------------|
| 0x00xx | Process        | exit, fork, exec, wait, ... |
| 0x01xx | I/O            | read, write, open, close, ... |
| 0x02xx | FS paths       | stat, access, mkdir, openat, ... |
| 0x03xx | FS extended    | stat64, getdents64, llseek, ... |
| 0x04xx | Memory         | brk, mmap2, munmap, ... |
| 0x05xx | Time           | nanosleep, clock_gettime, ... |
| 0x06xx | Signals        | kill, sigaction, rt_sigaction, ... |
| 0x07xx | Poll           | poll, ppoll |
| 0x08xx | User/group     | getuid, getgid, chown, ... |
| 0x09xx | Mount          | mount, umount2 |
| 0x0Axx | Misc           | futex, getcpu |
| 0xF0xx | Unimplemented  | Return -ENOSYS (musl compile stubs) |

Unimplemented syscalls (0xF0xx) exist so that musl libc compiles, but the
kernel returns `-ENOSYS` for all of them.

The canonical definitions are in `src/kernel/syscall/syscall.h` (kernel) and
`third_party/patches/musl/overlay/arch/{arm,m68k}/bits/syscall.h.in` (musl).
Both architectures must use identical numbers for all implemented syscalls.

---

## Calling Convention

### ARM (Thumb / EABI)

| Register | Purpose |
|----------|---------|
| `r7` | Syscall number |
| `r0`–`r3` | Arguments 1–4 |
| `r4` | Argument 5 |
| `r5` | Argument 6 |
| `r0` | Return value |

Invoke with `svc 0`.

### m68k (Linux m68k / musl convention)

| Register | Purpose |
|----------|---------|
| `d0` | Syscall number → return value |
| `d1`–`d5` | Arguments 1–5 |
| `a0` | Argument 6 |

Invoke with `trap #0`.

On success the return value is zero or positive.
On error the return value is a negative errno (e.g. `-ENOENT`).

---

## Syscall Table

| Number | Name | Signature |
|--------|------|-----------|
| 0x0000 | exit | `void exit(int status)` |
| 0x0001 | exit_group | `void exit_group(int status)` |
| 0x0002 | vfork | `pid_t vfork(void)` |
| 0x0003 | execve | `int execve(const char *path, char *const argv[], char *const envp[])` |
| 0x0004 | waitpid | `pid_t waitpid(pid_t pid, int *status, int options)` |
| 0x0005 | wait4 | `pid_t wait4(pid_t pid, int *status, int options, struct rusage *ru)` |
| 0x0006 | getpid | `pid_t getpid(void)` |
| 0x0007 | uname | `int uname(struct utsname *buf)` |
| 0x0008 | getppid | `pid_t getppid(void)` |
| 0x0009 | setpgid | `int setpgid(pid_t pid, pid_t pgid)` |
| 0x000A | getpgid | `pid_t getpgid(pid_t pid)` |
| 0x000B | setsid | `pid_t setsid(void)` |
| 0x000C | clone | `pid_t clone(unsigned long flags, void *stack, ...)` |
| 0x000D | set_tid_address | `pid_t set_tid_address(int *tidptr)` |
| 0x000E | fork | `pid_t fork(void)` |
| 0x0100 | read | `ssize_t read(int fd, void *buf, size_t n)` |
| 0x0101 | write | `ssize_t write(int fd, const void *buf, size_t n)` |
| 0x0102 | open | `int open(const char *path, int flags, mode_t mode)` |
| 0x0103 | close | `int close(int fd)` |
| 0x0104 | dup | `int dup(int oldfd)` |
| 0x0105 | dup2 | `int dup2(int oldfd, int newfd)` |
| 0x0106 | pipe | `int pipe(int fds[2])` |
| 0x0107 | ioctl | `int ioctl(int fd, unsigned long cmd, ...)` |
| 0x0108 | fcntl / fcntl64 | `int fcntl(int fd, int cmd, ...)` |
| 0x0109 | readv | `ssize_t readv(int fd, const struct iovec *iov, int iovcnt)` |
| 0x010A | writev | `ssize_t writev(int fd, const struct iovec *iov, int iovcnt)` |
| 0x010B | lseek | `off_t lseek(int fd, off_t off, int whence)` |
| 0x0200 | stat | `int stat(const char *path, struct stat *buf)` |
| 0x0201 | fstat | `int fstat(int fd, struct stat *buf)` |
| 0x0202 | access | `int access(const char *path, int mode)` |
| 0x0203 | getcwd | `char *getcwd(char *buf, size_t size)` |
| 0x0204 | mkdir | `int mkdir(const char *path, mode_t mode)` |
| 0x0205 | rmdir | `int rmdir(const char *path)` |
| 0x0206 | unlink | `int unlink(const char *path)` |
| 0x0207 | chdir | `int chdir(const char *path)` |
| 0x0208 | readlink | `ssize_t readlink(const char *path, char *buf, size_t bufsiz)` |
| 0x0209 | rename | `int rename(const char *old, const char *new)` |
| 0x020A | mknod | `int mknod(const char *path, mode_t mode, dev_t dev)` |
| 0x020B | chmod | `int chmod(const char *path, mode_t mode)` |
| 0x020C | openat | `int openat(int dirfd, const char *path, int flags, mode_t mode)` |
| 0x020D | fstatat64 | `int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags)` |
| 0x0300 | getdents | `int getdents(int fd, struct dirent *buf, size_t count)` |
| 0x0301 | stat64 | `int stat64(const char *path, struct stat64 *buf)` |
| 0x0302 | fstat64 | `int fstat64(int fd, struct stat64 *buf)` |
| 0x0303 | umask | `mode_t umask(mode_t mask)` |
| 0x0304 | lstat64 | `int lstat64(const char *path, struct stat64 *buf)` |
| 0x0305 | statfs64 | `int statfs64(const char *path, size_t sz, struct statfs64 *buf)` |
| 0x0306 | fstatfs64 | `int fstatfs64(int fd, size_t sz, struct statfs64 *buf)` |
| 0x0307 | _llseek | `int _llseek(int fd, long off_hi, long off_lo, loff_t *result, int whence)` |
| 0x0308 | statx | `int statx(...)` — always returns `-ENOSYS` |
| 0x0309 | utimes | `int utimes(const char *path, const struct timeval tv[2])` |
| 0x030A | getdents64 | `int getdents64(int fd, struct dirent64 *buf, size_t count)` |
| 0x0400 | brk | `void *brk(void *addr)` |
| 0x0401 | mmap2 | `void *mmap2(void *addr, size_t len, int prot, int flags, int fd, off_t pgoff)` |
| 0x0402 | munmap | `int munmap(void *addr, size_t len)` |
| 0x0403 | mprotect | `int mprotect(void *addr, size_t len, int prot)` — always returns 0 |
| 0x0404 | mremap | `void *mremap(...)` — always returns `-ENOMEM` |
| 0x0500 | nanosleep | `int nanosleep(const struct timespec *req, struct timespec *rem)` |
| 0x0501 | clock_gettime32 | `int clock_gettime(clockid_t clk, struct timespec *tp)` |
| 0x0502 | gettimeofday | `int gettimeofday(struct timeval *tv, void *tz)` |
| 0x0503 | clock_nanosleep32 | `int clock_nanosleep(clockid_t clk, int flags, ...)` |
| 0x0504 | clock_gettime64 | `int clock_gettime64(clockid_t clk, struct timespec64 *tp)` |
| 0x0505 | clock_nanosleep64 | `int clock_nanosleep64(clockid_t clk, int flags, ...)` |
| 0x0600 | kill | `int kill(pid_t pid, int sig)` |
| 0x0601 | sigaction | `int sigaction(int sig, uintptr_t handler, uintptr_t *old)` |
| 0x0602 | sigreturn | `void sigreturn(void)` |
| 0x0603 | rt_sigaction | `int rt_sigaction(int sig, const struct sigaction *act, ...)` |
| 0x0604 | rt_sigprocmask | `int rt_sigprocmask(int how, const sigset_t *set, ...)` |
| 0x0605 | rt_sigreturn | `void rt_sigreturn(void)` |
| 0x0700 | poll | `int poll(struct pollfd *fds, nfds_t nfds, int timeout)` |
| 0x0701 | ppoll / ppoll_time64 | `int ppoll(struct pollfd *fds, nfds_t n, ...)` |
| 0x0800 | getuid / getuid32 | `uid_t getuid(void)` — always returns 0 |
| 0x0801 | getgid / getgid32 | `gid_t getgid(void)` — always returns 0 |
| 0x0802 | geteuid / geteuid32 | `uid_t geteuid(void)` — always returns 0 |
| 0x0803 | getegid / getegid32 | `gid_t getegid(void)` — always returns 0 |
| 0x0804 | chown / chown32 | `int chown(...)` — no-op, returns 0 |
| 0x0805 | lchown / lchown32 | `int lchown(...)` — no-op, returns 0 |
| 0x0806 | setgroups / setgroups32 | `int setgroups(...)` — no-op, returns 0 |
| 0x0807 | fchown / fchown32 | `int fchown(...)` — no-op, returns 0 |
| 0x0900 | mount | `int mount(const char *src, const char *tgt, const char *fs, ...)` |
| 0x0901 | umount2 | `int umount2(const char *target, int flags)` |
| 0x0A00 | futex / futex_time64 | `int futex(...)` — no-op, returns 0 |
| 0x0A01 | getcpu | `int getcpu(unsigned *cpu, unsigned *node, void *unused)` |

### m68k-specific kernel syscalls

| Number | Name | Notes |
|--------|------|-------|
| 0xF0A8 | get_thread_area | Returns TLS pointer (musl m68k TLS) |
| 0xF0A9 | set_thread_area | Sets TLS pointer |

These use 0xF0xx numbers because they are m68k-specific and not part of the
shared PPAP numbering.  The kernel dispatches them explicitly in syscall.c.

---

## Detailed Descriptions

### Process Management

#### exit (0x0000) / exit_group (0x0001)

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

#### fork (0x000E) / clone (0x000C) / vfork (0x0002)

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

#### waitpid (0x0004) / wait4 (0x0005)

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

#### execve (0x0003)

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

#### getpid (0x0006) / getppid (0x0008)

```c
pid_t getpid(void);
pid_t getppid(void);
```

Return the process ID / parent process ID.  Identical to POSIX.

---

#### setpgid (0x0009) / getpgid (0x000A)

```c
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
```

Set or get the process group ID.  `pid == 0` means the calling process.

**vs POSIX/Linux:**  Simplified — no permission checks, no session-leader
restrictions.

---

#### setsid (0x000B)

```c
pid_t setsid(void);
```

Create a new session.  Sets both `sid` and `pgid` to the caller's PID.
Returns the new session ID.

**vs POSIX/Linux:**  Always succeeds — no check for existing session leader.

---

#### set_tid_address (0x000D)

```c
pid_t set_tid_address(int *tidptr);
```

Store `tidptr` in the PCB for thread library use.  Returns the caller's PID.

**vs POSIX/Linux:**  The kernel stores the pointer but never writes to it
(no thread support).  Exists to satisfy musl's startup sequence.

---

#### uname (0x0007)

```c
int uname(struct utsname *buf);
```

Fill `buf` (390 bytes = 6 x 65-byte fields) with system identification:

| Field | Value |
|-------|-------|
| sysname | `PiPAPo` |
| nodename | `ppap` |
| release | `0.11.0` |
| version | `#1 PPAP` |
| machine | `armv6m` (ARM) or `m68k` (m68k) |
| domainname | (empty) |

**vs POSIX/Linux:**  Identical interface.  Values are hardcoded.

---

#### getcpu (0x0A01)

```c
int getcpu(unsigned *cpu, unsigned *node, void *unused);
```

Write the current CPU core number (0 or 1) to `*cpu`.  `node` is set to 0.

**vs Linux:**  Same interface.  Always returns core 0 or 1 (RP2040 dual-core).

---

### File I/O

#### read (0x0100) / write (0x0101)

```c
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
```

Read from or write to a file descriptor.  Dispatches through the file
operations vtable (`f->ops->read` / `f->ops->write`).

**vs POSIX/Linux:**  Identical interface.  Behaviour depends on the backing
driver (tty, VFS file, pipe, device file).

---

#### readv (0x0109) / writev (0x010A)

```c
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
```

Scatter/gather I/O.  Loops through the iovec array calling `read`/`write` for
each element.  `iovcnt` must be 1–1024.

**vs POSIX/Linux:**  Semantically identical.  Internally implemented as a loop
of single read/write calls (not truly atomic across iovecs).

---

#### open (0x0102) / openat (0x020C)

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

#### close (0x0103)

```c
int close(int fd);
```

Close a file descriptor.  Decrements the reference count; the underlying file
object is freed when the count reaches zero.  Identical to POSIX.

---

#### lseek (0x010B) / _llseek (0x0307)

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

#### dup (0x0104) / dup2 (0x0105)

```c
int dup(int oldfd);
int dup2(int oldfd, int newfd);
```

Duplicate a file descriptor.  `dup` returns the lowest available fd.  `dup2`
atomically closes `newfd` (if open) and makes it a copy of `oldfd`.

**vs POSIX/Linux:**  Identical.  No `O_CLOEXEC` support (no `dup3`).

---

#### fcntl64 (0x0108)

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

#### pipe (0x0106)

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

#### ioctl (0x0107)

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

#### stat (0x0200) / fstat (0x0201)

```c
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
```

Get file status using the kernel's internal `struct stat` format.  Dispatches
to the filesystem's `stat` operation.

---

#### stat64 (0x0301) / lstat64 (0x0304) / fstat64 (0x0302) / fstatat64 (0x020D)

```c
int stat64(const char *path, struct stat64 *buf);
int lstat64(const char *path, struct stat64 *buf);
int fstat64(int fd, struct stat64 *buf);
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags);
```

Get file status in Linux `struct stat64` format (96 bytes).  These are the
primary stat calls used by musl.  `lstat64` does not follow symlinks.
`fstatat64` only supports `dirfd == AT_FDCWD`.

**vs POSIX/Linux:**  Wire-compatible with the Linux ARM stat64 structure.
Timestamps are zero (no RTC).  `st_uid`/`st_gid` are always 0.

---

### Directory Operations

#### getdents (0x0300) / getdents64 (0x030A)

```c
int getdents(int fd, struct dirent *buf, size_t count);
int getdents64(int fd, struct dirent64 *buf, size_t count);
```

Read directory entries from an open directory fd.  Returns the number of
entries written, or 0 at end-of-directory.

**vs POSIX/Linux:**  Same wire format as Linux.  `readdir(3)` in musl works
on top of `getdents64`.

---

### Memory Management

#### brk (0x0400)

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

#### mmap2 (0x0401)

```c
void *mmap2(void *addr, size_t len, int prot, int flags, int fd, off_t pgoff);
```

Map anonymous memory.  Only `MAP_ANONYMOUS | MAP_PRIVATE` is supported.
`fd` must be -1.  `pgoff` is ignored.

**vs POSIX/Linux:**
- **No file-backed mappings** — only anonymous memory.
- **No shared mappings** — `MAP_SHARED` is not supported.
- **No protection enforcement** — `prot` is accepted but ignored (no MMU).

---

### Signal Handling

#### kill (0x0600)

```c
int kill(pid_t pid, int sig);
```

Send a signal to a process.

- `sig == 0`: permission check only (always succeeds).
- Sets `sig_pending |= (1 << sig)` on the target process.
- Wakes the target if it is `PROC_BLOCKED` or `PROC_SLEEPING`.

---

#### sigaction (0x0601) / rt_sigaction (0x0603)

```c
int sigaction(int sig, uintptr_t handler, uintptr_t *old);
int rt_sigaction(int sig, const struct sigaction *act,
                 struct sigaction *oact, size_t sigsetsize);
```

Install a signal handler.  `SIGKILL` (9) and `SIGSTOP` (19) cannot be caught.

---

#### rt_sigprocmask (0x0604)

```c
int rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset,
                   size_t sigsetsize);
```

Manipulate the blocked signal mask (`SIG_BLOCK`, `SIG_UNBLOCK`, `SIG_SETMASK`).

---

### Time

#### nanosleep (0x0500) / clock_nanosleep (0x0503, 0x0505)

```c
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_nanosleep(clockid_t clk, int flags,
                    const struct timespec *req, struct timespec *rem);
```

Sleep for the specified duration.  Resolution: 10 ms (100 Hz tick).

0x0503 uses 32-bit `struct timespec`; 0x0505 uses 64-bit.

---

#### gettimeofday (0x0502)

```c
int gettimeofday(struct timeval *tv, void *tz);
```

Get elapsed time since boot as `struct timeval`.

---

#### clock_gettime (0x0501, 0x0504)

```c
int clock_gettime(clockid_t clk, struct timespec *tp);
int clock_gettime64(clockid_t clk, struct timespec64 *tp);
```

Get elapsed time since boot.  All clock IDs return the same monotonic value.

---

### Polling

#### poll (0x0700) / ppoll (0x0701)

```c
int poll(struct pollfd *fds, nfds_t nfds, int timeout);
int ppoll(struct pollfd *fds, nfds_t n,
          const struct timespec *timeout, const sigset_t *sigmask);
```

Poll file descriptors for readiness.  `sigmask` is accepted but not applied.

---

### Filesystem Management

#### mount (0x0900) / umount2 (0x0901)

```c
int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data);
int umount2(const char *target, int flags);
```

Supported filesystem types: `devfs`, `procfs`/`proc`, `tmpfs`, `vfat`, `ufs`.

---

#### statfs64 (0x0305) / fstatfs64 (0x0306)

```c
int statfs64(const char *path, size_t sz, struct statfs64 *buf);
int fstatfs64(int fd, size_t sz, struct statfs64 *buf);
```

Get filesystem statistics.

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

PPAP uses standard POSIX errno values (Linux ABI numbering):

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

On m68k, a per-process `svc_needs_restart` flag is used instead of the global
`svc_restart` array, because the m68k TRAP handler re-executes in a C loop
rather than adjusting the stacked PC.
