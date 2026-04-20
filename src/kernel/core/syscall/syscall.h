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

#ifndef PPAP_KERNEL_CORE_SYSCALL_SYSCALL_H
#define PPAP_KERNEL_CORE_SYSCALL_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

typedef struct pcb pcb_t;

/* ── Syscall numbers (shared with user space) ────────────────────────────── */

#include "common/syscall_nr.h"
#include "kernel/common/core/page_types.h"

/* ── Dispatch ────────────────────────────────────────────────────────────────
 */

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
extern volatile int exec_pending[2];
extern volatile int svc_restart[2];
extern volatile uint32_t svc_saved_a0[2];
extern volatile uint32_t
    svc_exc_return[2]; /* EXC_RETURN saved by SVC_Handler (M33) */

/* Helper: mark current syscall for restart after blocking yield.
 * Sets both the global svc_restart (for ARM trap.S) and the per-process
 * flag (for m68k, where globals leak between nested TRAP #0 handlers). */
void svc_set_restart(void);

/* ── Syscall implementations ─────────────────────────────────────────────────
 */

/* sys_proc.c */
long sys_exit(long status);
long sys_getpid(void);
long sys_execve(page_id_t path_page, uint16_t path_off, uintptr_t argv_ptr,
                uintptr_t envp_ptr);
long sys_vfork(uint32_t *frame);
long sys_waitpid(long pid, long status_ptr, long options);
long sys_set_tid_address(uintptr_t tidptr);
long sys_uname(uintptr_t buf_ptr);
long sys_setpgid(long pid, long pgid);
long sys_setsid(void);
long sys_wait4(long pid, long status_ptr, long options, uintptr_t rusage_ptr);
long sys_ptrace(long req, long pid, uintptr_t addr, uintptr_t data_ptr);
int trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4,
                         uint32_t a5);
void trace_after_syscall(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5,
                         long ret);
