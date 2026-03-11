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
#include "../signal/signal.h"
#include "../klog.h"
#include "../ecpu/ecpu_z80.h"
#include "../ecpu/ecpu_m68k.h"
#include "../subsys/ppap_m68k_bridge.h"
#include "../../target/target.h"
#include "common/ptrace.h"
#include "common/wait.h"
#include <string.h>

/* Wait status encoding (POSIX-compatible) */
#define W_EXITCODE(ret) (((ret) & 0xff) << 8)
#define W_STOPCODE(sig) ((((sig) & 0xff) << 8) | 0x7f)

#define TRACE_PHASE_ENTER  0
#define TRACE_PHASE_EXIT   1
#define TRACE_MODE_MASK \
    (PPAP_TRACE_MODE_PPAP_SYSCALL | PPAP_TRACE_MODE_SUBSYS_CALL)

static pcb_t *trace_find_tracee(pid_t tracer_pid, long pid)
{
    if (pid <= 0)
        return NULL;
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE || p->pid != (pid_t)pid)
            continue;
        if (p->tracer_pid != tracer_pid)
            continue;
        return p;
    }
    return NULL;
}

static void trace_wake_tracer(const pcb_t *tracee)
{
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE || p->pid != tracee->tracer_pid)
            continue;
        if (p->state == PROC_BLOCKED)
            p->state = PROC_RUNNABLE;
        break;
    }
}

static void trace_fill_event(uint32_t event, uint32_t abi, uint32_t nr,
                             uint32_t a0, uint32_t a1, uint32_t a2,
                             uint32_t a3, uint32_t a4, uint32_t a5,
                             int32_t ret, uint32_t flags)
{
    current->trace_event.event = event;
    current->trace_event.flags = flags;
    current->trace_event.abi = abi;
    current->trace_event.nr = nr;
    current->trace_event.args[0] = a0;
    current->trace_event.args[1] = a1;
    current->trace_event.args[2] = a2;
    current->trace_event.args[3] = a3;
    current->trace_event.args[4] = a4;
    current->trace_event.args[5] = a5;
    current->trace_event.ret = ret;
}

static void trace_stop_current(int restart)
{
    current->trace_wait_pending = 1;
    current->state = PROC_TRACED_STOP;
    trace_wake_tracer(current);
    if (restart)
        set_svc_restart();
    sched_yield();
}

