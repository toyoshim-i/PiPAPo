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
 *   sys_proc.c  — sys_exit, sys_getpid
 *   sys_io.c    — sys_write, sys_read  (routes through fd_table → file_ops)
 *   sys_time.c  — sys_nanosleep
 */

#include "syscall.h"
#include "../errno.h"
#include <stdint.h>

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
    case SYS_GETPID:
        ret = sys_getpid();
        break;
    case SYS_NANOSLEEP:
        ret = sys_nanosleep((void *)(uintptr_t)a0, (void *)(uintptr_t)a1);
        break;
    default:
        ret = -(long)ENOSYS;
        break;
    }

    frame[0] = (uint32_t)ret;   /* write return value into stacked r0 */
}
