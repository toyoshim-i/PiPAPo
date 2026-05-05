/*
 * syscall.S — INT 30h syscall stubs for PPAP i16 user-space programs
 *
 * PPAP i16 syscall convention:
 *   AX = syscall number
 *   BX, CX, DX, SI, DI = arguments 1-5
 *   AX = return value (16-bit; sign-extended to 32-bit for C)
 *
 * GCC's ia16 cdecl ABI preserves BX, SI, and DI across calls, so the
 * wrappers save and restore them around INT 30h.
 */

  .code16
  .text

.macro SYSCALL_RET
  cwd                     /* sign-extend AX into DX for 32-bit C returns */
  popw  %di
  popw  %si
  popw  %bx
  popw  %bp
  ret
.endm

/* void _exit(int status); */
  .global _exit
_exit:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  movw  4(%bp), %bx
  movw  $0x0000, %ax
  int   $0x30
.Lexit_hang:
  hlt
  jmp   .Lexit_hang

/* pid_t getpid(void); */
  .global getpid
getpid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  $0x0006, %ax
  int   $0x30
  SYSCALL_RET

/* int uname(struct utsname *buf); */
  .global uname
uname:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0B01, %ax
  int   $0x30
  SYSCALL_RET

/* pid_t getppid(void); */
  .global getppid
getppid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  $0x0008, %ax
  int   $0x30
  SYSCALL_RET

/* pid_t vfork(void); */
  .global vfork
vfork:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  $0x0002, %ax
  int   $0x30
  SYSCALL_RET

/* pid_t waitpid(pid_t pid, int *status, int options); */
  .global waitpid
waitpid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* pid low 16 bits */
  movw  8(%bp), %cx      /* status pointer */
  movw  10(%bp), %dx     /* options */
  movw  $0x0004, %ax
  int   $0x30
  SYSCALL_RET

/* int execve(const char *path, char *const argv[], char *const envp[]); */
  .global execve
execve:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0003, %ax
  int   $0x30
  SYSCALL_RET

/* ssize_t read(int fd, void *buf, size_t n); */
  .global read
read:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0100, %ax
  int   $0x30
  SYSCALL_RET

/* ssize_t write(int fd, const void *buf, size_t n); */
  .global write
write:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0101, %ax
  int   $0x30
  SYSCALL_RET

/* int open(const char *path, int flags, int mode); */
  .global open
open:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0102, %ax
  int   $0x30
  SYSCALL_RET

/* int close(int fd); */
  .global close
close:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0103, %ax
  int   $0x30
  SYSCALL_RET

/* int dup(int oldfd); */
  .global dup
dup:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0104, %ax
  int   $0x30
  SYSCALL_RET

/* int dup2(int oldfd, int newfd); */
  .global dup2
dup2:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  $0x0105, %ax
  int   $0x30
  SYSCALL_RET

/* int pipe(int fds[2]); */
  .global pipe
pipe:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0106, %ax
  int   $0x30
  SYSCALL_RET

/* int lseek(int fd, int offset, int whence); */
  .global lseek
lseek:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x010B, %ax
  int   $0x30
  SYSCALL_RET

/* int stat(const char *path, struct stat *buf); */
  .global stat
stat:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  $0x0200, %ax
  int   $0x30
  SYSCALL_RET

/* int access(const char *path, int mode); */
  .global access
access:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  $0x0202, %ax
  int   $0x30
  SYSCALL_RET

/* int getcwd(char *buf, size_t size); */
  .global getcwd
getcwd:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  $0x0203, %ax
  int   $0x30
  SYSCALL_RET

/* int chdir(const char *path); */
  .global chdir
chdir:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0207, %ax
  int   $0x30
  SYSCALL_RET

/* ssize_t readlink(const char *path, char *buf, size_t bufsiz); */
  .global readlink
readlink:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0208, %ax
  int   $0x30
  SYSCALL_RET

/* int getdents(int fd, struct dirent *buf, size_t count); */
  .global getdents
getdents:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  8(%bp), %dx
  movw  $0x0300, %ax
  int   $0x30
  SYSCALL_RET

/* int statfs64(const char *path, long sz, struct statfs *buf); */
  .global statfs64
statfs64:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  10(%bp), %dx
  movw  $0x0305, %ax
  int   $0x30
  SYSCALL_RET

/* void *brk(void *addr); */
  .global brk
brk:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  $0x0400, %ax
  int   $0x30
  popw  %di
  popw  %si
  popw  %bx
  popw  %bp
  ret