static void trace_reset_mode_state(pcb_t *p, uint8_t mode)
{
    p->trace_mode = mode;
    if (!(mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        p->trace_syscall_phase = TRACE_PHASE_ENTER;
    if (!(mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        p->trace_subsys_phase = TRACE_PHASE_ENTER;
}

static void trace_resume_target(pcb_t *target)
{
    target->trace_wait_pending = 0;
    if (target->state == PROC_TRACED_STOP)
        target->state = PROC_RUNNABLE;
    sched_yield();
}

static void trace_regs_init(struct ppap_ptrace_regs *regs,
                            uint32_t regset, uint32_t abi, uint32_t words)
{
    __builtin_memset(regs, 0, sizeof(*regs));
    regs->regset = regset;
    regs->abi = abi;
    regs->words = words;
}

static int trace_fill_arm_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    const uint32_t *sp = (const uint32_t *)(uintptr_t)target->sp;

    trace_regs_init(regs, PPAP_TRACE_REGSET_ARM, target->trace_event.abi, 17);
    regs->regs[0] = sp[8];
    regs->regs[1] = sp[9];
    regs->regs[2] = sp[10];
    regs->regs[3] = sp[11];
    regs->regs[4] = sp[0];
    regs->regs[5] = sp[1];
    regs->regs[6] = sp[2];
    regs->regs[7] = sp[3];
    regs->regs[8] = sp[4];
    regs->regs[9] = sp[5];
    regs->regs[10] = sp[6];
    regs->regs[11] = sp[7];
    regs->regs[12] = sp[12];
    regs->regs[13] = (uint32_t)(uintptr_t)(sp + 16);
    regs->regs[14] = sp[13];
    regs->regs[15] = sp[14];
    regs->regs[16] = sp[15];
    return 0;
}

#if defined(__m68k__)
static int trace_fill_m68k_frame_regs(const pcb_t *target,
                                      struct ppap_ptrace_regs *regs)
{
    const uint32_t *frame = (const uint32_t *)(uintptr_t)target->sp;
    const uint8_t *exc = (const uint8_t *)(uintptr_t)target->sp + 60;

    trace_regs_init(regs, PPAP_TRACE_REGSET_M68K, target->trace_event.abi, 20);
    for (uint32_t i = 0; i < 15; i++)
        regs->regs[i] = frame[i];
    regs->regs[15] = target->usp;
    regs->regs[16] = ((uint32_t)*(const uint16_t *)(const void *)(exc + 2) << 16)
                   | *(const uint16_t *)(const void *)(exc + 4);
    regs->regs[17] = *(const uint16_t *)(const void *)exc;
    regs->regs[18] = target->usp;
    regs->regs[19] = target->sp;
    return 0;
}
#endif

static int trace_fill_m68k_emu_regs(const pcb_t *target,
                                    struct ppap_ptrace_regs *regs)
{
    const ppap_m68k_exec_state_t *state =
        (const ppap_m68k_exec_state_t *)target->subsys_data;
    const m68k_state_t *cpu;

    if (!state)
        return -EINVAL;
    cpu = &state->m68k;

    trace_regs_init(regs, PPAP_TRACE_REGSET_M68K, target->trace_event.abi, 20);
    for (uint32_t i = 0; i < 8; i++) {
        regs->regs[i] = cpu->d[i];
        regs->regs[8 + i] = cpu->a[i];
    }
    regs->regs[16] = cpu->pc;
    regs->regs[17] = cpu->sr;
    regs->regs[18] = cpu->usp;
    regs->regs[19] = cpu->ssp;
    return 0;
}

static int trace_fill_z80_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    const z80_state_t *cpu = (const z80_state_t *)target->subsys_data;

    if (!cpu)
        return -EINVAL;

    trace_regs_init(regs, PPAP_TRACE_REGSET_Z80, target->trace_event.abi, 18);
    regs->regs[0] = z80_af(cpu);
    regs->regs[1] = z80_bc(cpu);
    regs->regs[2] = z80_de(cpu);
    regs->regs[3] = z80_hl(cpu);
    regs->regs[4] = cpu->ix;
    regs->regs[5] = cpu->iy;
    regs->regs[6] = cpu->sp;
    regs->regs[7] = cpu->pc;
    regs->regs[8] = ((uint16_t)cpu->a2 << 8) | cpu->f2;
    regs->regs[9] = ((uint16_t)cpu->b2 << 8) | cpu->c2;
    regs->regs[10] = ((uint16_t)cpu->d2 << 8) | cpu->e2;
    regs->regs[11] = ((uint16_t)cpu->h2 << 8) | cpu->l2;
    regs->regs[12] = cpu->iff1;
    regs->regs[13] = cpu->iff2;
    regs->regs[14] = cpu->im;
    regs->regs[15] = cpu->wz;
    regs->regs[16] = cpu->i;
    regs->regs[17] = cpu->r;
    return 0;
}

static int trace_fill_regs(const pcb_t *target, struct ppap_ptrace_regs *regs)
{
    if (target->state != PROC_TRACED_STOP)
        return -EBUSY;

    if (target->subsys == SUBSYS_CPM)
        return trace_fill_z80_regs(target, regs);

    if (target->subsys == SUBSYS_PPAP && target->subsys_data)
        return trace_fill_m68k_emu_regs(target, regs);

#if defined(__m68k__)
    return trace_fill_m68k_frame_regs(target, regs);
#else
    return trace_fill_arm_regs(target, regs);
#endif
}

int trace_before_syscall(uint32_t *frame, uint32_t nr, uint32_t a4, uint32_t a5)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        return 0;
    if (current->trace_syscall_phase != TRACE_PHASE_ENTER)
        return 0;
    if (nr == SYS_PTRACE)
        return 0;

    current->trace_syscall_phase = TRACE_PHASE_EXIT;
    trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_ENTER, PPAP_TRACE_ABI_PPAP, nr,
                     frame[0], frame[1], frame[2], frame[3], a4, a5, 0, 0);
    trace_stop_current(1);
    return 1;
}

void trace_after_syscall(uint32_t *frame, uint32_t nr,
                         uint32_t a4, uint32_t a5, long ret)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
        return;
    if (current->trace_syscall_phase != TRACE_PHASE_EXIT)
        return;
    if (current->state != PROC_RUNNABLE)
        return;
    if (nr == SYS_PTRACE)
        return;

    current->trace_syscall_phase = TRACE_PHASE_ENTER;
    trace_fill_event(PPAP_TRACE_EVENT_SYSCALL_EXIT, PPAP_TRACE_ABI_PPAP, nr,
                     frame[0], frame[1], frame[2], frame[3], a4, a5,
                     (int32_t)ret, 0);
    trace_stop_current(0);
}

