/*
 * syscall.c — SVC dispatch table
 *
 * syscall_dispatch() is called from SVC_Handler (svc.S):
 *   frame[0..3] = stacked r0-r3 (syscall arguments a0-a3)
 *   nr          = syscall number (captured from r7 by svc.S)
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
 */

#include "syscall.h"
#include "../vfs/vfs.h"
#include "../errno.h"
#include <stdint.h>

/* Flag set by sys_execve to tell SVC_Handler to do a full context restore */
volatile int exec_pending = 0;

/* Syscall restart state — see syscall.h for the full explanation */
volatile int      svc_restart  = 0;
volatile uint32_t svc_saved_a0 = 0;

void syscall_dispatch(uint32_t *frame, uint32_t nr)
{
    long a0 = (long)frame[0];
    long a1 = (long)frame[1];
    long a2 = (long)frame[2];
    /* a3 (frame[3]) unused in Phase 1 */

    long ret;

    switch (nr) {
    case SYS_EXIT:
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
        ret = sys_execve((const char *)(uintptr_t)a0);
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
    case SYS_VFORK:
        ret = sys_vfork(frame);
        break;
    default:
        ret = -(long)ENOSYS;
        break;
    }

    frame[0] = (uint32_t)ret;   /* write return value into stacked r0 */
}