/* int clock_gettime(int clk_id, void *tp); */
  .global clock_gettime
clock_gettime:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx
  movw  6(%bp), %cx
  movw  $0x0501, %ax
  int   $0x30
  SYSCALL_RET

/* int settimeofday(const struct timeval *tv, const void *tz); */
  .global settimeofday
settimeofday:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* tv */
  movw  6(%bp), %cx      /* tz (NULL in practice) */
  movw  $0x0506, %ax
  int   $0x30
  SYSCALL_RET

/* int ioctl(int fd, unsigned long cmd, void *arg); */
  .global ioctl
ioctl:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* fd */
  movw  6(%bp), %cx      /* cmd (low 16 bits, high is always 0) */
  movw  10(%bp), %dx     /* arg (after 4-byte unsigned long cmd) */
  movw  $0x0107, %ax
  int   $0x30
  SYSCALL_RET

/* User-space sigreturn trampoline.
 *
 * The kernel records the address of this symbol as sa_restorer when
 * sigaction() installs a handler (see below), and uses it as the
 * handler's near-return target when delivering a signal.  The handler's
 * `ret` lands here and this stub issues SYS_RT_SIGRETURN to unwind the
 * signal frame — same role as the sigreturn trampolines glibc/musl
 * provide on Linux. */
  .global _ppap_sigreturn_trampoline
_ppap_sigreturn_trampoline:
  addw  $2, %sp                 /* drop the sig arg the kernel planted */
  movw  $0x0605, %ax            /* SYS_RT_SIGRETURN */
  int   $0x30
  hlt                            /* unreachable */

/* sighandler_t signal(int sig, sighandler_t handler);
 *
 * Builds a kernel_sigaction on the user stack with
 * sa_restorer = _ppap_sigreturn_trampoline and forwards to
 * SYS_RT_SIGACTION.  Returns the syscall result, not the previous
 * handler — POSIX signal() callers that don't inspect the return
 * value (the common case) work fine. */
  .global signal
signal:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di

  /* Push struct kernel_sigaction (20 B) in reverse-field order; the
   * final SP points to act->handler. */
  xorw  %ax, %ax
  pushw %ax                     /* mask[1] high */
  pushw %ax                     /* mask[1] low  */
  pushw %ax                     /* mask[0] high */
  pushw %ax                     /* mask[0] low  */
  pushw %ax                     /* restorer high (ia16 near ptr) */
  movw  $_ppap_sigreturn_trampoline, %cx
  pushw %cx                     /* restorer low  */
  pushw %ax                     /* flags high   */
  pushw %ax                     /* flags low    */
  pushw %ax                     /* handler high (ia16 near ptr) */
  movw  6(%bp), %cx             /* handler arg */
  pushw %cx                     /* handler low  */

  movw  4(%bp), %bx             /* sig */
  movw  %sp, %cx                /* &act */
  xorw  %dx, %dx                /* oact = NULL (old_handler arg ignored) */
  movw  $8, %si                 /* sigsetsize */
  movw  $0x0603, %ax            /* SYS_RT_SIGACTION */
  int   $0x30

  addw  $20, %sp                /* drop struct */
  SYSCALL_RET

/* int rt_sigaction(int sig, const struct sigaction *act,
 *                  struct sigaction *oact, int sigsetsize); */
  .global rt_sigaction
rt_sigaction:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx             /* sig */
  movw  6(%bp), %cx             /* act */
  movw  8(%bp), %dx             /* oact */
  movw  10(%bp), %si            /* sigsetsize */
  movw  $0x0603, %ax            /* SYS_RT_SIGACTION */
  int   $0x30
  SYSCALL_RET

/* int kill(pid_t pid, int sig); */
  .global kill
kill:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* pid low 16 bits (pid_t is int32_t on ia16) */
  movw  8(%bp), %cx      /* sig — 4 bytes above bp past the pid_t slot  */
  movw  $0x0600, %ax
  int   $0x30
  SYSCALL_RET

/* int nanosleep(const void *req, void *rem); */
  .global nanosleep
nanosleep:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* req */
  movw  6(%bp), %cx      /* rem */
  movw  $0x0500, %ax
  int   $0x30
  SYSCALL_RET

/* int ppoll(struct pollfd *fds, int nfds, void *timeout,
 *           void *sigmask, int sigsetsize); */
  .global ppoll
