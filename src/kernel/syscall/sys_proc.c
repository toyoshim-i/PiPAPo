/*
 * sys_proc.c — Process-related syscall implementations
 *
 *   sys_exit(status)    — terminate the calling process
 *   sys_getpid()        — return the calling process's PID
 *   sys_vfork(frame)    — create child process (parent blocked)
 *   sys_waitpid(pid,st) — wait for child to exit, reap zombie
 *   sys_execve(path,argv) — replace process image with new ELF binary
 */

#include "syscall.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../exec/exec.h"
#include "../fd/fd.h"
#include "../mm/page.h"
#include "../errno.h"
#include <string.h>

/* Wait status encoding (POSIX-compatible) */
#define W_EXITCODE(ret) (((ret) & 0xff) << 8)

/* waitpid options */
#define WNOHANG  1

/* ── sys_exit ───────────────────────────────────────────────────────────────── */

/*
 * Terminate the calling process:
 *   1. Store exit status for waitpid()
 *   2. Close all file descriptors
 *   3. Free user pages (if owned — not shared via vfork)
 *   4. Unblock vfork parent if applicable
 *   5. Wake parent (if blocked in waitpid)
 *   6. Mark ZOMBIE and yield
 *
 * Note: sys_exit runs inside SVC_Handler (Handler mode).  sched_yield()
 * only pends PendSV, which tail-chains after SVC returns.  There is no
 * need for an infinite loop — the ZOMBIE process will never be scheduled
 * again because sched_next() only picks PROC_RUNNABLE processes.
 */
long sys_exit(long status)
{
    current->exit_status = (int)status;

    /* Close all open fds */
    fd_close_all(current);

    /* Free user pages only if we own them (vfork_parent == NULL means
     * either this isn't a vfork child, or execve already replaced them) */
    if (!current->vfork_parent) {
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (current->user_pages[i]) {
                page_free(current->user_pages[i]);
                current->user_pages[i] = NULL;
            }
        }
        /* Free mmap regions */
        for (int i = 0; i < MMAP_REGIONS_MAX; i++) {
            if (current->mmap_regions[i].addr) {
                uint32_t base = (uint32_t)(uintptr_t)current->mmap_regions[i].addr;
                for (uint32_t j = 0; j < current->mmap_regions[i].pages; j++)
                    page_free((void *)(uintptr_t)(base + j * PAGE_SIZE));
                current->mmap_regions[i].addr  = NULL;
                current->mmap_regions[i].pages = 0;
            }
        }
    }

    /* Unblock vfork parent if we are a vfork child */
    if (current->vfork_parent) {
        current->vfork_parent->state = PROC_RUNNABLE;
        current->vfork_parent = NULL;
    }

    /* Wake parent if it is blocked (e.g. in waitpid).
     * After execve, vfork_parent is NULL so the vfork unblock above
     * won't fire — we need this separate wake-up for waitpid. */
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].pid == current->ppid &&
            proc_table[i].state == PROC_BLOCKED) {
            proc_table[i].state = PROC_RUNNABLE;
            break;
        }
    }

    current->state = PROC_ZOMBIE;
    sched_yield();
    return 0;  /* never reached — PendSV switches away after SVC returns */
}

/* ── sys_getpid ─────────────────────────────────────────────────────────────── */

long sys_getpid(void)
{
    return (long)current->pid;
}

/* ── sys_vfork ──────────────────────────────────────────────────────────────── */

/*
 * Create a child process.  The parent is blocked until the child calls
 * execve() or _exit().
 *
 * The child gets its own stack page with a copy of the parent's HW exception
 * frame (r0=0 for child return value).  The child shares the parent's
 * user_pages (GOT/data).
 *
 * frame: pointer to the parent's stacked exception frame on PSP
 *        [r0, r1, r2, r3, r12, lr, pc, xpsr]
 */
