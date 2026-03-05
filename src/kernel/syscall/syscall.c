/*
 * syscall.c — SVC dispatch table
 *
 * syscall_dispatch() is called from SVC_Handler (svc.S):
 *   frame[0..3] = stacked r0-r3 (syscall arguments a0-a3)
 *   nr          = syscall number (captured from r7 by svc.S)
 *   a4, a5      = 5th/6th arguments (captured from r4/r5 by svc.S)
 *
 * The return value is written into frame[0] (stacked r0) so the calling
 * thread sees it in r0 after the SVC exception returns.
 *
 * Implementations:
 *   sys_proc.c  — sys_exit, sys_getpid, sys_vfork, sys_waitpid, sys_execve
 *   sys_io.c    — sys_write, sys_read  (routes through fd_table → file_ops)
 *   sys_time.c  — sys_nanosleep
 *   sys_fs.c    — sys_open, sys_close, sys_lseek, sys_stat, sys_fstat,
 *                 sys_getdents, sys_getcwd, sys_chdir
 *   signal.c    — sys_kill, sys_sigaction, sys_sigreturn
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../vfs/vfs.h"
#include "../errno.h"
#include <stdint.h>

#ifdef SYSCALL_DEBUG
#include "../../drivers/uart.h"
#endif

/* SIGCHLD — needed for clone() fast-path detection */
#define SIGCHLD_NR 17

/* Per-core SVC state — indexed by core_id() (Step 9 converts assembly).
 * For single-core (Steps 6-7), C uses [0]; assembly accesses [0] implicitly
 * because the symbol address = &array[0]. */
volatile int      exec_pending[2] = {0, 0};
volatile int      svc_restart[2]  = {0, 0};
volatile uint32_t svc_saved_a0[2] = {0, 0};