ppoll:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* fds */
  movw  6(%bp), %cx      /* nfds */
  movw  8(%bp), %dx      /* timeout */
  movw  10(%bp), %si     /* sigmask */
  movw  12(%bp), %di     /* sigsetsize */
  movw  $0x0701, %ax
  int   $0x30
  SYSCALL_RET

/* ssize_t writev(int fd, const struct iovec *iov, int iovcnt); */
  .global writev
writev:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* fd */
  movw  6(%bp), %cx      /* iov */
  movw  8(%bp), %dx      /* iovcnt */
  movw  $0x010A, %ax
  int   $0x30
  SYSCALL_RET

/* ssize_t readv(int fd, const struct iovec *iov, int iovcnt); */
  .global readv
readv:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* fd */
  movw  6(%bp), %cx      /* iov */
  movw  8(%bp), %dx      /* iovcnt */
  movw  $0x0109, %ax
  int   $0x30
  SYSCALL_RET

/* int mkdir(const char *path, int mode); */
  .global mkdir
mkdir:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* path */
  movw  6(%bp), %cx      /* mode */
  movw  $0x0204, %ax
  int   $0x30
  SYSCALL_RET

/* int unlink(const char *path); */
  .global unlink
unlink:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* path */
  movw  $0x0206, %ax
  int   $0x30
  SYSCALL_RET

/* int rmdir(const char *path); */
  .global rmdir
rmdir:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* path */
  movw  $0x0205, %ax
  int   $0x30
  SYSCALL_RET

/* int utimes(const char *path, const struct timeval times[2]); */
  .global utimes
utimes:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* path */
  movw  6(%bp), %cx      /* times[2] (or NULL) */
  movw  $0x0309, %ax
  int   $0x30
  SYSCALL_RET

/* int rename(const char *oldpath, const char *newpath); */
  .global rename
rename:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* oldpath */
  movw  6(%bp), %cx      /* newpath */
  movw  $0x0209, %ax
  int   $0x30
  SYSCALL_RET

/* int chmod(const char *path, int mode); */
  .global chmod
chmod:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* path */
  movw  6(%bp), %cx      /* mode */
  movw  $0x020B, %ax
  int   $0x30
  SYSCALL_RET

/* int link(const char *oldpath, const char *newpath); */
  .global link
link:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* oldpath */
  movw  6(%bp), %cx      /* newpath */
  movw  $0x020E, %ax
  int   $0x30
  SYSCALL_RET

/* pid_t setsid(void); */
  .global setsid
setsid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  $0x000B, %ax
  int   $0x30
  SYSCALL_RET

/* int setpgid(pid_t pid, pid_t pgid); */
  .global setpgid
setpgid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* pid */
  movw  6(%bp), %cx      /* pgid */
  movw  $0x0009, %ax
  int   $0x30
  SYSCALL_RET

/* pid_t getpgid(pid_t pid); */
  .global getpgid
getpgid:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* pid */
  movw  $0x000A, %ax
  int   $0x30
  SYSCALL_RET

/* long ptrace(int req, pid_t pid, void *addr, void *data); */
  .global ptrace
ptrace:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* req */
  movw  6(%bp), %cx      /* pid */
  movw  8(%bp), %dx      /* addr */
  movw  10(%bp), %si     /* data */
  movw  $0x000F, %ax
  int   $0x30
  SYSCALL_RET

/* void poweroff(void); */
  .global poweroff
poweroff:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  $0x0B00, %ax
  int   $0x30
  SYSCALL_RET

/* int mount(const char *src, const char *tgt, const char *fstype,
 *           long flags, const void *data);
 * Note: `flags` is a 32-bit long.  We pass only the low 16 bits — the
 * kernel zero-extends to uint32_t, and PPAP only honours MS_RDONLY=1
 * today, so this is a non-issue in practice. */
  .global mount
mount:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* src */
  movw  6(%bp), %cx      /* tgt */
  movw  8(%bp), %dx      /* fstype */
  movw  10(%bp), %si     /* flags low half (high half at 12(%bp) ignored) */
  movw  14(%bp), %di     /* data */
  movw  $0x0900, %ax
  int   $0x30
  SYSCALL_RET

/* int umount2(const char *tgt, long flags);
 * Same long-arg note as mount above. */
  .global umount2
umount2:
  pushw %bp
  movw  %sp, %bp
  pushw %bx
  pushw %si
  pushw %di
  movw  4(%bp), %bx      /* tgt */
  movw  6(%bp), %cx      /* flags low half */
  movw  $0x0901, %ax
  int   $0x30
  SYSCALL_RET
