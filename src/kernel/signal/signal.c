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
#include "../subsys/subsys.h"
#include "../errno.h"
#include <stddef.h>

static int ctz32(uint32_t x)
{
    int n = 0;
    if (!(x & 0xFFFF)) { n += 16; x >>= 16; }
    if (!(x & 0xFF))   { n += 8;  x >>= 8;  }
    if (!(x & 0xF))    { n += 4;  x >>= 4;  }
    if (!(x & 0x3))    { n += 2;  x >>= 2;  }
    if (!(x & 0x1))    { n += 1; }
    return n;
}

static int signal_default_action(int sig, uint32_t *regs)
{
    if (sig == SIGCHLD)
        return 1;

    {
        const subsys_ops_t *ops = current->subsys < SUBSYS_MAX
                                  ? subsys_ops_table[current->subsys] : 0;
        if (ops && ops->on_signal && ops->on_signal(current, sig, regs))
            return 1;
    }

    sys_exit(128 + sig);
    return 1;
}

int signal_check_kernel(void)
{
    uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
    if (!deliverable)
        return 0;

    int sig = ctz32(deliverable);
    current->sig_pending &= ~(1u << sig);

    if (current->sig_handlers[sig] == SIG_IGN)
        return 1;

    return signal_default_action(sig, NULL);
}

/* ── Architecture-specific signal delivery ─────────────────────────────────
 *
 * ARM:  RTE-based — sigreturn_trampoline (naked ASM), signal_setup_frame
 *       (PSP manipulation), signal_check (delivers via HW exception frame).
 *
 * m68k: Synchronous call — signal_check calls the handler directly via
 *       m68k_call_signal_handler (assembly thunk that sets a5 = GOT base).
 *       No sigreturn needed; sys_sigreturn / sys_rt_sigreturn return -ENOSYS.
 * ────────────────────────────────────────────────────────────────────────── */

#if defined(__m68k__)

/*
 * m68k signal delivery — synchronous call model
 *
 * On PPAP m68k, processes run in user mode with USP/SSP separation.
 * The ARM-style RTE-based signal delivery (push frame onto user stack,
 * modify exception frame, RTE to handler) is not used because the
 * m68000 lacks the full frame format needed for safe nested exceptions.
 *
 * Instead, signal_check() calls the handler directly as a C function via
 * an assembly thunk that sets a5 = GOT base (required for PIC/-msep-data).
 * The SSP register frame is saved/restored around the handler call so the
 * original context is preserved.
 *
 * This is equivalent to the RTE model in terms of observable behavior:
 * the handler runs synchronously on syscall return, just like ARM.
 */

#define SIGFRAME_SIZE  66   /* d0-d7/a0-a6 (60) + SR (2) + PC (4) */

/*
 * Trampoline stub — not used on m68k (synchronous delivery), but kept
 * so the linker doesn't complain about the extern declaration in signal.h.
 */
__attribute__((naked, used, section(".text.sigreturn_trampoline")))
void sigreturn_trampoline(void)
{
    __asm volatile("rts\n");
}

/*
 * m68k_call_signal_handler — assembly thunk to call a PIC signal handler.
 *
 * Sets a5 = GOT base before calling the handler (required for -msep-data),
 * saves/restores the kernel's a5 around the call.
 *
 * Defined in signal_m68k.S.
 */
extern void m68k_call_signal_handler(sighandler_t handler, int sig,
                                     uint32_t got_base);

/* ── signal_check ─────────────────────────────────────────────────────────── */

void signal_check(uint32_t *regs)
{
    uint32_t deliverable = current->sig_pending & ~current->sig_blocked;
    if (!deliverable)
        return;

    int sig = ctz32(deliverable);  /* lowest pending signal */
    current->sig_pending &= ~(1u << sig);

    sighandler_t handler = current->sig_handlers[sig];

    if (handler == SIG_IGN)
        return;

    if (handler == SIG_DFL) {
        signal_default_action(sig, regs);
        return;
    }

    /* Save SSP register frame (restored after handler returns) */
    uint8_t saved_frame[SIGFRAME_SIZE];
    __builtin_memcpy(saved_frame, regs, SIGFRAME_SIZE);

    /* Block signal during handler to prevent infinite recursion */
    uint32_t old_blocked = current->sig_blocked;
    current->sig_blocked |= (1u << sig);

    /* Call handler synchronously with correct GOT base */
    m68k_call_signal_handler(handler, sig, current->got_base);

    /* Restore SSP frame and signal mask */
    __builtin_memcpy(regs, saved_frame, SIGFRAME_SIZE);
    current->sig_blocked = old_blocked;
}