void syscall_dispatch(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5)
{
    long a0 = (long)frame[0];
    long a1 = (long)frame[1];
    long a2 = (long)frame[2];
    long a3 = (long)frame[3];
    (void)a3; (void)a4; (void)a5; /* available for 4-6 arg syscalls */

    long ret;

#ifdef SYSCALL_DEBUG
    /* Trace interesting syscalls — filter high-frequency I/O to avoid flood */
    int _sc_trace = (nr != SYS_READ && nr != SYS_WRITE && nr != SYS_WRITEV
                  && nr != SYS_READV && nr != SYS_IOCTL
                  && nr != SYS_CLOCK_GETTIME32 && nr != SYS_CLOCK_GETTIME64
                  && nr != SYS_FSTAT64 && nr != SYS_STAT64
                  && nr != SYS_OPENAT && nr != SYS_CLOSE && nr != SYS_OPEN
                  && nr != SYS_FCNTL64 && nr != SYS_RT_SIGPROCMASK
                  && nr != SYS_GETPID && nr != SYS_MMAP2
                  && nr != SYS_SET_TID_ADDRESS && nr != SYS_BRK
                  && nr != SYS_RT_SIGACTION && nr != SYS_MPROTECT
                  && nr != SYS_GETUID32 && nr != SYS_GETEUID32
                  && nr != SYS_GETGID32 && nr != SYS_GETEGID32
                  && nr != SYS_FUTEX);
#endif

    switch (nr) {

    /* ── Existing syscalls (Phase 1-3) ──────────────────────────────────── */
    case SYS_EXIT:
    case SYS_EXIT_GROUP:       /* single-threaded: exit_group = exit */
        ret = sys_exit(a0);
        break;
    case SYS_READ:
        ret = sys_read(a0, (char *)(uintptr_t)a1, (size_t)a2);
        break;
    case SYS_WRITE:
        ret = sys_write(a0, (const char *)(uintptr_t)a1, (size_t)a2);
        break;
    case SYS_OPEN:
        ret = sys_open((const char *)(uintptr_t)a0, a1, a2);
        break;
    case SYS_CLOSE:
        ret = sys_close(a0);
        break;
    case SYS_EXECVE:
        ret = sys_execve((const char *)(uintptr_t)a0,
                         (const char *const *)(uintptr_t)a1);
        break;
    case SYS_CHDIR:
        ret = sys_chdir((const char *)(uintptr_t)a0);
        break;
    case SYS_LSEEK:
        ret = sys_lseek(a0, a1, a2);
        break;
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_STAT:
        ret = sys_stat((const char *)(uintptr_t)a0,
                       (struct stat *)(uintptr_t)a1);
        break;
    case SYS_FSTAT:
        ret = sys_fstat(a0, (struct stat *)(uintptr_t)a1);
        break;
    case SYS_GETDENTS:
        ret = sys_getdents(a0, (struct dirent *)(uintptr_t)a1, (size_t)a2);
        break;
    case SYS_NANOSLEEP:
        ret = sys_nanosleep((void *)(uintptr_t)a0, (void *)(uintptr_t)a1);
        break;
    case SYS_WAITPID:
        ret = sys_waitpid(a0, a1, a2);
        break;
    case SYS_GETCWD:
        ret = sys_getcwd((char *)(uintptr_t)a0, (size_t)a1);
        break;
    case SYS_DUP:
        ret = sys_dup(a0);
        break;
    case SYS_PIPE:
        ret = sys_pipe((int *)(uintptr_t)a0);
        break;
    case SYS_BRK:
        ret = sys_brk(a0);
        break;
    case SYS_DUP2:
        ret = sys_dup2(a0, a1);
        break;
    case SYS_KILL:
        ret = sys_kill(a0, a1);
        break;
    case SYS_SIGACTION:
        ret = sys_sigaction(a0, a1, a2);
        break;
    case SYS_SIGRETURN:
        ret = sys_sigreturn();
        break;
    case SYS_UNLINK:
        ret = sys_unlink((const char *)(uintptr_t)a0);
        break;
    case SYS_MKDIR:
        ret = sys_mkdir((const char *)(uintptr_t)a0, a1);
        break;
    case SYS_VFORK:
        ret = sys_vfork(frame);
        break;

    /* ── P0: musl boot-critical ─────────────────────────────────────────── */
    case SYS_FORK:             /* musl's vfork() calls fork(2) */
        ret = sys_vfork(frame);
        break;
    case SYS_CLONE:            /* musl's _Fork() uses clone(SIGCHLD, 0) */
        if (a0 == SIGCHLD_NR && a1 == 0)
            ret = sys_vfork(frame);
        else
            ret = -(long)ENOSYS;
        break;
    case SYS_SET_TID_ADDRESS:
        ret = sys_set_tid_address((void *)(uintptr_t)a0);
        break;
    case SYS_UNAME:
        ret = sys_uname((void *)(uintptr_t)a0);
        break;
    case SYS_WRITEV:
        ret = sys_writev(a0, (const void *)(uintptr_t)a1, a2);
        break;
    case SYS_READV:
        ret = sys_readv(a0, (const void *)(uintptr_t)a1, a2);
        break;
    case SYS_IOCTL:
        ret = sys_ioctl(a0, a1, a2);
        break;
    case SYS_MMAP2:
        ret = sys_mmap2((uint32_t)a0, (uint32_t)a1, (uint32_t)a2,
                        (uint32_t)a3, a4, a5);
        break;
    case SYS_MUNMAP:
        ret = sys_munmap((uint32_t)a0, (uint32_t)a1);
        break;
    case SYS_RT_SIGACTION:
        ret = sys_rt_sigaction(a0, (const void *)(uintptr_t)a1,
                               (void *)(uintptr_t)a2, a3);
        break;
    case SYS_RT_SIGPROCMASK:
        ret = sys_rt_sigprocmask(a0, (const void *)(uintptr_t)a1,
                                 (void *)(uintptr_t)a2, a3);
        break;
    case SYS_RT_SIGRETURN:
        ret = sys_rt_sigreturn();
        break;
    case SYS_FCNTL64:
        ret = sys_fcntl64(a0, a1, a2);
        break;
    case SYS_STAT64:
        ret = sys_stat64((const char *)(uintptr_t)a0,
                         (void *)(uintptr_t)a1);
        break;
    case SYS_FSTAT64:
        ret = sys_fstat64(a0, (void *)(uintptr_t)a1);
        break;
    case SYS_LSTAT64:
        ret = sys_lstat64((const char *)(uintptr_t)a0,
                          (void *)(uintptr_t)a1);
        break;
    case SYS_GETDENTS64:
        ret = sys_getdents64(a0, (void *)(uintptr_t)a1, a2);
        break;
    case SYS_WAIT4:
        ret = sys_wait4(a0, a1, a2, (void *)(uintptr_t)a3);
        break;
    case SYS_LLSEEK:
        ret = sys_llseek(a0, a1, a2, (void *)(uintptr_t)a3, (long)a4);
        break;
    case SYS_CLOCK_GETTIME32:
        ret = sys_clock_gettime32(a0, (void *)(uintptr_t)a1);
        break;
    case SYS_CLOCK_GETTIME64:
        ret = sys_clock_gettime64(a0, (void *)(uintptr_t)a1);
        break;

    /* ── P0: trivial return-0 stubs ─────────────────────────────────────── */
    case SYS_GETUID32:
    case SYS_GETEUID32:
    case SYS_GETGID32:
    case SYS_GETEGID32:
        ret = 0;               /* always root */
        break;
    case SYS_FUTEX:
        ret = 0;               /* single-threaded: locks are NOPs */
        break;
    case SYS_MPROTECT:
        ret = 0;               /* no MMU */
        break;

    /* ── P1: interactive shell ──────────────────────────────────────────── */
    case SYS_ACCESS:
        ret = sys_access((const char *)(uintptr_t)a0, a1);
        break;
    case SYS_READLINK:
        ret = sys_readlink((const char *)(uintptr_t)a0,
                           (char *)(uintptr_t)a1, a2);
        break;
    case SYS_GETPPID:
        ret = (long)current->ppid;
        break;
    case SYS_SETPGID:
        ret = sys_setpgid(a0, a1);
        break;
    case SYS_GETPGID: {
        /* getpgid(pid): pid==0 means self */
        pcb_t *t = current;
        if (a0 != 0) {
            t = NULL;
            for (uint32_t i = 0; i < PROC_MAX; i++) {
                if (proc_table[i].state != PROC_FREE &&
                    proc_table[i].pid == (pid_t)a0) {
                    t = &proc_table[i];
                    break;
                }
            }
            if (!t) { ret = -(long)ESRCH; break; }
        }
        ret = (long)t->pgid;
        break;
    }
    case SYS_SETSID:
        ret = sys_setsid();
        break;
    case SYS_UMASK:
        ret = sys_umask(a0);
        break;
    case SYS_RMDIR:
        ret = sys_rmdir((const char *)(uintptr_t)a0);
        break;
    case SYS_GETTIMEOFDAY:
        ret = sys_gettimeofday((void *)(uintptr_t)a0,
                               (void *)(uintptr_t)a1);
        break;
    case SYS_CLOCK_NANOSLEEP32:
        ret = sys_clock_nanosleep32(a0, a1,
                                    (const void *)(uintptr_t)a2,
                                    (void *)(uintptr_t)a3);
        break;
    case SYS_CLOCK_NANOSLEEP64:
        ret = sys_clock_nanosleep64(a0, a1,
                                    (const void *)(uintptr_t)a2,
                                    (void *)(uintptr_t)a3);
        break;
    case SYS_OPENAT:
        /* AT_FDCWD fast-path: route to sys_open (path=a1, flags=a2, mode=a3) */
        ret = sys_open((const char *)(uintptr_t)a1, a2, a3);
        break;
    case SYS_FSTATAT64:
        /* AT_FDCWD fast-path: dirfd ignored, route to stat64/lstat64 */
        ret = sys_stat64((const char *)(uintptr_t)a1,
                         (void *)(uintptr_t)a2);
        break;
    case SYS_MREMAP:
        ret = -(long)ENOMEM;   /* force malloc to mmap new region */
        break;

    /* ── P2: stubs for applet completeness ──────────────────────────────── */
    case SYS_MKNOD:
        ret = -(long)EPERM;
        break;
    case SYS_CHMOD:
        ret = 0;               /* stub — no real permission model */
        break;
    case SYS_RENAME:
        ret = -(long)ENOSYS;
        break;
    case SYS_LCHOWN32:
    case SYS_FCHOWN32:
    case SYS_CHOWN32:
    case SYS_SETGROUPS32:
        ret = 0;               /* single-user: ownership ops succeed */
        break;
    case SYS_GETCPU:
        ret = (long)core_id();
        break;
    case SYS_UTIMES:
        ret = 0;               /* no RTC — timestamp update is no-op */
        break;
    case SYS_STATX:
        ret = -(long)ENOSYS;   /* stat64 suffices */
        break;

    /* ── P3: mount / umount / statfs ──────────────────────────────────────── */
    case SYS_MOUNT:
        ret = sys_mount((const char *)(uintptr_t)a0,
                        (const char *)(uintptr_t)a1,
                        (const char *)(uintptr_t)a2,
                        a3, (const void *)(uintptr_t)a4);
        break;
    case SYS_UMOUNT2:
        ret = sys_umount2((const char *)(uintptr_t)a0, a1);
        break;
    case SYS_STATFS64:
        ret = sys_statfs64((const char *)(uintptr_t)a0, a1,
                           (void *)(uintptr_t)a2);
        break;
    case SYS_FSTATFS64:
        ret = sys_fstatfs64(a0, a1, (void *)(uintptr_t)a2);
        break;

    /* ── P4: poll / blocking I/O ─────────────────────────────────────────── */
    case SYS_PPOLL:
        ret = sys_ppoll((void *)(uintptr_t)a0, (uint32_t)a1,
                        (const void *)(uintptr_t)a2,
                        (const void *)(uintptr_t)a3, a4);
        break;
    case SYS_PPOLL_TIME64:
        ret = sys_ppoll_time64((void *)(uintptr_t)a0, (uint32_t)a1,
                               (const void *)(uintptr_t)a2,
                               (const void *)(uintptr_t)a3, a4);
        break;

    default:
#ifdef SYSCALL_DEBUG
        uart_puts("ENOSYS: syscall ");
        uart_print_dec((uint32_t)nr);
        uart_putc('\n');
#endif
        ret = -(long)ENOSYS;
        break;
    }

#ifdef SYSCALL_DEBUG
    if (_sc_trace || ret == -(long)ENOSYS) {
        uart_puts("SC ");
        uart_print_dec(nr);
        uart_puts("(");
        uart_print_hex32((uint32_t)a0);
        uart_puts(",");
        uart_print_hex32((uint32_t)a1);
        uart_puts(")=");
        uart_print_hex32((uint32_t)ret);
        uart_putc('\n');
    }
#endif

    frame[0] = (uint32_t)ret;   /* write return value into stacked r0 */
}
