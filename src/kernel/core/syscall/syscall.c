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
 *   signal.c    — sys_kill, sys_rt_sigaction, sys_rt_sigreturn
 */

#include "kernel/core/syscall/syscall.h"

#include <stdint.h>

#include "common/errno.h"
#include "common/ptrace.h"
#include "kernel/common/mod/mod_vfs.h"
#include "kernel/core/proc/proc.h"

/* SIGCHLD — needed for clone() fast-path detection */
#define SIGCHLD_NR 17

/* Per-core SVC state — indexed by core_id() (Step 9 converts assembly).
 * For single-core (Steps 6-7), C uses [0]; assembly accesses [0] implicitly
 * because the symbol address = &array[0]. */
volatile int exec_pending[2] = {0, 0};
volatile int syscall_restart[2] = {0, 0};
volatile uint32_t syscall_saved_arg0[2] = {0, 0};
volatile uint32_t svc_exc_return[2] = {0, 0};

#if defined(__ia16__)
static long ia16_sign_extend_arg(long value) {
  return (long)(int32_t)(int16_t)(uint16_t)value;
}
#endif

#if defined(KSTACK_USAGE_TRACK) && \
    (defined(__arm__) || defined(__thumb__) || defined(__ARM_ARCH))
#define SYSCALL_TRACK_KSTACK 1
#endif

void syscall_set_restart(void) {
  syscall_restart[core_id()] = 1;
  current->syscall_needs_restart = 1;
}

/* Translate a user pointer into a (page, off) pair for path/buffer
 * syscalls.  Centralises the per-arch translator step at the
 * dispatcher boundary so each path syscall body works on (page, off)
 * directly and doesn't depend on the user-pointer convention. */
static int xlate_user_ptr(uintptr_t up, user_page_ref_t *ref) {
  if (up == 0u) return -(long)EINVAL;
  if (proc_user_ptr_to_page_ref(current, up, ref) < 0) return -(long)EFAULT;
  return 0;
}

/* Variant that accepts NULL and returns it as (PAGE_ID_INVALID, 0).
 * Used by sys_mount where source_ptr is documented as optional. */
static int xlate_user_ptr_optional(uintptr_t up, user_page_ref_t *ref) {
  if (up == 0u) {
    ref->page = PAGE_ID_INVALID;
    ref->off = 0;
    return 0;
  }
  if (proc_user_ptr_to_page_ref(current, up, ref) < 0) return -(long)EFAULT;
  return 0;
}

