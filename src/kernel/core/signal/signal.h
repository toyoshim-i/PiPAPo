/*
 * signal.h — Architecture-independent signal core API
 *
 * POSIX-compatible signal numbers, handler types (SIG_DFL, SIG_IGN,
 * user function pointer), and the shared helpers that every per-arch
 * `src/arch/<arch>/kernel/core/signal/signal_check.c` calls on the
 * trap-return path.  Each arch's `signal_check` entry point is
 * declared in its own per-arch `signal_check.h` (paired with its
 * `signal_check.c`), reached via `kernel/core/signal/signal_check.h`
 * through the arch overlay.
 */

#ifndef PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H
#define PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H

#include <stdint.h>

/* Re-exported so callers of this header get the SIG* numbers and NSIG
 * alongside the sighandler_t / SIG_DFL / SIG_IGN API.  Not strictly
 * needed by the declarations in this file, but the umbrella matches
 * POSIX <signal.h> expectations. */
#include "common/signal.h"

typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

/*
 * Apply the default disposition for `sig`:
 *   - SIGCHLD: ignore (returns 1).
 *   - Subsystem-handled (Human68k / CP/M / S-OS): defer to subsys op
 *     (returns 1 if the subsys handled it).
 *   - Otherwise: terminate the process via sys_exit(128 + sig); does
 *     not return.
 *
 * `regs` is forwarded to the subsys hook for arches that pass a
 * saved-register frame (m68k / riscv); pass NULL on arches that
 * don't (arm_m / ia16 / xtensa).
 */
int signal_default_action(int sig, uint32_t *regs);

/*
 * Pop one deliverable pending signal and apply the trivial
 * dispositions (SIG_IGN discard, SIG_DFL via signal_default_action).
 * Intended to run at the top of every per-arch signal_check.
 *
 * Returns:
 *   0 — no further action.  Either no signal was deliverable, the
 *       handler was SIG_IGN (consumed, discard), or SIG_DFL ran
 *       (subsys hook handled it, or sys_exit did not return).
 *   1 — `*sig_out` holds the popped signal number and the handler at
 *       current->sig_handlers[*sig_out] is a user function pointer.
 *       Caller builds the arch-specific delivery frame.
 *
 * `regs` is forwarded to signal_default_action; see its doc.
 */
int signal_pop_pending(int *sig_out, uint32_t *regs);

/*
 * Deliver one pending signal from kernel context for emulator-backed
 * tasks.  Supports only the dispositions that survive execve():
 *   - SIG_IGN: discard.
 *   - SIG_DFL: SIGCHLD ignored, others terminate (subsys hook may
 *     claim it first).
 *
 * Returns 0 if no signal was pending, 1 if a pending signal was
 * consumed.  If the default action terminates the process, this
 * function does not return.
 */
int signal_check_kernel(void);

/* All arches use sa_restorer-style delivery: the handler's return
 * target is the per-process sa_restorer registered via rt_sigaction
 * (user-space stub — see each arch's src/arch/<arch>/user/syscall.S).
 * No kernel-owned trampoline symbol exists anymore. */

#endif /* PPAP_KERNEL_CORE_SIGNAL_SIGNAL_H */
