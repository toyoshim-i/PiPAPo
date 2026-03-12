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

/* ── Syscall numbers (shared with user space) ────────────────────────────── */

#include "common/syscall_nr.h"

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
long sys_ptrace(long req, long pid, void *addr, void *data);
int trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5);
void trace_after_syscall(uint32_t *frame, uint32_t nr,
                         uint32_t a4, uint32_t a5, long ret);
int trace_before_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5);
void trace_after_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5,
                        int32_t ret);
void trace_debug_stop(uint32_t abi, uint32_t pc, uint32_t flags);
void trace_exec_stop(void);

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
long sys_rename(const char *oldpath, const char *newpath);
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