void syscall_dispatch(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5) {
  long a0 = (long)frame[0];
  long a1 = (long)frame[1];
  long a2 = (long)frame[2];
  long a3 = (long)frame[3];
  (void)a3;
  (void)a4;
  (void)a5; /* available for 4-6 arg syscalls */

#if defined(__ia16__)
  /* The ia16 user ABI passes syscall args in 16-bit registers.  Most args are
   * pointers, sizes, or flags and should remain zero-extended, but signed
   * process-control values such as waitpid(-1, ...) need explicit sign
   * extension before the shared syscall layer interprets them as 32-bit long.
   */
  switch (nr) {
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
    case SYS_WAITPID:
    case SYS_WAIT4:
    case SYS_GETPGID:
      a0 = ia16_sign_extend_arg(a0);
      break;
    case SYS_SETPGID:
    case SYS_KILL:
      a0 = ia16_sign_extend_arg(a0);
      a1 = ia16_sign_extend_arg(a1);
      break;
    default:
      break;
  }
#endif

  long ret;

#ifdef SYSCALL_TRACK_KSTACK
  proc_kstack_paint();
#endif

  if (trace_before_syscall(frame, nr, a4, a5)) {
#ifdef SYSCALL_TRACK_KSTACK
    (void)proc_kstack_scan();
#endif
    return;
  }

  switch (nr) {
    /* ── Core POSIX syscalls ─────────────────────────────────────────────── */
    case SYS_EXIT:
    case SYS_EXIT_GROUP: /* single-threaded: exit_group = exit */
      ret = sys_exit(a0);
      break;
    case SYS_READ: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &r)) != 0) break;
      ret = sys_read(a0, r.page, r.off, (size_t)a2);
    } break;
    case SYS_WRITE: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &r)) != 0) break;
      ret = sys_write(a0, r.page, r.off, (size_t)a2);
    } break;
    case SYS_OPEN: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_open(r.page, r.off, a1, a2);
    } break;
    case SYS_CLOSE:
      ret = sys_close(a0);
      break;
    case SYS_EXECVE: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_execve(r.page, r.off, (uintptr_t)a1, (uintptr_t)a2);
    } break;
    case SYS_CHDIR: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_chdir(r.page, r.off);
    } break;
    case SYS_LSEEK:
      ret = sys_lseek(a0, a1, a2);
      break;
    case SYS_GETPID:
      ret = sys_getpid();
      break;
    case SYS_STAT: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_stat(r.page, r.off, (uintptr_t)a1);
    } break;
    case SYS_FSTAT:
      ret = sys_fstat(a0, (uintptr_t)a1);
      break;
    case SYS_GETDENTS:
      ret = sys_getdents(a0, (uintptr_t)a1, (size_t)a2);
      break;
    case SYS_NANOSLEEP:
      ret = sys_nanosleep((uintptr_t)a0, (uintptr_t)a1);
      break;
    case SYS_WAITPID:
      ret = sys_waitpid(a0, a1, a2);
      break;
    case SYS_GETCWD:
      ret = sys_getcwd((uintptr_t)a0, (size_t)a1);
      break;
    case SYS_DUP:
      ret = sys_dup(a0);
      break;
    case SYS_PIPE:
      ret = sys_pipe((uintptr_t)a0);
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
    case SYS_UNLINK: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_unlink(r.page, r.off);
    } break;
    case SYS_MKDIR: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_mkdir(r.page, r.off, a1);
    } break;
    case SYS_VFORK:
      ret = sys_vfork(frame);
      break;

    /* ── libc boot-critical ────────────────────────────────────────────── */
    case SYS_FORK: /* libc vfork() falls back to fork(2) */
      ret = sys_vfork(frame);
      break;
    case SYS_PTRACE:
      ret = sys_ptrace(a0, a1, (uintptr_t)a2, (uintptr_t)a3);
      break;
    case SYS_CLONE: /* libc _Fork() uses clone(SIGCHLD, 0) */
      if (a0 == SIGCHLD_NR && a1 == 0)
        ret = sys_vfork(frame);
      else
        ret = -(long)ENOSYS;
      break;
    case SYS_SET_TID_ADDRESS:
      ret = sys_set_tid_address((uintptr_t)a0);
      break;
    case SYS_UNAME:
      ret = sys_uname((uintptr_t)a0);
      break;
    case SYS_WRITEV:
      ret = sys_writev(a0, (uintptr_t)a1, a2);
      break;
    case SYS_READV:
      ret = sys_readv(a0, (uintptr_t)a1, a2);
      break;
    case SYS_IOCTL:
      ret = sys_ioctl(a0, a1, (uintptr_t)a2);
      break;
    case SYS_MMAP2:
      ret = sys_mmap2((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3,
                      a4, a5);
      break;
    case SYS_MUNMAP:
      ret = sys_munmap((uint32_t)a0, (uint32_t)a1);
      break;
    case SYS_RT_SIGACTION:
      ret = sys_rt_sigaction(a0, (uintptr_t)a1, (uintptr_t)a2, a3);
      break;
    case SYS_RT_SIGPROCMASK:
      ret = sys_rt_sigprocmask(a0, (uintptr_t)a1, (uintptr_t)a2, a3);
      break;
    case SYS_RT_SIGRETURN:
      ret = sys_rt_sigreturn();
      break;
    case SYS_FCNTL:
      ret = sys_fcntl64(a0, a1, a2);
      break;
    case SYS_STAT64: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_stat64(r.page, r.off, (uintptr_t)a1);
    } break;
    case SYS_FSTAT64:
      ret = sys_fstat64(a0, (uintptr_t)a1);
      break;
    case SYS_LSTAT64: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_lstat64(r.page, r.off, (uintptr_t)a1);
    } break;
    case SYS_GETDENTS64:
      ret = sys_getdents64(a0, (uintptr_t)a1, a2);
      break;
    case SYS_WAIT4:
      ret = sys_wait4(a0, a1, a2, (uintptr_t)a3);
      break;
    case SYS_LLSEEK:
      ret = sys_llseek(a0, a1, a2, (uintptr_t)a3, (long)a4);
      break;
    case SYS_CLOCK_GETTIME32:
      ret = sys_clock_gettime32(a0, (uintptr_t)a1);
      break;
    case SYS_CLOCK_GETTIME64:
      ret = sys_clock_gettime64(a0, (uintptr_t)a1);
      break;

    /* ── Trivial return-0 stubs ─────────────────────────────────────────── */
    case SYS_GETUID:
    case SYS_GETEUID:
    case SYS_GETGID:
    case SYS_GETEGID:
      ret = 0; /* always root */
      break;
    case SYS_FUTEX:
      ret = 0; /* single-threaded: locks are NOPs */
      break;
    case SYS_MPROTECT:
      ret = 0; /* no MMU */
      break;

    /* ── Interactive shell ──────────────────────────────────────────────── */
    case SYS_ACCESS: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_access(r.page, r.off, a1);
    } break;
    case SYS_READLINK: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_readlink(r.page, r.off, (uintptr_t)a1, a2);
    } break;
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
        if (!t) {
          ret = -(long)ESRCH;
          break;
        }
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
    case SYS_RMDIR: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_rmdir(r.page, r.off);
    } break;
    case SYS_GETTIMEOFDAY:
      ret = sys_gettimeofday((uintptr_t)a0, (uintptr_t)a1);
      break;
    case SYS_SETTIMEOFDAY:
      ret = sys_settimeofday((uintptr_t)a0, (uintptr_t)a1);
      break;
    case SYS_CLOCK_NANOSLEEP32:
      ret = sys_clock_nanosleep32(a0, a1, (uintptr_t)a2, (uintptr_t)a3);
      break;
    case SYS_CLOCK_NANOSLEEP64:
      ret = sys_clock_nanosleep64(a0, a1, (uintptr_t)a2, (uintptr_t)a3);
      break;
    case SYS_OPENAT: {
      /* AT_FDCWD fast-path: route to sys_open (path=a1, flags=a2, mode=a3) */
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &r)) != 0) break;
      ret = sys_open(r.page, r.off, a2, a3);
    } break;
    case SYS_FSTATAT64: {
      /* AT_FDCWD fast-path: dirfd ignored, route to stat64. */
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &r)) != 0) break;
      ret = sys_stat64(r.page, r.off, (uintptr_t)a2);
    } break;
    case SYS_MREMAP:
      ret = -(long)ENOMEM; /* force malloc to mmap new region */
      break;

    /* ── Stubs for applet completeness ──────────────────────────────────── */
    case SYS_MKNOD:
      ret = -(long)EPERM;
      break;
    case SYS_CHMOD: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_chmod(r.page, r.off, a1);
    } break;
    case SYS_RENAME: {
      user_page_ref_t ro, rn;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &ro)) != 0) break;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &rn)) != 0) break;
      ret = sys_rename(ro.page, ro.off, rn.page, rn.off);
    } break;
    case SYS_LINK: {
      user_page_ref_t ro, rn;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &ro)) != 0) break;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &rn)) != 0) break;
      ret = sys_link(ro.page, ro.off, rn.page, rn.off);
    } break;
    case SYS_LCHOWN:
    case SYS_FCHOWN:
    case SYS_CHOWN:
    case SYS_SETGROUPS:
      ret = 0; /* single-user: ownership ops succeed */
      break;
    case SYS_GETCPU:
      ret = (long)core_id();
      break;
    case SYS_UTIMES: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_utimes(r.page, r.off, (uintptr_t)a1);
    } break;
    case SYS_STATX:
      ret = -(long)ENOSYS; /* stat64 suffices */
      break;

    /* ── mount / umount / statfs ────────────────────────────────────────────
     */
    case SYS_MOUNT: {
      user_page_ref_t rs, rt, rf;
      /* source is optional; target and fstype are required. */
      if ((ret = xlate_user_ptr_optional((uintptr_t)a0, &rs)) != 0) break;
      if ((ret = xlate_user_ptr((uintptr_t)a1, &rt)) != 0) break;
      if ((ret = xlate_user_ptr((uintptr_t)a2, &rf)) != 0) break;
      ret = sys_mount(rs.page, rs.off, rt.page, rt.off, rf.page, rf.off, a3,
                      (uintptr_t)a4);
    } break;
    case SYS_UMOUNT2: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_umount2(r.page, r.off, a1);
    } break;
    case SYS_STATFS64: {
      user_page_ref_t r;
      if ((ret = xlate_user_ptr((uintptr_t)a0, &r)) != 0) break;
      ret = sys_statfs64(r.page, r.off, a1, (uintptr_t)a2);
    } break;
    case SYS_FSTATFS64:
      ret = sys_fstatfs64(a0, a1, (uintptr_t)a2);
      break;

    /* ── poll / blocking I/O ─────────────────────────────────────────────── */
    case SYS_POLL:
      ret = sys_poll((uintptr_t)a0, (uint32_t)a1, a2);
      break;
    case SYS_PPOLL:
      ret = sys_ppoll((uintptr_t)a0, (uint32_t)a1, (uintptr_t)a2, (uintptr_t)a3,
                      a4);
      break;

    /* ── System control ──────────────────────────────────────────────────── */
    case SYS_POWEROFF:
      ret = sys_poweroff();
      break;

    /* ── TLS: set/get_thread_area (m68k Linux libc uses these for TLS) ─── */
    case 0xF0A8: /* __NR_get_thread_area */
      ret = (long)current->tp_value;
      break;
    case 0xF0A9: /* __NR_set_thread_area */
      current->tp_value = (uint32_t)a0;
      ret = 0;
      break;

    default:
      ret = -(long)ENOSYS;
      break;
  }

  trace_after_syscall(frame, nr, a4, a5, ret);
#ifdef SYSCALL_TRACK_KSTACK
  {
    uint16_t hwm = proc_kstack_scan();
    if (hwm)
      mod_vfs.klogf("KSTACK: slot hwm=%u/%u nr=%u\n", (unsigned)hwm,
                    (unsigned)proc_kstack_capacity(), (unsigned)nr);
  }
#endif
  frame[0] = (uint32_t)ret; /* write return value into stacked r0 */
}
