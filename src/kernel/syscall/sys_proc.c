/*
 * sys_proc.c — Process-related syscall implementations
 *
 *   sys_exit(status)  — terminate the calling process
 *   sys_getpid()      — return the calling process's PID
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../errno.h"

/* ── sys_exit ───────────────────────────────────────────────────────────────── */

/*
 * Mark the calling process as ZOMBIE and yield the CPU.
 *
 * PendSV_Handler will call sched_next(), which skips ZOMBIE entries, so
 * this process will never be scheduled again.  The parent is responsible
 * for reaping the slot via proc_free() (waitpid, Phase 3+).
 *
 * Phase 1: the exit status is discarded — no parent to collect it.
 */
long sys_exit(long status)
{
    (void)status;
    current->state = PROC_ZOMBIE;
    sched_yield();  /* set PENDSVSET; PendSV fires after SVC_Handler returns */
    /* Unreachable: once ZOMBIE this process is never scheduled again.
     * The WFI spin ensures the CPU sleeps until PendSV fires and switches away. */
    for (;;)
        __asm__ volatile("wfi");
}

/* ── sys_getpid ─────────────────────────────────────────────────────────────── */

long sys_getpid(void)
{
    return (long)current->pid;
}

/* ── sys_execve ────────────────────────────────────────────────────────────── */
/*
 * Stub — returns -ENOSYS for now.  The full user-callable execve (replacing
 * the current process image via SVC) requires PSP manipulation that comes
 * with vfork in Phase 3 Step 5.  For Step 3, the kernel calls do_execve()
 * directly to create the init process.
 */

long sys_execve(const char *path)
{
    (void)path;
    return -(long)ENOSYS;
}