long sys_vfork(uint32_t *frame)
{
    /* 1. Allocate child PCB */
    pcb_t *child = proc_alloc();
    if (!child)
        return -(long)ENOMEM;

    /* 2. Allocate stack page for child */
    void *stack = page_alloc();
    if (!stack) {
        proc_free(child);
        return -(long)ENOMEM;
    }
    child->stack_page = stack;

    /* 3. Share parent's user_pages with child */
    for (int i = 0; i < USER_PAGES_MAX; i++)
        child->user_pages[i] = current->user_pages[i];

    /* 4. Build child's stack: copy the parent's entire stack page.
     *
     *    The compiler saves local variables (including r9/GOT base) to
     *    the stack.  After vfork returns, the child reads these stack-saved
     *    values.  If the child has a fresh stack, those reads return garbage.
     *    Copying the parent's stack gives the child a valid snapshot.
     *
     *    Then build the PendSV context (SW + HW frames) at the same offset
     *    on the child's page as the parent's PSP frame.
     */
    memcpy(stack, current->stack_page, PAGE_SIZE);

    /* Calculate child's frame position at the same offset as parent's */
    uintptr_t frame_off = (uintptr_t)frame - (uintptr_t)current->stack_page;
    uint32_t *child_frame = (uint32_t *)((uint8_t *)stack + frame_off);

    /* Set child's r0 = 0 (child sees vfork return 0) */
    child_frame[0] = 0;

    /* Build SW callee-saved frame below the HW frame.
     * r9 = GOT base so PIC addressing works after PendSV restore. */
    uint32_t *sw = child_frame - 8;
    memset(sw, 0, 8 * sizeof(uint32_t));
    sw[5] = current->got_base;   /* r9 = GOT SRAM address for PIC */

    child->sp = (uint32_t)(uintptr_t)sw;
    child->ticks_remaining = PROC_DEFAULT_TICKS;

    /* 5. Set up identity and relationships */
    child->ppid = current->pid;
    child->vfork_parent = current;
    child->got_base = current->got_base;
    memcpy(child->cwd, current->cwd, sizeof(child->cwd));
    memcpy(child->comm, current->comm, sizeof(child->comm));

    /* 6. Inherit file descriptors from parent */
    fd_inherit(child, current);

    /* 7. Set parent's return value (child PID) in stacked r0 */
    frame[0] = (uint32_t)child->pid;

    /* 8. Block parent, make child runnable */
    current->state = PROC_BLOCKED;
    child->state = PROC_RUNNABLE;

    /* 9. Yield — PendSV will switch to child after SVC returns.
     * Note: sched_yield() only pends PendSV (can't preempt SVC), so the
     * code below still executes.  This is harmless — the parent is BLOCKED,
     * so PendSV tail-chains and switches it out after our SVC return. */
    sched_yield();

    return (long)child->pid;
}

/* ── sys_waitpid ────────────────────────────────────────────────────────────── */

/*
 * Wait for a child process to exit.
 *   pid > 0:   wait for specific child
 *   pid == -1: wait for any child
 *   options & WNOHANG: return 0 immediately if no child has exited
 *
 * Returns child PID on success, -ECHILD if no children, 0 if WNOHANG.
 *
 * Blocking: since this runs inside SVC_Handler (Handler mode), we cannot
 * loop and retry — sched_yield() only pends PendSV, which cannot preempt
 * SVC.  Instead, we mark PROC_BLOCKED, set svc_restart = 1, and return.
 * SVC_Handler restores frame[0] and adjusts PC-2 so the syscall re-executes
 * when the process is rescheduled.  PendSV tail-chains after SVC returns.
 */
long sys_waitpid(long pid, long status_ptr, long options)
{
    pcb_t *zombie = NULL;
    int has_child = 0;

    /* Scan for matching zombie or living child */
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE)
            continue;
        if (p->ppid != current->pid)
            continue;
        if (pid > 0 && p->pid != (pid_t)pid)
            continue;

        has_child = 1;

        if (p->state == PROC_ZOMBIE) {
            zombie = p;
            break;
        }
    }

    if (zombie) {
        /* Reap the zombie */
        pid_t cpid = zombie->pid;

        if (status_ptr) {
            int *sp = (int *)(uintptr_t)status_ptr;
            *sp = W_EXITCODE(zombie->exit_status);
        }

        /* Free zombie's stack page */
        if (zombie->stack_page) {
            page_free(zombie->stack_page);
            zombie->stack_page = NULL;
        }

        proc_free(zombie);
        return (long)cpid;
    }

    if (!has_child)
        return -(long)ECHILD;

    if (options & WNOHANG)
        return 0;

    /* Block and arrange for syscall restart.
     * sys_exit will wake us by setting PROC_RUNNABLE.
     * SVC_Handler will restore frame[0] and PC-2 so the SVC re-executes. */
    current->state = PROC_BLOCKED;
    svc_restart = 1;
    sched_yield();
    return 0;  /* value ignored — SVC_Handler restores original frame[0] */
}

