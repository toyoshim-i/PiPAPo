/*
 * syscall.h — Syscall numbers and dispatch interface
 *
 * ARM EABI Linux convention (compatible with musl libc):
 *   r7    = syscall number
 *   r0–r3 = arguments 1–4 (hardware-stacked by Cortex-M0+)
 *   r4    = argument 5 (callee-saved, captured by SVC_Handler)
 *   r5    = argument 6 (callee-saved, captured by SVC_Handler)
 *   r0    = return value (negative errno on error)
 *   svc 0 triggers the SVC exception
 *
 * SVC_Handler (svc.S) captures r4, r5, r7 before the compiler can
 * clobber them, reads the stacked r0–r3 exception frame from PSP,
 * and calls syscall_dispatch(frame, nr, a4, a5).  syscall_dispatch()
 * writes the return value back into the stacked r0 so the caller sees
 * it in r0 after exception return.
 */

#ifndef PPAP_SYSCALL_H
#define PPAP_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ── Syscall numbers (ARM EABI Linux-compatible) ───────────────────────────── */

/* Existing (Phase 1-3) */
#define SYS_EXIT         1
#define SYS_FORK         2
#define SYS_READ         3
#define SYS_WRITE        4
#define SYS_OPEN         5
#define SYS_CLOSE        6
#define SYS_WAITPID      7
#define SYS_UNLINK      10
#define SYS_EXECVE      11
#define SYS_CHDIR       12
#define SYS_MKNOD       14
#define SYS_CHMOD       15
#define SYS_LSEEK       19
#define SYS_GETPID      20
#define SYS_MOUNT       21
#define SYS_ACCESS      33
#define SYS_KILL        37
#define SYS_RENAME      38
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_DUP         41
#define SYS_PIPE        42
#define SYS_BRK         45
#define SYS_UMOUNT2     52
#define SYS_IOCTL       54
#define SYS_SETPGID     57
#define SYS_UMASK       60
#define SYS_DUP2        63
#define SYS_GETPPID     64
#define SYS_SETSID      66
#define SYS_SIGACTION   67
#define SYS_GETTIMEOFDAY 78
#define SYS_READLINK    85
#define SYS_MUNMAP      91
#define SYS_STAT       106
#define SYS_FSTAT      108
#define SYS_SIGRETURN  119
#define SYS_CLONE      120
#define SYS_UNAME      122
#define SYS_MPROTECT   125
#define SYS_GETPGID    132
#define SYS_LLSEEK     140
#define SYS_GETDENTS   141
#define SYS_READV      145
#define SYS_WRITEV     146
#define SYS_NANOSLEEP  162
#define SYS_MREMAP     163
#define SYS_RT_SIGRETURN   173
#define SYS_RT_SIGACTION   174
#define SYS_RT_SIGPROCMASK 175
#define SYS_GETCWD     183
#define SYS_VFORK      190
#define SYS_MMAP2      192
#define SYS_STAT64     195
#define SYS_LSTAT64    196
#define SYS_FSTAT64    197
#define SYS_LCHOWN32   198
#define SYS_GETUID32   199
#define SYS_GETGID32   200
#define SYS_GETEUID32  201
#define SYS_GETEGID32  202
#define SYS_SETGROUPS32 206
#define SYS_FCHOWN32   207
#define SYS_CHOWN32    212
#define SYS_GETDENTS64 217
#define SYS_FCNTL64    221
#define SYS_FUTEX      240
#define SYS_EXIT_GROUP 248
#define SYS_SET_TID_ADDRESS 256
#define SYS_CLOCK_GETTIME32 263
#define SYS_CLOCK_NANOSLEEP32 265
#define SYS_STATFS64   266
#define SYS_FSTATFS64  267
#define SYS_UTIMES     269
#define SYS_WAIT4      114
#define SYS_OPENAT     322
#define SYS_FSTATAT64  327
#define SYS_GETCPU     345
#define SYS_STATX      397
#define SYS_PPOLL            336
#define SYS_CLOCK_GETTIME64  403
#define SYS_CLOCK_NANOSLEEP64 407
#define SYS_PPOLL_TIME64     414

/* AT_FDCWD: musl's *at syscalls use this as dirfd for cwd-relative paths */
#define AT_FDCWD       (-100)

/* ── Dispatch ──────────────────────────────────────────────────────────────── */

/*
 * Called from SVC_Handler (svc.S) with:
 *   frame[0..3] = stacked r0-r3 (syscall arguments a0-a3)
 *   nr          = syscall number (captured from r7)
 *   a4          = 5th argument (captured from r4)
 *   a5          = 6th argument (captured from r5)
 *
 * Dispatches to the appropriate sys_* implementation and writes the return
 * value into frame[0] (stacked r0) so the caller sees it after SVC return.
 */
