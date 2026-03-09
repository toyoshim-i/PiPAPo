/*
 * syscall.h — PPAP syscall numbers and dispatch interface
 *
 * PPAP syscall convention:
 *   ARM:  r7 = syscall number, r0–r3 = args 1–4, r4/r5 = args 5–6,
 *         svc 0 triggers SVC exception, return value in r0.
 *   m68k: d0 = syscall number, d1–d5 = args 1–5, a0 = arg 6,
 *         trap #0 triggers syscall, return value in d0.
 *
 * Syscall numbers use a 16-bit scheme: high byte = group, low byte = index.
 * Each group has 256 slots for future expansion.
 */

#ifndef PPAP_SYSCALL_H
#define PPAP_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* ── Syscall numbers ──────────────────────────────────────────────────────────
 *
 * 16-bit numbering: group (high byte) | index (low byte).
 * Architecture-independent — same numbers on ARM and m68k.
 */

/* Group 0x00: Process lifecycle */
#define SYS_EXIT              0x0000
#define SYS_EXIT_GROUP        0x0001
#define SYS_VFORK             0x0002
#define SYS_EXECVE            0x0003
#define SYS_WAITPID           0x0004
#define SYS_WAIT4             0x0005
#define SYS_GETPID            0x0006
#define SYS_UNAME             0x0007
#define SYS_GETPPID           0x0008
#define SYS_SETPGID           0x0009
#define SYS_GETPGID           0x000A
#define SYS_SETSID            0x000B
#define SYS_CLONE             0x000C
#define SYS_SET_TID_ADDRESS   0x000D
#define SYS_FORK              0x000E

/* Group 0x01: I/O */
#define SYS_READ              0x0100
#define SYS_WRITE             0x0101
#define SYS_OPEN              0x0102
#define SYS_CLOSE             0x0103
#define SYS_DUP               0x0104
#define SYS_DUP2              0x0105
#define SYS_PIPE              0x0106
#define SYS_IOCTL             0x0107
#define SYS_FCNTL             0x0108
#define SYS_READV             0x0109
#define SYS_WRITEV            0x010A
#define SYS_LSEEK             0x010B

/* Group 0x02: File system — paths */
#define SYS_STAT              0x0200
#define SYS_FSTAT             0x0201
#define SYS_ACCESS            0x0202
#define SYS_GETCWD            0x0203
#define SYS_MKDIR             0x0204
#define SYS_RMDIR             0x0205
#define SYS_UNLINK            0x0206
#define SYS_CHDIR             0x0207
#define SYS_READLINK          0x0208
#define SYS_RENAME            0x0209
#define SYS_MKNOD             0x020A
#define SYS_CHMOD             0x020B
#define SYS_OPENAT            0x020C
#define SYS_FSTATAT64         0x020D

/* Group 0x03: File system — extended */
#define SYS_GETDENTS          0x0300
#define SYS_STAT64            0x0301
#define SYS_FSTAT64           0x0302
#define SYS_UMASK             0x0303
#define SYS_LSTAT64           0x0304
#define SYS_STATFS64          0x0305
#define SYS_FSTATFS64         0x0306
#define SYS_LLSEEK            0x0307
#define SYS_STATX             0x0308
#define SYS_UTIMES            0x0309
#define SYS_GETDENTS64        0x030A

/* Group 0x04: Memory management */
#define SYS_BRK               0x0400
#define SYS_MMAP2             0x0401
#define SYS_MUNMAP            0x0402
#define SYS_MPROTECT          0x0403
#define SYS_MREMAP            0x0404

/* Group 0x05: Time */
#define SYS_NANOSLEEP         0x0500
#define SYS_CLOCK_GETTIME32   0x0501
#define SYS_GETTIMEOFDAY      0x0502
#define SYS_CLOCK_NANOSLEEP32 0x0503
#define SYS_CLOCK_GETTIME64   0x0504
#define SYS_CLOCK_NANOSLEEP64 0x0505

/* Group 0x06: Signals */
#define SYS_KILL              0x0600
#define SYS_SIGACTION         0x0601
#define SYS_SIGRETURN         0x0602
#define SYS_RT_SIGACTION      0x0603
#define SYS_RT_SIGPROCMASK    0x0604
#define SYS_RT_SIGRETURN      0x0605

/* Group 0x07: Poll */
#define SYS_POLL              0x0700
#define SYS_PPOLL             0x0701

/* Group 0x08: User/group identity */
#define SYS_GETUID            0x0800
#define SYS_GETGID            0x0801
#define SYS_GETEUID           0x0802
#define SYS_GETEGID           0x0803
#define SYS_CHOWN             0x0804
#define SYS_LCHOWN            0x0805
#define SYS_SETGROUPS         0x0806
#define SYS_FCHOWN            0x0807

/* Group 0x09: Mount / filesystem ops */
#define SYS_MOUNT             0x0900
#define SYS_UMOUNT2           0x0901

/* Group 0x0A: Misc */
#define SYS_FUTEX             0x0A00
#define SYS_GETCPU            0x0A01

/* AT_FDCWD: *at syscalls use this as dirfd for cwd-relative paths */
#define AT_FDCWD       (-100)

/* ── Dispatch ──────────────────────────────────────────────────────────────── */

/*
 * Called from the arch-specific trap handler with:
 *   frame[0..3] = syscall arguments a0-a3
 *   nr          = syscall number
 *   a4          = 5th argument
 *   a5          = 6th argument
 *
 * Dispatches to the appropriate sys_* implementation and writes the return
 * value into frame[0] so the caller sees it after trap return.
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

/* Helper: mark current syscall for restart after blocking yield.
 * Sets both the global svc_restart (for ARM trap.S) and the per-process
 * flag (for m68k, where globals leak between nested TRAP #0 handlers). */
void set_svc_restart(void);

/* ── Syscall implementations ───────────────────────────────────────────────── */

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
long sys_poll(void *fds, uint32_t nfds, long timeout_ms);
long sys_ppoll(void *fds, uint32_t nfds, const void *timeout,
               const void *sigmask, uint32_t sigsetsize);
long sys_ppoll_time64(void *fds, uint32_t nfds, const void *timeout,
                      const void *sigmask, uint32_t sigsetsize);

#endif /* PPAP_SYSCALL_H */