/* ── sys_execve ─────────────────────────────────────────────────────────────── */

/*
 * Replace the current process image with a new ELF binary.
 * Called from user space (typically after vfork).
 *
 * On success: never returns — the new program starts executing.
 * On failure: returns negative errno.
 */
long sys_execve(const char *path, const char *const *argv)
{
    /* Save old pages to free after successful load */
    void *old_stack = current->stack_page;
    void *old_user[USER_PAGES_MAX];
    int owns_pages = (current->vfork_parent == NULL);
    for (int i = 0; i < USER_PAGES_MAX; i++)
        old_user[i] = current->user_pages[i];

    /* Clear pages so do_execve allocates fresh ones */
    current->stack_page = NULL;
    for (int i = 0; i < USER_PAGES_MAX; i++)
        current->user_pages[i] = NULL;

    /* Close old fds — do_execve will set up new ones */
    fd_close_all(current);

    /* Load the new binary.  argv points into the old stack/data pages
     * which are still valid (detached from current but not yet freed). */
    int err = do_execve(current, path, argv);
    if (err < 0) {
        /* Restore old pages on failure */
        current->stack_page = old_stack;
        for (int i = 0; i < USER_PAGES_MAX; i++)
            current->user_pages[i] = old_user[i];
        fd_stdio_init(current);
        return (long)err;
    }

    /* Free old stack page */
    if (old_stack)
        page_free(old_stack);

    /* Free old user pages only if we owned them */
    if (owns_pages) {
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (old_user[i])
                page_free(old_user[i]);
        }
    }

    /* Unblock vfork parent — we have our own pages now */
    if (current->vfork_parent) {
        current->vfork_parent->state = PROC_RUNNABLE;
        current->vfork_parent = NULL;
    }

    /* Signal SVC_Handler to do a PendSV-like full context restore from
     * current->sp before exception return.  This ensures r4-r11 (including
     * r9/GOT base) are correctly loaded from the new process's SW frame.
     *
     * We cannot set r9 via inline asm here because the C compiler's
     * function epilogue (callee-saved register restores) would undo it. */
    exec_pending = 1;

    return 0;
}

/* ── sys_set_tid_address ───────────────────────────────────────────────────── */

long sys_set_tid_address(void *tidptr)
{
    current->clear_child_tid = (int *)tidptr;
    return (long)current->pid;
}

/* ── sys_uname ──────────────────────────────────────────────────────────────── */

/*
 * struct utsname layout (65 bytes per field × 6 fields = 390 bytes).
 * Matches Linux/musl: each field is char[65].
 */
#define UTS_LEN 65

long sys_uname(void *buf)
{
    if (!buf)
        return -(long)EINVAL;

    char *p = (char *)buf;
    __builtin_memset(p, 0, UTS_LEN * 6);

    /* sysname */
    const char *s = "PicoPiAndPortable";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* nodename */
    s = "ppap";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* release */
    s = "0.6.0";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* version */
    s = "#1 PPAP";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* machine */
    s = "armv6m";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    /* p += UTS_LEN; — domainname follows but we leave it zeroed */

    return 0;
}

/* ── sys_setpgid ────────────────────────────────────────────────────────────── */

long sys_setpgid(long pid, long pgid)
{
    pcb_t *target;

    if (pid == 0)
        target = current;
    else {
        target = NULL;
        for (uint32_t i = 0; i < PROC_MAX; i++) {
            if (proc_table[i].state != PROC_FREE &&
                proc_table[i].pid == (pid_t)pid) {
                target = &proc_table[i];
                break;
            }
        }
        if (!target)
            return -(long)ESRCH;
    }

    target->pgid = (pgid == 0) ? target->pid : (pid_t)pgid;
    return 0;
}

/* ── sys_setsid ─────────────────────────────────────────────────────────────── */

long sys_setsid(void)
{
    current->sid  = current->pid;
    current->pgid = current->pid;
    return (long)current->pid;
}

/* ── sys_wait4 ──────────────────────────────────────────────────────────────── */

long sys_wait4(long pid, long status_ptr, long options, void *rusage)
{
    (void)rusage;   /* rusage not supported */
    return sys_waitpid(pid, status_ptr, options);
}
