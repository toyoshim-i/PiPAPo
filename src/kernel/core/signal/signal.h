/*
 * signal.h — Signal constants, types, and kernel API
 *
 * Phase 3 signal infrastructure: POSIX-compatible signal numbers,
 * handler types (SIG_DFL, SIG_IGN, user function pointer), and
 * the signal_check() hook called from SVC_Handler on return to
 * user mode.
 */

#ifndef PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H
#define PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H

#include <stdint.h>

#include "common/signal.h"

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

/*
 * Called from the trap/SVC handler after syscall_dispatch(), when the
 * process is still RUNNABLE.  Checks sig_pending & ~sig_blocked; if
 * a signal is deliverable, dispatches it (SIG_IGN → discard, SIG_DFL →
 * terminate/ignore, user handler → set up trampoline frame).
 *
 * m68k: takes a pointer to the saved register frame (d0-d7/a0-a6 + SR + PC)
 *       on the supervisor stack so it can modify the return context.
 * i16:  takes the saved 24-byte interrupt frame and returns the stack
 *       pointer that trap.S should restore from.
 * ARM:  takes no args — manipulates PSP directly.
 */
#if defined(__m68k__)
void signal_check(uint32_t *regs);
#elif defined(__ia16__)
uint16_t signal_check(uint16_t user_sp, uint16_t user_ss);
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

/* Legacy kernel-text sigreturn trampoline symbol, kept only for m68k
 * where synchronous signal delivery needs the symbol to satisfy the
 * linker but never executes it.  ARM and ia16 use sa_restorer-style
 * delivery: the handler's return target is the per-process sa_restorer
 * registered via rt_sigaction (user-space stub — see each arch's
 * src/arch/<arch>/user/syscall.S). */
#if defined(__m68k__)
extern void sigreturn_trampoline(void);
#endif

#endif /* PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H */