int trace_before_subsys(uint32_t abi, uint32_t nr, uint32_t a0, uint32_t a1,
                        uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
void trace_after_subsys(uint32_t abi, uint32_t nr, uint32_t a0, uint32_t a1,
                        uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                        int32_t ret);
void trace_debug_stop(uint32_t abi, uint32_t pc, uint32_t flags);
int trace_has_swbp(void);
int trace_maybe_stop_at_swbp(uint32_t abi, uint32_t pc);
void trace_exec_stop(void);
void trace_arm_hwbp_on_switch(const pcb_t *next);
int trace_arm_hardfault_debug_stop(uint32_t *psp_frame);

long sys_poweroff(void);
void kernel_panic_halt(uint8_t status);

/* sys_io.c */
int sys_copy_from_user(void *dst, uintptr_t user_ptr, size_t len);
int sys_copy_to_user(uintptr_t user_ptr, const void *src, size_t len);
int sys_copy_user_string(char *dst, size_t dst_size, uintptr_t user_ptr);
/* Read/write `n` bytes starting at (page, off).  Callers with a user
 * pointer translate via proc_user_ptr_to_page_ref before calling. */
long sys_read(long fd, page_id_t page, uint16_t off, size_t n);
long sys_write(long fd, page_id_t page, uint16_t off, size_t n);
long sys_writev(long fd, uintptr_t iov, long iovcnt);
long sys_readv(long fd, uintptr_t iov, long iovcnt);
long sys_ioctl(long fd, long cmd, uintptr_t arg_ptr);

/* sys_time.c */
long sys_nanosleep(uintptr_t req_ptr, uintptr_t rem_ptr);
long sys_clock_gettime32(long clk_id, uintptr_t tp_ptr);
long sys_clock_gettime64(long clk_id, uintptr_t tp_ptr);
long sys_gettimeofday(uintptr_t tv_ptr, uintptr_t tz_ptr);
long sys_settimeofday(uintptr_t tv_ptr, uintptr_t tz_ptr);
long sys_clock_nanosleep32(long clk, long flags, uintptr_t req_ptr,
                           uintptr_t rem_ptr);
long sys_clock_nanosleep64(long clk, long flags, uintptr_t req_ptr,
                           uintptr_t rem_ptr);

/* Kernel-side wallclock helper.  Returns current Unix time as
 * (seconds, frac) where frac is [0, PPAP_TICK_HZ).  Single source of
 * truth for all time syscalls and subsystem bridges (DOS AH=2Ah/2Ch). */
void sys_time_now(uint32_t *sec_out, uint32_t *frac_ticks_out);

/* fd/pipe.c */
long sys_pipe(uintptr_t fds_ptr);

/* sys_mem.c */
long sys_brk(long addr);
long sys_mmap2(uintptr_t addr, size_t len, uint32_t prot, uint32_t flags,
               uint32_t fd, uint32_t pgoff);
long sys_munmap(uintptr_t addr, size_t len);

/* sys_fs.c — dup/dup2 */
long sys_dup(long oldfd);
long sys_dup2(long oldfd, long newfd);

/* signal/signal.c */
long sys_kill(long pid, long sig);
long sys_sigaction(long sig, long handler, long old_ptr);
long sys_sigreturn(void);
long sys_rt_sigaction(long sig, uintptr_t act_ptr, uintptr_t oact_ptr,
                      long sigsetsize);
long sys_rt_sigprocmask(long how, uintptr_t set_ptr, uintptr_t oset_ptr,
                        long sigsetsize);
long sys_rt_sigreturn(void);

/* sys_fs.c — VFS-routed file system calls */
struct stat;
struct dirent;
void fd_pool_init(void);
long sys_open(page_id_t page, uint16_t off, long flags, long mode);
long sys_close(long fd);
long sys_lseek(long fd, long off, long whence);
/* Path-taking calls take (page_id_t, uint16_t) for each path argument
 * (same convention as sys_open).  Buffer/struct args (e.g. stat
 * result, readlink output) keep `uintptr_t` and route through
 * sys_copy_to_user / sys_copy_from_user, where the user-pointer
 * translator is centralized. */
long sys_stat(page_id_t page, uint16_t off, uintptr_t buf_ptr);
long sys_fstat(long fd, uintptr_t buf_ptr);
long sys_getdents(long fd, uintptr_t buf_ptr, size_t count);
long sys_getcwd(uintptr_t buf_ptr, size_t size);
long sys_chdir(page_id_t page, uint16_t off);
long sys_mkdir(page_id_t page, uint16_t off, long mode);
long sys_unlink(page_id_t page, uint16_t off);
long sys_stat64(page_id_t page, uint16_t off, uintptr_t buf_ptr);
long sys_fstat64(long fd, uintptr_t buf_ptr);
long sys_lstat64(page_id_t page, uint16_t off, uintptr_t buf_ptr);
long sys_getdents64(long fd, uintptr_t buf_ptr, long count);
long sys_llseek(long fd, long off_hi, long off_lo, uintptr_t result_ptr,
                long whence);
long sys_fcntl64(long fd, long cmd, long arg);
long sys_access(page_id_t page, uint16_t off, long mode);
long sys_readlink(page_id_t page, uint16_t off, uintptr_t buf_ptr, long bufsiz);
long sys_rmdir(page_id_t page, uint16_t off);
long sys_rename(page_id_t old_page, uint16_t old_off, page_id_t new_page,
                uint16_t new_off);
long sys_umask(long mask);
/* sys_mount source is optional — pass page_id = PAGE_ID_INVALID for
 * "no source" (the dispatcher does this when user_ptr is NULL). */
long sys_mount(page_id_t source_page, uint16_t source_off,
               page_id_t target_page, uint16_t target_off,
               page_id_t fstype_page, uint16_t fstype_off, long flags,
               uintptr_t data_ptr);
long sys_umount2(page_id_t page, uint16_t off, long flags);
long sys_statfs64(page_id_t page, uint16_t off, long sz, uintptr_t buf_ptr);
long sys_fstatfs64(long fd, long sz, uintptr_t buf_ptr);

/* sys_poll.c */
long sys_poll(uintptr_t fds_ptr, uint32_t nfds, long timeout_ms);
long sys_ppoll(uintptr_t fds_ptr, uint32_t nfds, uintptr_t timeout_ptr,
               uintptr_t sigmask_ptr, uint32_t sigsetsize);
long sys_ppoll_time64(uintptr_t fds_ptr, uint32_t nfds, uintptr_t timeout_ptr,
                      uintptr_t sigmask_ptr, uint32_t sigsetsize);

#endif /* PPAP_KERNEL_CORE_SYSCALL_SYSCALL_H */