int trace_before_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        return 0;
    if (current->trace_subsys_phase != TRACE_PHASE_ENTER)
        return 0;

    current->trace_subsys_phase = TRACE_PHASE_EXIT;
    trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_ENTER, abi, nr,
                     a0, a1, a2, a3, a4, a5, 0, 0);
    trace_stop_current(0);
    return 1;
}

void trace_after_subsys(uint32_t abi, uint32_t nr,
                        uint32_t a0, uint32_t a1, uint32_t a2,
                        uint32_t a3, uint32_t a4, uint32_t a5,
                        int32_t ret)
{
    if (!(current->trace_mode & PPAP_TRACE_MODE_SUBSYS_CALL))
        return;
    if (current->trace_subsys_phase != TRACE_PHASE_EXIT)
        return;
    if (current->state != PROC_RUNNABLE)
        return;

    current->trace_subsys_phase = TRACE_PHASE_ENTER;
    trace_fill_event(PPAP_TRACE_EVENT_SUBSYS_EXIT, abi, nr,
                     a0, a1, a2, a3, a4, a5, ret, 0);
    trace_stop_current(0);
}

void trace_exec_stop(void)
{
    if (!current->trace_requested)
        return;

    current->trace_requested = 0;
    current->trace_syscall_phase = TRACE_PHASE_ENTER;
    current->trace_subsys_phase = TRACE_PHASE_ENTER;
    trace_fill_event(PPAP_TRACE_EVENT_EXEC, PPAP_TRACE_ABI_PPAP, SYS_EXECVE,
                     0, 0, 0, 0, 0, 0, 0, 0);
    trace_stop_current(0);
}

long sys_ptrace(long req, long pid, void *addr, void *data)
{
    if (req == PTRACE_TRACEME) {
        if (current->tracer_pid != 0 || current->trace_requested)
            return -(long)EPERM;
        current->tracer_pid = current->ppid;
        current->trace_requested = 1;
        current->trace_mode = 0;
        current->trace_wait_pending = 0;
        current->trace_syscall_phase = TRACE_PHASE_ENTER;
        current->trace_subsys_phase = TRACE_PHASE_ENTER;
        __builtin_memset(&current->trace_event, 0, sizeof(current->trace_event));
        return 0;
    }

    pcb_t *target = trace_find_tracee(current->pid, pid);
    if (!target)
        return -(long)ESRCH;

    switch (req) {
    case PTRACE_GETEVENT:
        if (!data)
            return -(long)EINVAL;
        *(struct ppap_ptrace_event *)data = target->trace_event;
        return 0;
    case PTRACE_GETREGS:
        if (!data)
            return -(long)EINVAL;
        return trace_fill_regs(target, (struct ppap_ptrace_regs *)data);
    case PTRACE_SETMODE: {
        uint8_t mode = (uint8_t)(uintptr_t)addr;
        if (mode & (uint8_t)~TRACE_MODE_MASK)
            return -(long)EINVAL;
        trace_reset_mode_state(target, mode);
        return 0;
    }
    case PTRACE_CONT:
        trace_resume_target(target);
        return 0;
    case PTRACE_SYSCALL:
        if (!(target->trace_mode & PPAP_TRACE_MODE_PPAP_SYSCALL))
            target->trace_syscall_phase = TRACE_PHASE_ENTER;
        target->trace_mode |= PPAP_TRACE_MODE_PPAP_SYSCALL;
        trace_resume_target(target);
        return 0;
    case PTRACE_DETACH:
        target->trace_requested = 0;
        target->tracer_pid = 0;
        trace_reset_mode_state(target, 0);
        target->trace_wait_pending = 0;
        __builtin_memset(&target->trace_event, 0, sizeof(target->trace_event));
        trace_resume_target(target);
        return 0;
    default:
        return -(long)EINVAL;
    }
}

/* ── sys_exit ───────────────────────────────────────────────────────────────── */

/*
 * Terminate the calling process:
 *   1. Store exit status for waitpid()
 *   2. Close all file descriptors
 *   3. Free user pages (if owned — not shared via vfork)
 *   4. Unblock vfork parent if applicable
 *   5. Wake parent (if blocked in waitpid)
 *   6. Reparent children to init (PID 1)
 *   7. Mark ZOMBIE and yield
 *
 * Page lifecycle:
 *   - user_pages[] are freed here in sys_exit() (step 3).
 *   - stack_page is freed later in sys_waitpid() when the parent reaps
 *     the zombie.  Orphans are reparented to init (step 6), ensuring
 *     init can always reap them.
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
#if defined(__m68k__)
        if (current->user_stack_page) {
            page_free(current->user_stack_page);
            current->user_stack_page = NULL;
        }
#endif
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
    } else {
        /* vfork child exiting without exec: free child-owned pages only
         * (e.g. the user stack copy allocated by sys_vfork). */
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (current->user_pages[i] &&
                current->user_pages[i] !=
                    current->vfork_parent->user_pages[i]) {
                page_free(current->user_pages[i]);
                current->user_pages[i] = NULL;
            }
        }
