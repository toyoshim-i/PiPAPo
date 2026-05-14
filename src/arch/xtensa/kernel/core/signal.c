/*
 * signal.c — Xtensa LX7 signal delivery (stub)
 *
 * CC-3 will wire real user-handler delivery once the trap glue passes a
 * saved register frame here.  Today this just consumes pending signals
 * with their SIG_IGN / SIG_DFL dispositions:
 *
 *   - SIG_IGN: clear pending bit.
 *   - SIG_DFL: subsys hook or terminate via sys_exit (signal_default_action).
 *   - User handler: silently dropped — sig_pending stays cleared, but the
 *     handler does not run.  Acceptable while no Xtensa user program
 *     installs handlers; revisit when CC-3 builds the delivery frame.
 *
 * trap.S does not call signal_check yet; the symbol exists so the
 * sigaction-family syscalls link.
 */

#include "kernel/core/signal/signal.h"

#include <stdint.h>

#include "common/errno.h"
#include "kernel/core/signal/signal_helper.h"

void signal_check(void) {
  int sig;
  (void)signal_pop_pending(&sig, NULL);
  /* User-handler delivery deferred to CC-3. */
}

long sys_rt_sigreturn(void) { return -(long)ENOSYS; }
