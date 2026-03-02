/*
 * signal.h — Signal constants, types, and kernel API
 *
 * Phase 3 signal infrastructure: POSIX-compatible signal numbers,
 * handler types (SIG_DFL, SIG_IGN, user function pointer), and
 * the signal_check() hook called from SVC_Handler on return to
 * user mode.
 */

#ifndef PPAP_SIGNAL_H
#define PPAP_SIGNAL_H

#include <stdint.h>

#define NSIG      32

typedef void (*sighandler_t)(int);
#define SIG_DFL   ((sighandler_t)0)
#define SIG_IGN   ((sighandler_t)1)

/* Signal numbers (Linux ARM compatible) */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGKILL    9
#define SIGUSR1   10
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19

/*
 * Called from SVC_Handler (svc.S) after syscall_dispatch(), when the
 * process is still RUNNABLE.  Checks sig_pending & ~sig_blocked; if
 * a signal is deliverable, dispatches it (SIG_IGN → discard, SIG_DFL →
 * terminate/ignore, user handler → set up trampoline frame on PSP).
 */
void signal_check(void);

/* Trampoline in kernel .text (flash XIP) — signal handler returns here
 * via bx lr, which triggers SVC SYS_SIGRETURN to restore context. */
extern void sigreturn_trampoline(void);

#endif /* PPAP_SIGNAL_H */