#else /* ARM */

/*
 * Placed in kernel .text (flash XIP).  User-mode code can execute flash.
 * When the signal handler does bx lr, it lands here.
 * Uses SYS_RT_SIGRETURN (0x0605).
 */
__attribute__((naked, used, section(".text.sigreturn_trampoline")))
void sigreturn_trampoline(void)
{
    __asm volatile(
        "ldr  r7, =0x0605\n" /* SYS_RT_SIGRETURN */
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
static int signal_setup_frame(int sig, sighandler_t handler)
{
    uint32_t psp;
    __asm volatile("mrs %0, psp" : "=r"(psp));

    /* New HW frame goes 32 bytes below current PSP */
    uint32_t new_psp = psp - 32;

    /* Bounds check: don't write below the stack page */
    uint32_t stack_base = (uint32_t)(uintptr_t)current->stack_page;
    if (new_psp < stack_base)
        return -1;  /* stack overflow — cannot deliver signal */

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
    return 0;
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
        signal_default_action(sig, NULL);
        return;
    }

    /* User handler — set up trampoline frame on user stack */
    if (signal_setup_frame(sig, handler) < 0) {
        /* Stack overflow — cannot deliver signal, terminate process */
        sys_exit(128 + sig);
    }
}

#endif /* __m68k__ */

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

#if defined(__m68k__)

/* m68k uses synchronous signal delivery — no sigreturn needed.
 * These stubs exist for the syscall dispatch table. */
long sys_sigreturn(void)  { return -(long)ENOSYS; }
long sys_rt_sigreturn(void) { return -(long)ENOSYS; }

#else /* ARM */

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

long sys_rt_sigreturn(void)
{
    return sys_sigreturn();   /* same mechanism */
}

#endif /* __m68k__ */

/* ── sys_rt_sigaction ────────────────────────────────────────────────────── */
/*
 * musl's sigaction() calls rt_sigaction(sig, act, oact, sigsetsize).
 *
 * struct k_sigaction {
 *     void (*handler)(int);    // offset 0
 *     unsigned long sa_flags;  // offset 4
 *     void (*sa_restorer)(void); // offset 8
 *     unsigned long sa_mask[2];  // offset 12
 * };
 */
long sys_rt_sigaction(long sig, const void *act, void *oact, long sigsetsize)
{
    (void)sigsetsize;

    if (sig < 1 || sig >= NSIG)
        return -(long)EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -(long)EINVAL;

    /* Return old handler if requested */
    if (oact) {
        uint32_t *out = (uint32_t *)oact;
        out[0] = (uint32_t)(uintptr_t)current->sig_handlers[sig];
        out[1] = 0;   /* sa_flags */
        out[2] = 0;   /* sa_restorer */
        out[3] = 0;   /* sa_mask[0] */
        out[4] = 0;   /* sa_mask[1] */
    }

    /* Install new handler */
    if (act) {
        const uint32_t *in = (const uint32_t *)act;
        current->sig_handlers[sig] = (sighandler_t)(uintptr_t)in[0];
        /* sa_flags and sa_mask are noted but not fully supported yet */
    }

    return 0;
}

/* ── sys_rt_sigprocmask ──────────────────────────────────────────────────── */
/*
 * Manipulate the signal mask.
 *   how: SIG_BLOCK(0), SIG_UNBLOCK(1), SIG_SETMASK(2)
 *   set/oset: pointer to 64-bit signal mask (we use low 32 bits)
 */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

long sys_rt_sigprocmask(long how, const void *set, void *oset, long sigsetsize)
{
    (void)sigsetsize;

    /* Return current mask if requested */
    if (oset) {
        uint32_t *out = (uint32_t *)oset;
        out[0] = current->sig_blocked;
        if (sigsetsize >= 8)
            out[1] = 0;   /* high 32 bits — always 0 */
    }

    if (!set)
        return 0;

    uint32_t mask = *(const uint32_t *)set;

    switch (how) {
    case SIG_BLOCK:
        current->sig_blocked |= mask;
        break;
    case SIG_UNBLOCK:
        current->sig_blocked &= ~mask;
        break;
    case SIG_SETMASK:
        current->sig_blocked = mask;
        break;
    default:
        return -(long)EINVAL;
    }

    return 0;
}