#if defined(__m68k__)
        /* Free child's user stack copy if different from parent's */
        if (current->user_stack_page &&
            current->user_stack_page != current->vfork_parent->user_stack_page) {
            page_free(current->user_stack_page);
            current->user_stack_page = NULL;
        }
#endif
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

    /* Reparent children to init (PID 1) so they can be reaped.
     * If a reparented child is already zombie, wake init. */
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *child = &proc_table[i];
        if (child->state == PROC_FREE || child->ppid != current->pid)
            continue;
        child->ppid = 1;
        if (child->state == PROC_ZOMBIE) {
            for (uint32_t j = 0; j < PROC_MAX; j++) {
                if (proc_table[j].pid == 1 &&
                    proc_table[j].state == PROC_BLOCKED) {
                    proc_table[j].state = PROC_RUNNABLE;
                    break;
                }
            }
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
 * The child gets its own stack page with a copy of the parent's exception
 * frame (return register = 0 for child).  The child shares the parent's
 * user_pages (GOT/data).
 *
 * frame: pointer to the parent's stacked exception frame
 *   ARM:  [r0, r1, r2, r3, r12, lr, pc, xpsr] on PSP
 *   m68k: &regs[1] (d1 slot) in the TRAP #0 saved register frame
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

#if defined(__m68k__)
    /* m68k: The TRAP #0 frame (15 regs + SR + PC) has the same layout as
     * the switch.S context frame.  The child returns directly to user code
     * via switch.S restore (movem.l + rte), bypassing TRAP #0 cleanup.
     *
     * frame = &regs[1] (d1 slot).  child_frame - 1 = d0 slot. */
    uint32_t *child_regs = child_frame - 1;   /* d0 slot */
    child_regs[0] = 0;                        /* d0 = 0 (child return) */
    child_regs[13] = current->got_base;       /* a5 = GOT base for PIC */

    child->sp = (uint32_t)(uintptr_t)child_regs;

    /* m68k user mode: USP points to a user_stack_page (separate from
     * stack_page / SSP).  The child must have its own user stack copy;
     * otherwise the child's post-vfork code (pushing execve arguments)
     * overwrites the parent's return addresses on the shared stack. */
    {
        void *parent_ustack = current->user_stack_page;

        if (parent_ustack) {
            void *child_ustack = page_alloc();
            if (!child_ustack) {
                page_free(stack);
                proc_free(child);
                return -(long)ENOMEM;
            }
            memcpy(child_ustack, parent_ustack, PAGE_SIZE);
            child->user_stack_page = child_ustack;

            /* Adjust child USP to same offset within the new page */
            uint32_t parent_usp = current->usp;
            uint32_t usp_off = parent_usp -
                               (uint32_t)(uintptr_t)parent_ustack;
            child->usp = (uint32_t)(uintptr_t)child_ustack + usp_off;

            /* Patch a6 (frame pointer) if it points into the user stack */
            uint32_t parent_base = (uint32_t)(uintptr_t)parent_ustack;
            uint32_t child_base = (uint32_t)(uintptr_t)child_ustack;
            if (child_regs[14] >= parent_base &&
                child_regs[14] < parent_base + PAGE_SIZE) {
                child_regs[14] += child_base - parent_base;
            }
        } else {
            child->usp = current->usp;
        }
    }
#else
    /* ARM: Set child's r0 = 0 (child sees vfork return 0) */
    child_frame[0] = 0;

    /* Build SW callee-saved frame below the HW frame.
     * r9 = GOT base so PIC addressing works after PendSV restore. */
    uint32_t *sw = child_frame - 8;
    memset(sw, 0, 8 * sizeof(uint32_t));
    sw[5] = current->got_base;   /* r9 = GOT SRAM address for PIC */

    child->sp = (uint32_t)(uintptr_t)sw;
#endif
    child->ticks_remaining = PROC_DEFAULT_TICKS;

    /* 5. Set up identity and relationships */
    child->ppid = current->pid;
    child->pgid = current->pgid;   /* inherit process group (like real fork) */
    child->sid  = current->sid;    /* inherit session ID */
    child->vfork_parent = current;
    child->got_base = current->got_base;
    child->tp_value = current->tp_value;
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

    /* Clear stale flags left by the child's syscalls while we were blocked.
     * exec_pending and svc_restart are global (not per-process), so a child's
     * execve or blocking read can leave flags that would corrupt our return. */
    exec_pending[core_id()] = 0;
    svc_restart[core_id()]  = 0;

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
    pcb_t *stopped = NULL;
    int has_child = 0;

    /* Scan for matching stopped child, zombie, or living child */
    for (uint32_t i = 1; i < PROC_MAX; i++) {
        pcb_t *p = &proc_table[i];
        if (p->state == PROC_FREE)
            continue;
        if (p->ppid != current->pid)
            continue;
        if (pid > 0 && p->pid != (pid_t)pid)
            continue;

        has_child = 1;

        if ((options & WSTOPPED) &&
            p->state == PROC_TRACED_STOP &&
            p->trace_wait_pending) {
            stopped = p;
            break;
        }

        if (p->state == PROC_ZOMBIE) {
            zombie = p;
            break;
        }
    }

    if (stopped) {
        if (status_ptr) {
            int *sp = (int *)(uintptr_t)status_ptr;
            *sp = W_STOPCODE(SIGTRAP);
        }
        stopped->trace_wait_pending = 0;
        return (long)stopped->pid;
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
    set_svc_restart();
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
#if defined(__m68k__)
    void *old_user_stack = current->user_stack_page;
#endif

    /* Clear pages so do_execve allocates fresh ones */
    current->stack_page = NULL;
    for (int i = 0; i < USER_PAGES_MAX; i++)
        current->user_pages[i] = NULL;
#if defined(__m68k__)
    current->user_stack_page = NULL;
#endif

    /* Load the new binary.  argv points into the old stack/data pages
     * which are still valid (detached from current but not yet freed). */
    int err = do_execve(current, path, argv);
    if (err < 0) {
        /* Restore old pages on failure — fds are untouched (POSIX) */
        current->stack_page = old_stack;
        for (int i = 0; i < USER_PAGES_MAX; i++)
            current->user_pages[i] = old_user[i];
#if defined(__m68k__)
        current->user_stack_page = old_user_stack;
#endif
        return (long)err;
    }

    /* POSIX: preserve open fds across execve (redirections, pipes).
     * Only install default tty stdio if fd 0/1/2 are not already open
     * (e.g. init's first exec before any shell sets up fds). */
    if (!current->fd_table[0] && !current->fd_table[1] &&
        !current->fd_table[2])
        fd_stdio_init(current);

    /* Free old stack page.
     * m68k has no MSP/PSP split — we're still executing on old_stack.
     * Defer the free until trap.S switches SP to the new stack. */
#if defined(__m68k__)
    extern volatile void *m68k_exec_old_stack;
    m68k_exec_old_stack = old_stack;
#else
    if (old_stack)
        page_free(old_stack);
#endif

    /* Free old user pages only if we owned them */
    if (owns_pages) {
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (old_user[i])
                page_free(old_user[i]);
        }
#if defined(__m68k__)
        if (old_user_stack)
            page_free(old_user_stack);
#endif
    } else if (current->vfork_parent) {
        /* vfork child: free pages that were allocated specifically for
         * the child (e.g. user stack copy), not the shared parent pages. */
        for (int i = 0; i < USER_PAGES_MAX; i++) {
            if (old_user[i] &&
                old_user[i] != current->vfork_parent->user_pages[i])
                page_free(old_user[i]);
        }
#if defined(__m68k__)
        if (old_user_stack &&
            old_user_stack != current->vfork_parent->user_stack_page)
            page_free(old_user_stack);
#endif
    }

    /* Unblock vfork parent — we have our own pages now */
    if (current->vfork_parent) {
        current->vfork_parent->state = PROC_RUNNABLE;
        current->vfork_parent = NULL;
    }

    trace_exec_stop();

    /* Signal SVC_Handler to do a PendSV-like full context restore from
     * current->sp before exception return.  This ensures r4-r11 (including
     * r9/GOT base) are correctly loaded from the new process's SW frame.
     *
     * We cannot set r9 via inline asm here because the C compiler's
     * function epilogue (callee-saved register restores) would undo it. */
    exec_pending[core_id()] = 1;

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
    const char *s = "PiPAPo";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* nodename — target-specific (e.g. "pico1", "pico1calc", "qemu_arm") */
    s = target_name();
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* release */
    s = "0.11.0";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* version */
    s = "#1 PPAP";
    for (int i = 0; s[i] && i < UTS_LEN - 1; i++) p[i] = s[i];
    p += UTS_LEN;

    /* machine */
#if defined(__m68k__)
    s = "m68k";
#else
    s = "armv6m";
#endif
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