void syscall_dispatch(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5);

/* Per-core SVC state arrays — indexed by core_id() for dual-core.
 * Assembly (svc.S) accesses [0] implicitly via the symbol base address;
 * Step 9 converts assembly to use core_id() indexing.
 *
 * exec_pending: set by sys_execve to tell SVC_Handler to do a full
 *   context restore from the new process image (r9/GOT base).
 *
 * svc_restart / svc_saved_a0: blocking syscalls set svc_restart[N] = 1.
 *   SVC_Handler detects this, restores frame[0] from svc_saved_a0[N], and
 *   adjusts the stacked PC back by 2 bytes (size of "svc 0").  When the
 *   process is rescheduled, the SVC re-executes with original arguments. */
extern volatile int      exec_pending[2];
extern volatile int      svc_restart[2];
extern volatile uint32_t svc_saved_a0[2];

/* ── Syscall implementations ───────────────────────────────────────────────── */
/*
 * Declared here; defined in syscall.c (stubs) until Steps 9-10 wire them
 * up to the process table, fd table, and sleep timer.
 */

/* sys_proc.c */
long sys_exit(long status);
long sys_getpid(void);
long sys_execve(const char *path, const char *const *argv);
long sys_vfork(uint32_t *frame);
long sys_waitpid(long pid, long status_ptr, long options);
long sys_set_tid_address(void *tidptr);
long sys_uname(void *buf);
long sys_setpgid(long pid, long pgid);
long sys_setsid(void);
long sys_wait4(long pid, long status_ptr, long options, void *rusage);

/* sys_io.c */
long sys_read(long fd, char *buf, size_t n);
long sys_write(long fd, const char *buf, size_t n);
long sys_writev(long fd, const void *iov, long iovcnt);
long sys_readv(long fd, const void *iov, long iovcnt);
long sys_ioctl(long fd, long cmd, long arg);

/* sys_time.c */
long sys_nanosleep(void *req, void *rem);
long sys_clock_gettime32(long clk_id, void *tp);
long sys_clock_gettime64(long clk_id, void *tp);
long sys_gettimeofday(void *tv, void *tz);
long sys_clock_nanosleep32(long clk, long flags, const void *req, void *rem);
long sys_clock_nanosleep64(long clk, long flags, const void *req, void *rem);

/* fd/pipe.c */
long sys_pipe(int *fds);

/* sys_mem.c */
long sys_brk(long addr);
long sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot,
               uint32_t flags, uint32_t fd, uint32_t pgoff);
long sys_munmap(uint32_t addr, uint32_t len);

/* sys_fs.c — dup/dup2 */
long sys_dup(long oldfd);
long sys_dup2(long oldfd, long newfd);

/* signal/signal.c */
long sys_kill(long pid, long sig);
long sys_sigaction(long sig, long handler, long old_ptr);
long sys_sigreturn(void);
long sys_rt_sigaction(long sig, const void *act, void *oact, long sigsetsize);
long sys_rt_sigprocmask(long how, const void *set, void *oset, long sigsetsize);
long sys_rt_sigreturn(void);

/* sys_fs.c — VFS-routed file system calls */
struct stat;
struct dirent;
void file_pool_init(void);
long sys_open(const char *path, long flags, long mode);
long sys_close(long fd);
long sys_lseek(long fd, long off, long whence);
long sys_stat(const char *path, struct stat *buf);
long sys_fstat(long fd, struct stat *buf);
long sys_getdents(long fd, struct dirent *buf, size_t count);
long sys_getcwd(char *buf, size_t size);
long sys_chdir(const char *path);
long sys_mkdir(const char *path, long mode);
long sys_unlink(const char *path);
long sys_stat64(const char *path, void *buf);
long sys_fstat64(long fd, void *buf);
long sys_lstat64(const char *path, void *buf);
long sys_getdents64(long fd, void *buf, long count);
long sys_llseek(long fd, long off_hi, long off_lo, void *result, long whence);
long sys_fcntl64(long fd, long cmd, long arg);
long sys_access(const char *path, long mode);
long sys_readlink(const char *path, char *buf, long bufsiz);
long sys_rmdir(const char *path);
long sys_umask(long mask);
long sys_mount(const char *source, const char *target,
               const char *fstype, long flags, const void *data);
long sys_umount2(const char *target, long flags);
long sys_statfs64(const char *path, long sz, void *buf);
long sys_fstatfs64(long fd, long sz, void *buf);

/* sys_poll.c */
long sys_ppoll(void *fds, uint32_t nfds, const void *timeout,
               const void *sigmask, uint32_t sigsetsize);
long sys_ppoll_time64(void *fds, uint32_t nfds, const void *timeout,
                      const void *sigmask, uint32_t sigsetsize);

#endif /* PPAP_SYSCALL_H */
