/*
 * signal.h — Signal constants, types, and kernel API
 *
 * Phase 3 signal infrastructure: POSIX-compatible signal numbers,
 * handler types (SIG_DFL, SIG_IGN, user function pointer), and
 * the signal_check() hook called from SVC_Handler on return to
 * user mode.
 */

#ifndef PPAP_KERNEL_SIGNAL_SIGNAL_H
#define PPAP_KERNEL_SIGNAL_SIGNAL_H

#include <stdint.h>

#define NSIG      32

typedef void (*sighandler_t)(int);
#define SIG_DFL   ((sighandler_t)0)
#define SIG_IGN   ((sighandler_t)1)

/* Signal numbers (Linux ARM compatible) */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGTRAP    5
#define SIGKILL    9
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19

/*
 * Called from the trap/SVC handler after syscall_dispatch(), when the
 * process is still RUNNABLE.  Checks sig_pending & ~sig_blocked; if
 * a signal is deliverable, dispatches it (SIG_IGN → discard, SIG_DFL →
 * terminate/ignore, user handler → set up trampoline frame).
 *
 * m68k: takes a pointer to the saved register frame (d0-d7/a0-a6 + SR + PC)
 *       on the supervisor stack so it can modify the return context.
 * ARM:  takes no args — manipulates PSP directly.
 */
#if defined(__m68k__)
void signal_check(uint32_t *regs);
#else
void signal_check(void);
#endif

/*
 * Deliver one pending signal from kernel context for emulator-backed tasks.
 *
 * This path supports the dispositions that survive execve():
 *   - SIG_IGN: discard
 *   - SIG_DFL: ignore SIGCHLD, terminate on others
 *
 * Returns 0 if no signal was pending, 1 if a pending signal was consumed.
 * If the default action terminates the process, this function does not return.
 */
int signal_check_kernel(void);

/* Trampoline in kernel .text (flash XIP) — signal handler returns here
 * via bx lr (ARM) which triggers SVC SYS_SIGRETURN to restore context.
 * On m68k this is a stub (synchronous delivery, no trampoline needed). */
extern void sigreturn_trampoline(void);

#endif /* PPAP_KERNEL_SIGNAL_SIGNAL_H */
