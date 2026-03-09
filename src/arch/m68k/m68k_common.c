/*
 * m68k_common.c — Shared m68k architecture state and fault handling
 *
 * Provides:
 *   - Context switch pending flag
 *   - Crash handler: prints register dump, kills faulting user process
 */

#include <stdint.h>
#include "../kernel/proc/proc.h"
#include "../kernel/klog.h"

/* Context switch pending flag.
 * Set by arch_yield() (via sched_tick or sched_yield).
 * Checked by timer ISR before RTE to perform the switch. */
volatile uint32_t m68k_switch_pending = 0;

/* ── Crash handler ─────────────────────────────────────────────────────── *
 *
 * Called from boot.S fault handlers with:
 *   fault_type: 68000 vector number (2=bus, 3=addr, 4=illegal, 5=zerodiv,
 *               6=CHK, 7=TRAPV, 8=privilege)
 *   regs:       pointer to saved d0-d7/a0-a6 (60 bytes), followed by
 *               the 68000 exception frame (group 0: 14 bytes, group 1: 6 bytes)
 *
 * Prints a crash report with signal name, PC, fault address, and full
 * register dump.  If the faulting process is a user process (pid > 0),
 * kills it and returns 1 (caller reschedules).  If pid == 0 (kernel),
 * returns 0 (caller halts).
 */

/* Forward declarations */
long sys_exit(long status);
long sys_kill(long pid, long sig);

/* POSIX signal numbers */
#define SIGILL  4
#define SIGBUS  7
#define SIGFPE  8
#define SIGSEGV 11
#define SIGABRT 6

/* Fault type → description and POSIX signal */
static const char *fault_name(int fault_type)
{
    switch (fault_type) {
    case 2:  return "SIGBUS (bus error)";
    case 3:  return "SIGBUS (address error)";
    case 4:  return "SIGILL (illegal instruction)";
    case 5:  return "SIGFPE (divide by zero)";
    case 6:  return "SIGFPE (CHK)";
    case 7:  return "SIGFPE (TRAPV)";
    case 8:  return "SIGSEGV (privilege violation)";
    default: return "SIG??? (unknown)";
    }
}

static int fault_signal(int fault_type)
{
    switch (fault_type) {
    case 2: case 3:  return SIGBUS;
    case 4:          return SIGILL;
    case 5: case 6: case 7: return SIGFPE;
    case 8:          return SIGSEGV;
    default:         return SIGABRT;
    }
}

/*
 * m68k_crash_handler — called from boot.S fault handlers
 *
 *   fault_type: 68000 vector number (2-8)
 *   regs:       saved d0-d7/a0-a6 (60 bytes), followed by exception frame
 *
 * Prints a crash report.  For user processes, delivers the corresponding
 * POSIX signal.  If the process has a handler installed, the signal is
 * posted (the process gets a chance to handle it on next signal_check).
 * If no handler (SIG_DFL), the process is killed immediately.
 *
 * Returns: 1 = process killed, caller should reschedule
 *          0 = kernel fault, caller should halt
 */
int m68k_crash_handler(int fault_type, uint32_t *regs)
{
    uint16_t *exc = (uint16_t *)((uint8_t *)regs + 60);
    uint32_t pc, fault_addr = 0;
    uint16_t sr;
    int is_group0 = (fault_type == 2 || fault_type == 3);

    if (is_group0) {
        fault_addr = ((uint32_t)exc[1] << 16) | exc[2];
        sr = exc[4];
        pc = ((uint32_t)exc[5] << 16) | exc[6];
    } else {
        sr = exc[0];
        pc = ((uint32_t)exc[1] << 16) | exc[2];
    }

    pcb_t *p = current;
    int sig = fault_signal(fault_type);

    /* Print crash report (klogf supports: %s %u %x %% only) */
    klogf("\n*** %s ***", fault_name(fault_type));
    if (p)
        klogf("  Process %u (%s)", (uint32_t)p->pid, p->comm);
    klogf("  PC=%x  SR=%x", pc, (uint32_t)sr);
    if (is_group0)
        klogf("  Fault addr=%x  FC=%x", fault_addr, (uint32_t)exc[0]);
    klogf("  d0=%x d1=%x d2=%x d3=%x",
          regs[0], regs[1], regs[2], regs[3]);
    klogf("  d4=%x d5=%x d6=%x d7=%x",
          regs[4], regs[5], regs[6], regs[7]);
    klogf("  a0=%x a1=%x a2=%x a3=%x",
          regs[8], regs[9], regs[10], regs[11]);
    klogf("  a4=%x a5=%x a6=%x",
          regs[12], regs[13], regs[14]);

    /* Kernel fault → unrecoverable */
    if (!p || p->pid == 0) {
        klogf("  Kernel fault — halting.");
        return 0;
    }

    /* Check if the process has a signal handler installed.
     * If so, post the signal and let signal_check() deliver it
     * on the next return to user-mode.  For now on m68k, signal
     * delivery is not yet implemented, so we fall through to kill.
     * TODO: when m68k signal delivery is implemented, return the
     * saved context to the process with the signal pending. */
    sighandler_t handler = p->sig_handlers[sig];
    if (handler != (sighandler_t)0 /* SIG_DFL */ &&
        handler != (sighandler_t)1 /* SIG_IGN */) {
        klogf("  Signal %u posted (handler at %x)",
              (uint32_t)sig, (uint32_t)(uintptr_t)handler);
        p->sig_pending |= (1u << sig);
        /* TODO: once m68k signal_check() can deliver signals,
         * return here and let the process resume with the signal
         * pending.  For now, fall through to kill. */
    }

    klogf("  Killed (exit status %u)", (uint32_t)(128 + sig));
    sys_exit(128 + sig);
    return 1;
}
