/*
 * signal.c — Signal infrastructure for PPAP
 *
 *   sys_kill(pid, sig)            — send signal to process
 *   sys_sigaction(sig, hdl, old)  — install/query signal handler
 *   sys_sigreturn()               — restore context after signal handler
 *   signal_check()                — called from SVC_Handler on return to user
 *   sigreturn_trampoline          — handler returns here (kernel .text)
 *
 * Signal delivery model:
 *   signal_check() is called from SVC_Handler after syscall_dispatch()
 *   when the process is RUNNABLE.  It delivers one pending signal per
 *   syscall return:
 *     - SIG_IGN: clear pending bit, done
 *     - SIG_DFL: SIGCHLD ignored, all others terminate via sys_exit(128+sig)
 *     - User handler: push a new HW exception frame below PSP (the original
 *       frame becomes "sigframe"), set PSP to new frame.  On exception return
 *       the CPU pops the new frame and runs the handler.  The handler returns
 *       via bx lr to sigreturn_trampoline, which does SVC SYS_SIGRETURN.
 *       sys_sigreturn advances PSP by 32 so the CPU pops the sigframe
 *       (original context) on exception return.
 */

#include "signal.h"
#include "../proc/proc.h"
#include "../proc/sched.h"
#include "../syscall/syscall.h"
#include "../errno.h"
#include <stddef.h>

/* ── sigreturn_trampoline ─────────────────────────────────────────────────── */
/*
 * Placed in kernel .text (flash XIP).  User-mode code can execute flash.
 * When the signal handler does bx lr, it lands here.
 * 4 bytes: movs r7, #119 (SYS_SIGRETURN) + svc 0.
 */
__attribute__((naked, used, section(".text.sigreturn_trampoline")))
void sigreturn_trampoline(void)
{
    __asm volatile(
        "movs r7, #119\n"    /* SYS_SIGRETURN */
        "svc  0\n"
        "b    .\n"           /* should never reach */
    );
}

/* ── signal_setup_frame ───────────────────────────────────────────────────── */
/*
 * Build a signal delivery frame on the user stack.
 *
 * Before:
 *   [PSP + 0..28]   original HW exception frame
 *   [PSP + 32]      original user SP
 *
 * After:
 *   [PSP - 32 + 0..28]  new HW frame (r0=sig, pc=handler, lr=trampoline)
 *   [PSP + 0..28]       original HW frame = sigframe (untouched)
 *   [PSP + 32]          original user SP
 *
 * PSP is set to PSP - 32.  On exception return the CPU pops the new frame
 * and runs the handler.  The handler's SP = PSP - 32 + 32 = old PSP,
 * where the sigframe sits.
 */
static void signal_setup_frame(int sig, sighandler_t handler)
{
    uint32_t psp;
    __asm volatile("mrs %0, psp" : "=r"(psp));

    /* New HW frame goes 32 bytes below current PSP */
    uint32_t new_psp = psp - 32;
    uint32_t *frame = (uint32_t *)new_psp;

    frame[0] = (uint32_t)sig;                                 /* r0 = signal number  */
    frame[1] = 0;                                              /* r1                  */
    frame[2] = 0;                                              /* r2                  */
    frame[3] = 0;                                              /* r3                  */
    frame[4] = 0;                                              /* r12                 */
    frame[5] = (uint32_t)(uintptr_t)sigreturn_trampoline;     /* lr (Thumb bit set)  */
    frame[6] = (uint32_t)(uintptr_t)handler & ~1u;            /* pc (bit0 clear)     */
    frame[7] = 0x01000000u;                                    /* xpsr (Thumb bit)    */

    __asm volatile("msr psp, %0" :: "r"(new_psp));
}

/* ── signal_check ─────────────────────────────────────────────────────────── */

void signal_check(void)
{
    uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
    if (!deliverable)
        return;

    int sig = __builtin_ctz(deliverable);  /* lowest pending signal */
    current->sig_pending &= ~(1u << sig);

    sighandler_t handler = current->sig_handlers[sig];

    if (handler == SIG_IGN)
        return;

    if (handler == SIG_DFL) {
        /* Default action: SIGCHLD is ignored, all others terminate */
        if (sig == SIGCHLD)
            return;
        sys_exit(128 + sig);
        return;
    }

    /* User handler — set up trampoline frame on user stack */
    signal_setup_frame(sig, handler);
}

/* ── sys_kill ─────────────────────────────────────────────────────────────── */

long sys_kill(long pid, long sig)
{
    if (sig < 0 || sig >= NSIG)
        return -(long)EINVAL;

    /* Find target process by PID */
    pcb_t *target = NULL;
    for (uint32_t i = 0; i < PROC_MAX; i++) {
        if (proc_table[i].state != PROC_FREE &&
            proc_table[i].pid == (pid_t)pid) {
            target = &proc_table[i];
            break;
        }
    }

    if (!target)
        return -(long)ESRCH;

    /* Signal 0 is a validity check — don't deliver */
    if (sig == 0)
        return 0;

    /* Set pending bit */
    target->sig_pending |= (1u << sig);

    /* Wake the target if it is blocked/sleeping */
    if (target->state == PROC_BLOCKED || target->state == PROC_SLEEPING)
        target->state = PROC_RUNNABLE;

    return 0;
}

/* ── sys_sigaction ────────────────────────────────────────────────────────── */

long sys_sigaction(long sig, long handler, long old_ptr)
{
    if (sig < 1 || sig >= NSIG)
        return -(long)EINVAL;

    /* SIGKILL and SIGSTOP cannot be caught or ignored */
    if (sig == SIGKILL || sig == SIGSTOP)
        return -(long)EINVAL;

    /* Return old handler if requested */
    if (old_ptr) {
        sighandler_t *out = (sighandler_t *)(uintptr_t)old_ptr;
        *out = current->sig_handlers[sig];
    }

    /* Install new handler */
    current->sig_handlers[sig] = (sighandler_t)(uintptr_t)handler;

    return 0;
}

/* ── sys_sigreturn ────────────────────────────────────────────────────────── */
/*
 * Restore context after a signal handler returns via sigreturn_trampoline.
 *
 * At this point PSP points to the trampoline's HW exception frame.
 * The sigframe (original context) is at PSP + 32.  Advancing PSP by 32
 * makes the CPU pop the sigframe on exception return, restoring the
 * original user context (r0-r3, r12, lr, pc, xpsr).
 */
long sys_sigreturn(void)
{
    uint32_t psp;
    __asm volatile("mrs %0, psp" : "=r"(psp));
    psp += 32;
    __asm volatile("msr psp, %0" :: "r"(psp));
    return 0;  /* value ignored — sigframe[0] has original r0 */
}
